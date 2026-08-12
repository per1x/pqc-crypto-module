// sha3_core —— Keccak 海绵结构（padding / 吸收 / 挤压）搬进 PL
//
// 【这一步补的是什么】
// 仓库里原本只有 keccak_f1600 置换核，海绵的 framing 全在 C 里做
// （src/hal/pqc_accel.c 的 accel_shake）。那个分工在当时是对的：核更小，
// SHAKE128/256 与 SHA3-256/512 共用同一个置换。但代价是**每置换一次就要
// 过一次总线**：PS 写 200 字节状态 → 触发 → 读回 200 字节。
// 一次 SHAKE256 吸收 32 字节要搬 400 字节总线数据，去做 24 个周期的运算。
//
// 把海绵挪进 PL 之后，总线上走的是消息和摘要本身，中间状态一次也不出芯片。
// 除了快，还有一条更要紧的：**中间状态不再离开密码边界**。
// SHAKE 的中间状态在 ML-KEM 里就是 ρ/σ 展开的上下文，让它反复出入普通世界
// 可寻址的缓冲区，在真密码机里是说不过去的。
//
// 【接口：字节流，两边都有背压】
//   start        脉冲，复位海绵并锁存 rate/suffix
//   in_valid/in_ready/in_data   消息字节流，1 字节/周期，吸收期间从不停顿
//   in_flush     脉冲，宣告消息结束（**只在 in_valid 为低时被采样**）
//   out_valid/out_ready/out_data 摘要字节流，按需挤压
//
// 挤压**不设长度寄存器**：消费方读够了就不读了。SHAKE 的输出长度本来就是
// 任意的，设长度寄存器等于凭空给它加一个上限，还多一个要校验的参数。
// 需要固定长度（SHA3-256/512）时由调用方数够字节数即可。
//
// 空消息（SHA3-256("")）能正常处理：start 之后直接 in_flush。
// 这不是边角料 —— 它是 FIPS 202 的第一条测试向量。
//
// 【吸收为什么是 1 字节/周期而不是 1 lane/周期】
// keccak_f1600 的读口是组合的，写口在 !busy 时接受写入。于是"读出 lane →
// 异或进一个字节 → 写回"整个是一条组合路径，一个周期就能做完，不需要先
// 攒够 8 个字节再提交。攒 lane 的写法要处理"不足 8 字节的尾巴怎么对齐"，
// 而按字节做连这个问题都不存在 —— pad 的位置计算也跟着一起消失了。
// 代价是每个 rate 块多花几十个周期，但那本来就被 24 周期的置换摊薄了。
//
// 【padding 只要两次读-改-写】
// pad10*1 是：消息 ‖ suffix ‖ 0…0 ‖ 0x80(在 rate-1 处)。中间那串 0 异或进去
// 是空操作，可以整个跳过。所以只剩两笔：
//   ① 在当前位置异或 suffix；② 在 rate-1 处异或 0x80。
// 两笔顺序执行，所以 suffix 恰好落在 rate-1 时（消息长度 ≡ rate-1）
// 自然会合并成 suffix^0x80，不需要单独判这个边角情况。
// rate 是 8 的倍数（168/136/72 都是），所以 rate-1 永远是某个 lane 的最高字节。
`default_nettype none

module sha3_core (
    input  wire        clk,
    input  wire        rst_n,

    // ---- 控制 ----
    // rate_bytes：SHAKE128=168 SHAKE256/SHA3-256=136 SHA3-512=72
    // suffix    ：SHAKE=0x1F  SHA3=0x06
    input  wire [7:0]  rate_bytes,
    input  wire [7:0]  suffix,
    input  wire        start,      // 脉冲：清空海绵、锁存参数
    input  wire        zeroize,    // 脉冲：清空状态并回到空闲（密码边界擦除）

    // ---- 消息入口 ----
    input  wire        in_valid,
    output wire        in_ready,
    input  wire [7:0]  in_data,
    input  wire        in_flush,   // 只在 in_valid 为低时被采样

    // ---- 摘要出口 ----
    output wire        out_valid,
    input  wire        out_ready,
    output wire [7:0]  out_data,

    // ---- 状态观测 ----
    output wire        busy,
    output wire        absorbing,
    output wire        squeezing
);
    localparam [3:0] S_IDLE      = 4'd0,
                     S_CLR       = 4'd1,
                     S_ABSORB    = 4'd2,
                     S_PAD_SUF   = 4'd3,
                     S_PAD_LAST  = 4'd4,
                     S_PERM_KICK = 4'd5,
                     S_PERM_WAIT = 4'd6,
                     S_SQUEEZE   = 4'd7,
                     S_ZWAIT     = 4'd8;

    // 清空之后回到哪：正常 start 是去吸收，zeroize 是回空闲
    localparam [1:0] R_ABSORB = 2'd0, R_SQUEEZE = 2'd1, R_IDLE = 2'd2;

    reg [3:0] state;
    reg [1:0] ret;

    reg [7:0] rate_r;      // 锁存的 rate，单位字节
    reg [7:0] suffix_r;
    reg [7:0] bpos;        // 当前 rate 块内的字节位置（吸收）
    reg [7:0] sq_bpos;     // 当前 rate 块内的字节位置（挤压）
    reg [4:0] clr_idx;

    // ---- 置换核 ----
    reg         kec_start;
    wire        kec_done;
    reg         kec_wr_en;
    reg  [4:0]  kec_wr_addr;
    reg  [63:0] kec_wr_data;
    reg  [4:0]  kec_rd_addr;
    wire [63:0] kec_rd_data;

    keccak_f1600 u_kec (
        .clk(clk), .rst_n(rst_n),
        .start(kec_start), .done(kec_done),
        .wr_en(kec_wr_en), .wr_addr(kec_wr_addr), .wr_data(kec_wr_data),
        .rd_addr(kec_rd_addr), .rd_data(kec_rd_data));

    // 最后一个 lane 的下标：rate-1 落在这里，且一定是它的最高字节
    wire [4:0] last_lane = rate_r[7:3] - 5'd1;

    // ---- 读地址：按当前阶段选 ----
    always @(*) begin
        case (state)
        S_SQUEEZE:  kec_rd_addr = sq_bpos[7:3];
        S_PAD_LAST: kec_rd_addr = last_lane;
        default:    kec_rd_addr = bpos[7:3];
        endcase
    end

    // ---- 写口：读-改-写异或注入 ----
    // 三个注入点共用同一条组合路径，区别只在异或什么、异或到哪个字节位置。
    wire [63:0] xor_msg = {56'd0, in_data}  << (8 * bpos[2:0]);
    wire [63:0] xor_suf = {56'd0, suffix_r} << (8 * bpos[2:0]);
    wire [63:0] xor_end = 64'h8000_0000_0000_0000;   // 0x80 在 lane 的最高字节

    always @(*) begin
        kec_wr_en   = 1'b0;
        kec_wr_addr = kec_rd_addr;
        kec_wr_data = 64'd0;
        case (state)
        S_CLR: begin
            kec_wr_en   = 1'b1;
            kec_wr_addr = clr_idx;
            kec_wr_data = 64'd0;
        end
        S_ABSORB: begin
            kec_wr_en   = in_valid;
            kec_wr_data = kec_rd_data ^ xor_msg;
        end
        S_PAD_SUF: begin
            kec_wr_en   = 1'b1;
            kec_wr_data = kec_rd_data ^ xor_suf;
        end
        S_PAD_LAST: begin
            kec_wr_en   = 1'b1;
            kec_wr_data = kec_rd_data ^ xor_end;
        end
        default: ;
        endcase
    end

    // ---- 握手 ----
    assign in_ready  = (state == S_ABSORB);
    assign out_valid = (state == S_SQUEEZE);
    assign out_data  = kec_rd_data[8 * sq_bpos[2:0] +: 8];

    assign busy      = (state != S_IDLE);
    assign absorbing = (state == S_ABSORB);
    assign squeezing = (state == S_SQUEEZE);

    // 置换是否正在进行。**必须自己记**，因为 keccak_f1600 不往外给 busy，
    // 而它在 busy 期间会**默默丢弃写入**。zeroize 若在置换途中把 25 个 lane
    // 写 0，写入会全部落空，状态机却照样走完 S_CLR —— 表面清干净了，
    // 海绵里的消息还原封不动。这是那种不会报错的错，所以要挡在前面。
    reg perm_busy;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            perm_busy <= 1'b0;
        end else if (kec_start) begin
            perm_busy <= 1'b1;
        end else if (kec_done) begin
            perm_busy <= 1'b0;
        end
    end

    // zeroize 取上升沿。取电平的话，tamper 这种一直拉高的信号会把状态机
    // 永远摁在等待里，反而清不掉 —— 与 trng_top 里 fifo_flush 用边沿是
    // 同一个理由。
    reg zeroize_d;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            zeroize_d <= 1'b0;
        end else begin
            zeroize_d <= zeroize;
        end
    end
    wire zeroize_rise = zeroize && !zeroize_d;

    // 与 trng_cond 同一个坑：start 打进去之后，置换核的 busy 要下一拍才拉高，
    // 而 done 是电平、会保持到下一次 start。所以踢和等必须分成两个状态，
    // 否则等待状态会立刻看见上一次遗留的 done。
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state     <= S_IDLE;
            ret       <= R_ABSORB;
            rate_r    <= 8'd136;
            suffix_r  <= 8'h1F;
            bpos      <= 8'd0;
            sq_bpos   <= 8'd0;
            clr_idx   <= 5'd0;
            kec_start <= 1'b0;
        end else begin
            kec_start <= 1'b0;

            // start 与 zeroize 都是**从任何状态**都生效的，不只是空闲时。
            //
            // 挤压是没有自然终点的：SHAKE 的输出长度任意，核心无从知道消费方
            // 什么时候读够了，所以挤完最后一块也不会自己回到空闲。若 start 只
            // 在空闲时才认，第一次操作之后核心就永远停在 S_SQUEEZE，第二次
            // 操作根本发不动 —— 驱动那边看到的是"写了 start 之后 in_ready
            // 永远不来"，一个不会报错、只会挂死的行为。
            //
            // 所以 start 的语义定成"丢掉手头的一切，从干净海绵重新开始"。
            // 反正 S_CLR 本来就要把 25 个 lane 写 0，重启和擦除走同一条路。
            if (start || zeroize_rise) begin
                state   <= S_ZWAIT;
                // start 优先：既写 start 又拉 zeroize 时按启动新操作处理
                ret     <= start ? R_ABSORB : R_IDLE;
                clr_idx <= 5'd0;
                bpos    <= 8'd0;
                sq_bpos <= 8'd0;
                if (start) begin
                    rate_r   <= rate_bytes;
                    suffix_r <= suffix;
                end
            end else begin
                case (state)
                S_IDLE: ;   // 等 start

                // 等在飞的置换落地，否则下面的写 0 会被静默丢掉
                S_ZWAIT: begin
                    if (!perm_busy) begin
                        state <= S_CLR;
                    end
                end

                S_CLR: begin
                    if (clr_idx == 5'd24) begin
                        clr_idx <= 5'd0;
                        state   <= (ret == R_IDLE) ? S_IDLE : S_ABSORB;
                    end else begin
                        clr_idx <= clr_idx + 5'd1;
                    end
                end

                S_ABSORB: begin
                    if (in_valid) begin
                        if (bpos + 8'd1 == rate_r) begin
                            bpos  <= 8'd0;
                            ret   <= R_ABSORB;
                            state <= S_PERM_KICK;
                        end else begin
                            bpos <= bpos + 8'd1;
                        end
                    end else if (in_flush) begin
                        // flush 只在 in_valid 为低时采样，所以这里不会与
                        // 上面那支同时成立
                        state <= S_PAD_SUF;
                    end
                end

                S_PAD_SUF: begin
                    state <= S_PAD_LAST;
                end

                S_PAD_LAST: begin
                    bpos    <= 8'd0;
                    sq_bpos <= 8'd0;
                    ret     <= R_SQUEEZE;
                    state   <= S_PERM_KICK;
                end

                S_PERM_KICK: begin
                    kec_start <= 1'b1;
                    state     <= S_PERM_WAIT;
                end

                // 判完成必须用 perm_busy 把 kec_done **限定在本次置换**上。
                //
                // keccak_f1600 的 done 是电平，会一直保持到下一次 start。
                // 进入本状态的头一拍，kec_start 才刚发出去（置换还没开始），
                // 而 done 上挂着的还是**上一次**置换留下的 1 —— 只看 kec_done
                // 会当场判定"已完成"，于是在置换进行当中就开始挤压。
                //
                // 这个 bug 的表现极具迷惑性：A 阵列在 24 轮里每拍都在变，
                // 挤出来的头十几个字节取自中间轮的状态，后面的字节等置换跑完
                // 才取、反而是对的。于是摘要"前面错、后面对"，看着像位序或
                // 对齐问题，其实是时序问题。第一次操作还恰好是对的（复位后
                // done=0，没有陈旧值），非要连算两次才暴露 —— 所以
                // test_back_to_back 那条用例不是锦上添花。
                S_PERM_WAIT: begin
                    if (perm_busy && kec_done) begin
                        state <= (ret == R_SQUEEZE) ? S_SQUEEZE : S_ABSORB;
                    end
                end

                S_SQUEEZE: begin
                    if (out_ready) begin
                        if (sq_bpos + 8'd1 == rate_r) begin
                            sq_bpos <= 8'd0;
                            ret     <= R_SQUEEZE;
                            state   <= S_PERM_KICK;
                        end else begin
                            sq_bpos <= sq_bpos + 8'd1;
                        end
                    end
                end

                default: state <= S_IDLE;
                endcase
            end
        end
    end

`ifndef SYNTHESIS
    // rate 必须是 8 的倍数且不超过 200 —— pad 的位置计算全建立在这上面。
    // 参数写错时状态机不会报错，只会安静地算出错误的摘要，所以在这里拦。
    always @(posedge clk) begin
        if (rst_n && start) begin
            if (rate_bytes[2:0] != 3'd0 || rate_bytes == 8'd0 || rate_bytes > 8'd200) begin
                $display("[sha3_core] 非法 rate=%0d：必须是 8 的倍数且在 1..200", rate_bytes);
                $finish;
            end
        end
        if (rst_n && in_valid && in_flush) begin
            $display("[sha3_core] in_valid 与 in_flush 不得同时拉高");
            $finish;
        end
    end
`endif

endmodule

`default_nettype wire
