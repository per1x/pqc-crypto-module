// ML-DSA 的两个多项式采样器：给种子和 nonce，出一条 256 系数的多项式
//
// ============================================================================
// 【这一层解决什么】
// ============================================================================
// sample.v 里那两个原语（mldsa_rej_uniform / mldsa_rej_eta）是**纯组合**的：
// 给几个字节，回一个候选值和"收不收"。它们不管字节从哪来、也不管攒够 256 个
// 系数没有。而真正麻烦的恰恰是外面这一圈：
//
//   · 驱动 sha3_core 吸收头部、冲刷、然后一直抽；
//   · 把抽出来的字节按 3 个（uniform）或半个（eta）切给原语；
//   · 被拒的候选**不占位置**，所以要多抽多少字节事先不知道；
//   · 攒够 256 个就停，且**不能再多抽一个字节** —— 多抽不会改变结果，
//     但会让 sha3_core 停在一个和黄金模型不同的位置上，
//     以后想复用同一个海绵接着抽别的东西时就对不上了。
//
// 把这一圈封在这里，KeyGen/Sign 的 FSM 就只需要"给种子、等 done、读系数"。
//
// ============================================================================
// 【为什么两个采样器分开写而不是参数化成一个】
// ============================================================================
// 它们只有"驱动 SHAKE"这一段像，剩下的全不一样：
//   · uniform 用 SHAKE128（rate 168），一次吃 3 字节出 1 个候选；
//   · eta 用 SHAKE256（rate 136），一次吃 1 字节出 **2** 个候选（高低半字节），
//     而且第二个候选可能因为已经攒够 256 个而被丢掉。
// 硬凑成一个模块会得到一堆 if (MODE==...) 的分支，比两份代码更难看懂。
//
// ============================================================================
// 【头部字节序：ρ‖nonce，nonce 是小端 16 位】
// ============================================================================
// FIPS 204 里 A 的头是 ρ‖IntegerToBytes(256·r + s, 2)，s₁/s₂ 的头是
// ρ'‖IntegerToBytes(nonce, 2)，两者都是**小端两字节**。写反的话采出来的
// 多项式完全合法、分布也对，只是和标准的不一样 —— 只有对着向量比才发现。
// 所以这里 nonce 低字节先送，和 mldsa_oracle.py 里那两行一致。
`default_nettype none

// ---------------------------------------------------------------------------
// RejNTTPoly：SHAKE128 → [0, q) 上均匀的 256 个系数
// ---------------------------------------------------------------------------
module mldsa_poly_uniform (
    input  wire        clk,
    input  wire        rst_n,

    input  wire        start,          // 脉冲
    input  wire [255:0] seed,          // ρ，32 字节；seed[7:0] 是第 0 字节
    input  wire [15:0] nonce,          // 256·r + s
    output reg         done,

    // ---- 挂到外部 sha3_core（KeyGen 全程只有一个海绵，见 keygen 的说明）----
    output reg         sha_start,
    output wire [7:0]  sha_rate,
    output wire [7:0]  sha_suffix,
    output reg         sha_in_valid,
    output reg  [7:0]  sha_in_data,
    output reg         sha_in_flush,
    input  wire        sha_in_ready,
    input  wire        sha_out_valid,
    output wire        sha_out_ready,
    input  wire [7:0]  sha_out_data,

    // ---- 结果：done 之后按下标读 ----
    input  wire [7:0]  rd_addr,
    output wire [22:0] rd_data,
    output wire [8:0]  count
);
    localparam [7:0] RATE = 8'd168, SUFFIX = 8'h1F;   // SHAKE128

    /* ⚠️ **必须用连续赋值，不能写成 `always @(*) sha_rate = RATE;`。**
     *
     * 那样写的话右边全是常量，**敏感列表是空的** —— 这个块永远不会被触发，
     * 两个输出一直保持 X。而 sha3_core 是在 start 那一拍锁存 rate/suffix 的，
     * 于是它拿到 X：补位那两拍写到一个 X 地址上，海绵状态里多出一个游离的
     * X 字节、收尾的 0x80 又没写进去，置换一跑整个状态全变 X。
     *
     * 表现是"采样一直不结束"，而 sha3_core 单独跑 9/9 全过 —— 因为它自己的
     * 测试台是显式驱动 rate/suffix 的。**故障在驱动侧，症状在被驱动侧。** */
    assign sha_rate   = RATE;
    assign sha_suffix = SUFFIX;

    localparam [2:0] S_IDLE = 3'd0, S_GAP = 3'd1, S_ABS = 3'd2,
                     S_FLUSH = 3'd3, S_RUN = 3'd4, S_DONE = 3'd5;
    reg [2:0]  st;
    reg [5:0]  hdr_i;        // 头部送到第几字节（0..33）
    reg [1:0]  bcnt;         // 攒了几个字节
    reg [15:0] bbuf;         // 攒着的低两字节

    // 头部：ρ 的 32 字节，然后 nonce 低字节、高字节
    wire [5:0] hdr_nxt_i = hdr_i + 6'd1;
    wire [7:0] hdr_byte = (hdr_i < 6'd32) ? seed[hdr_i*8 +: 8]
                        : (hdr_i == 6'd32) ? nonce[7:0]
                                           : nonce[15:8];
    /* 握手那一拍要装的是**下一个**字节。用 hdr_i 的话会把当前字节再送一遍 ——
     * 因为 hdr_i 是在同一拍自增的，非阻塞赋值下 hdr_byte 还是旧值。
     * 症状：第 0 个字节进了两次、整条头部往后错一位，最后一个字节没送出去。
     * 采出来的多项式完全合法，只是和标准的不一样 —— 只有对着向量比才发现。 */
    wire [7:0] hdr_byte_nxt = (hdr_nxt_i < 6'd32) ? seed[hdr_nxt_i*8 +: 8]
                            : (hdr_nxt_i == 6'd32) ? nonce[7:0]
                                                   : nonce[15:8];

    wire [23:0] triple = {sha_out_data, bbuf};
    wire [22:0] cand;
    wire        cand_ok;
    mldsa_rej_uniform u_rej (.bytes_in(triple), .cand(cand), .cand_ok(cand_ok));

    reg  [8:0] n;            // 已经收下的系数个数
    reg        we;
    reg [7:0]  waddr;
    /* ⚠️ 写进 BRAM 的值必须**跟着 we 一起打进寄存器**。
     * we/waddr 是寄存器、下一拍才生效，而 cand 是从 sha_out_data 组合出来的 ——
     * 到写生效的那一拍，字节流已经翻过去了，存进去的是**下一个**候选的低位。
     * 症状：判据（收不收）是对的、个数也对，唯独存下来的值全错，
     * 而且第 0 个就错，看着像位序或字节序搞反了。 */
    reg [22:0] wdata;
    assign count = n;

    // ⚠️ **out_ready 必须是组合的。**
    //    第一版把它做成寄存器（`sha_out_ready <= 1'b1`），于是"我决定要收"
    //    和"核真的把这个字节交出来"差了一拍 —— 我在核还没握手的拍次去读
    //    sha_out_data，读到的是 X。X 传进 cand_ok，`if (cand_ok)` 在
    //    Icarus 里当假走掉，**一个系数都收不下**，表现是"采样一直没结束"，
    //    而现象看起来完全像是拒绝采样不收敛。
    //
    //    改成组合 ready + 只在同一拍 valid&&ready 时消费，两边就对齐了。
    assign sha_out_ready = (st == S_RUN) && (n < 9'd256);
    wire byte_fire = sha_out_valid && sha_out_ready;

    // 256 × 23 位。用 ram_dp 而不是寄存器阵列：23×256 = 5888 个触发器，
    // 放进一块 BRAM 是白捡的。
    ram_dp #(.DW(23), .AW(8)) u_mem (
        .clk(clk),
        .a_we(we), .a_addr(waddr), .a_din(wdata), .a_dout(),
        .b_we(1'b0), .b_addr(rd_addr), .b_din(23'd0), .b_dout(rd_data));

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            st <= S_IDLE; done <= 1'b0; n <= 9'd0;
            hdr_i <= 6'd0; bcnt <= 2'd0; bbuf <= 16'd0;
            sha_start <= 1'b0; sha_in_valid <= 1'b0; sha_in_flush <= 1'b0;
            sha_in_data <= 8'd0;
            we <= 1'b0; waddr <= 8'd0; wdata <= 23'd0;
        end else begin
            sha_start <= 1'b0;
            sha_in_valid <= 1'b0;
            sha_in_flush <= 1'b0;
            we <= 1'b0;
            done <= 1'b0;

            case (st)
            S_IDLE: if (start) begin
                n <= 9'd0; hdr_i <= 6'd0; bcnt <= 2'd0;
                sha_start <= 1'b1;
                st <= S_ABS;
            end

            // 握手要**按握上的那一拍**推进，而不是"上一拍看到 ready 就发"。
            // 后者在 ready 中途落下时会丢字节，而丢掉的是头部里的某一个 ——
            // 采出来的多项式仍然完全合法，只是和标准的不一样。
            S_ABS: begin
                sha_in_valid <= 1'b1;
                if (sha_in_valid && sha_in_ready) begin
                    if (hdr_i == 6'd33) begin
                        sha_in_valid <= 1'b0;
                        st <= S_GAP;
                    end else begin
                        hdr_i        <= hdr_nxt_i;
                        sha_in_data  <= hdr_byte_nxt;
                    end
                end else begin
                    sha_in_data <= hdr_byte;   /* 还没握上，保持当前字节 */
                end
            end

            // ⚠️ **必须空一拍再冲刷。** sha_in_valid 是寄存器，最后一个字节的
            //    有效电平会拖到下一拍；而 sha3_core 明说 in_flush 只在
            //    in_valid 为低时被采样 —— 同一拍拉 flush 会被整个忽略，
            //    海绵永远吸不完，表现是"采样一直没结束"，
            //    而看现象完全像是拒绝采样的收敛出了问题。
            S_GAP: st <= S_FLUSH;

            S_FLUSH: begin
                sha_in_flush <= 1'b1;
                st <= S_RUN;
            end

            S_RUN: begin
                // 攒够 256 个就**立刻停止抽取** —— 多抽一个字节不会改变
                // 这条多项式，但会让海绵停在与黄金模型不同的位置上。
                if (n >= 9'd256) begin
                    st <= S_DONE;
                end else if (byte_fire) begin
                    if (bcnt == 2'd2) begin
                        bcnt <= 2'd0;
                        // 第三个字节到位，这一拍就判收不收
                        if (cand_ok) begin
                            we    <= 1'b1;
                            waddr <= n[7:0];
                            wdata <= cand;
                            n     <= n + 9'd1;
                        end
                    end else begin
                        bbuf <= {sha_out_data, bbuf[15:8]};
                        bcnt <= bcnt + 2'd1;
                    end
                end
            end

            S_DONE: begin done <= 1'b1; st <= S_IDLE; end
            default: st <= S_IDLE;
            endcase
        end
    end

endmodule

// ---------------------------------------------------------------------------
// RejBoundedPoly：SHAKE256 → [−η, η] 上的 256 个系数
//
// 与上面唯一实质的区别：一个字节出**两个**候选（低半字节在前），
// 而且第二个候选可能因为已经攒够 256 个而必须丢掉 —— 这一条要写对，
// 否则最后一个系数会串到下一条多项式去。
// ---------------------------------------------------------------------------
module mldsa_poly_eta (
    input  wire        clk,
    input  wire        rst_n,

    input  wire [2:0]  eta,            // 2 或 4，运行时给
    input  wire        start,
    input  wire [511:0] seed,          // ρ'，64 字节
    input  wire [15:0] nonce,
    output reg         done,

    output reg         sha_start,
    output wire [7:0]  sha_rate,
    output wire [7:0]  sha_suffix,
    output reg         sha_in_valid,
    output reg  [7:0]  sha_in_data,
    output reg         sha_in_flush,
    input  wire        sha_in_ready,
    input  wire        sha_out_valid,
    output wire        sha_out_ready,
    input  wire [7:0]  sha_out_data,

    input  wire [7:0]  rd_addr,
    output wire signed [31:0] rd_data,
    output wire [8:0]  count
);
    localparam [7:0] RATE = 8'd136, SUFFIX = 8'h1F;   // SHAKE256

    /* 与 uniform 同一个理由：连续赋值，不能用空敏感列表的 always 块。 */
    assign sha_rate   = RATE;
    assign sha_suffix = SUFFIX;

    localparam [2:0] S_IDLE = 3'd0, S_GAP = 3'd1, S_ABS = 3'd2,
                     S_FLUSH = 3'd3, S_RUN = 3'd4, S_DONE = 3'd5,
                     S_LO = 3'd6, S_HI = 3'd7;
    reg [2:0] st;
    reg [6:0] hdr_i;         // 0..65
    /* 只留高半字节：低半字节在 S_RUN 那一拍就直接送进原语判了，
     * 不需要存。存整个字节会让 lint 正确地指出低 4 位没人用。 */
    reg [3:0] byte_hi;

    wire [6:0] hdr_nxt_i = hdr_i + 7'd1;
    wire [7:0] hdr_byte = (hdr_i < 7'd64) ? seed[hdr_i*8 +: 8]
                        : (hdr_i == 7'd64) ? nonce[7:0]
                                           : nonce[15:8];
    /* 与 uniform 同一个理由：握手那一拍装下一个字节，否则头部整个错一位。 */
    wire [7:0] hdr_byte_nxt = (hdr_nxt_i < 7'd64) ? seed[hdr_nxt_i*8 +: 8]
                            : (hdr_nxt_i == 7'd64) ? nonce[7:0]
                                                   : nonce[15:8];

    reg  [3:0] nib;
    wire signed [31:0] coeff;
    wire               coeff_ok;
    mldsa_rej_eta u_rej (.eta(eta),
        .nibble(nib), .coeff(coeff), .coeff_ok(coeff_ok));

    reg [8:0] n;
    reg       we;
    reg [7:0] waddr;
    /* 与 uniform 同一个理由：coeff 是从 nib 组合出来的，而 nib 在下一拍就变了。 */
    reg signed [31:0] wdata;
    assign count = n;

    // 与 uniform 同一个理由：组合 ready，同一拍 valid&&ready 才消费。
    assign sha_out_ready = (st == S_RUN) && (n < 9'd256);
    wire byte_fire = sha_out_valid && sha_out_ready;

    ram_dp #(.DW(32), .AW(8)) u_mem (
        .clk(clk),
        .a_we(we), .a_addr(waddr), .a_din(wdata), .a_dout(),
        .b_we(1'b0), .b_addr(rd_addr), .b_din(32'd0), .b_dout(rd_data));

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            st <= S_IDLE; done <= 1'b0; n <= 9'd0; hdr_i <= 7'd0;
            sha_start <= 1'b0; sha_in_valid <= 1'b0; sha_in_flush <= 1'b0;
            sha_in_data <= 8'd0;
            we <= 1'b0; waddr <= 8'd0; wdata <= 32'sd0; nib <= 4'd0; byte_hi <= 4'd0;
        end else begin
            sha_start <= 1'b0;
            sha_in_valid <= 1'b0;
            sha_in_flush <= 1'b0;
            we <= 1'b0;
            done <= 1'b0;

            case (st)
            S_IDLE: if (start) begin
                n <= 9'd0; hdr_i <= 7'd0;
                sha_start <= 1'b1;
                st <= S_ABS;
            end

            S_ABS: begin
                sha_in_valid <= 1'b1;
                if (sha_in_valid && sha_in_ready) begin
                    if (hdr_i == 7'd65) begin
                        sha_in_valid <= 1'b0;
                        st <= S_GAP;
                    end else begin
                        hdr_i       <= hdr_nxt_i;
                        sha_in_data <= hdr_byte_nxt;
                    end
                end else begin
                    sha_in_data <= hdr_byte;
                end
            end

            // 与 uniform 同一个理由：空一拍让 in_valid 落下来，flush 才被采样。
            S_GAP: st <= S_FLUSH;

            S_FLUSH: begin
                sha_in_flush <= 1'b1;
                st <= S_RUN;
            end

            S_RUN: begin
                if (n >= 9'd256) begin
                    st <= S_DONE;
                end else if (byte_fire) begin
                    byte_hi <= sha_out_data[7:4];
                    nib     <= sha_out_data[3:0];   // 低半字节先
                    st     <= S_LO;
                end
            end

            // 低半字节判完，切到高半字节。两个状态而不是一拍处理两个：
            // 组合原语只有一份，一拍只能判一个候选。
            S_LO: begin
                if (coeff_ok) begin
                    we    <= 1'b1;
                    waddr <= n[7:0];
                    wdata <= coeff;
                    n     <= n + 9'd1;
                end
                nib <= byte_hi;
                st  <= S_HI;
            end

            S_HI: begin
                // ⚠️ 这里必须重新看 n：低半字节可能刚好把它填到 256。
                // 不看的话高半字节会写到下标 256（回绕成 0），
                // 把第一个系数覆盖掉 —— 而多项式长度看着仍然是对的。
                if (coeff_ok && (n < 9'd256)) begin
                    we    <= 1'b1;
                    waddr <= n[7:0];
                    wdata <= coeff;
                    n     <= n + 9'd1;
                end
                st <= S_RUN;
            end

            S_DONE: begin done <= 1'b1; st <= S_IDLE; end
            default: st <= S_IDLE;
            endcase
        end
    end

endmodule
