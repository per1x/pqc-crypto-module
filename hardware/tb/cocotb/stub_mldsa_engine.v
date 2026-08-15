// stub_mldsa_engine —— mldsa_engine 的行为级替身，**只进测试台，不进 bitstream**
//
// ============================================================================
// 【为什么要有它】
// ============================================================================
// 真的 mldsa_engine（NTT 流水化 + 时分复用 + 运行时选参数集）在另一条线上做，
// 还没落地。而 mldsa_axi 要验的东西**一条都不依赖算法**：寄存器时序、
// 长度校验、私钥金库的进出、闩锁、非法输入、陈旧状态。这些用一个按固定
// 延迟出假数据的替身验得更干净 —— 跑得快，而且任何一条用例红了都只可能是
// AXI 那一层的问题，不会与算法互相甩锅。
//
// 真 engine 落地之后把它从 Makefile 里换掉即可，**用例一条都不用改**
// （用例只经 AXI 寄存器说话，从不偷看内部信号）。到那时这些用例的意义会变成
// "换了真 engine 之后 AXI 层的行为没变"，仍然值得留着。
//
// ⚠️ 用它验出来的结论必须照实说：**验的是 AXI 逻辑，不是 ML-DSA**。
//
// ============================================================================
// 【替身的行为：一份可以在 Python 里算出来的假输出】
// ============================================================================
// ① 把输入缓冲里 [0, in_total) 的字节**逐个吸收进 SHAKE256**，挤出一个字节 → h。
//    in_total 按双方约定的排布自己算一遍（**故意重算一遍**：两边都算错同一个
//    数的概率远低于抄一份，长度表因此被交叉验证了）。
//    走真的 sha3_core 有两个用处：**它把接线走了一遍**（start/rate/suffix/
//    valid/ready/flush/out_valid 全在这条路上），而且 Python 侧一行
//    hashlib.shake_256(buf).digest(1) 就能对上。
//
//    ⚠️ 用哈希而不是"字节和"是有意的：和是**与顺序无关**的，那样
//    "金库供的 sk 到底落在 engine 的哪个偏移"就验不出来了（错位、甚至整段
//    调换顺序，和都一样）。改成哈希之后，用例里"按槽签 == 自送 sk 签"那条
//    断言才真的钉住了**逐字节相同的一份排布**。
// ② 按 op 铺输出：
//      KeyGen : pk[i] = h + i          sk[j] = (h ^ 0xA5) + j
//      Sign   : sig[i] = h + i
//      Verify : 不出字节，verify_ok = (h == 0)
//    —— h 取决于**整个输入缓冲**，所以 sk 是不是真从金库进来了、rnd/ctx/msg
//    有没有落在该落的地方，都会体现在输出上。用例据此反证金库那条路确实通了。
//    verify_ok 挂在 h 上是为了让两种判定都能确定性地造出来（Python 调两个
//    字节搜一下就能把 h 凑成 0）。
//
// done 是**电平**，保持到下一次 start 才清 —— 与三个 ML-KEM 核一样。
// 这是有意的：mlkem_axi 在真硅上栽过"第二次运行读到残留 done"那个坑，
// 替身必须保留同一个形状，否则 mldsa_axi 的 kickdly 就成了没被验过的代码。
`default_nettype none

module mldsa_engine (
    input  wire        clk,
    input  wire        rst_n,
    // 擦除口：mldsa_axi 把 ZEROIZE/tamper 转给引擎，引擎擦完把 wiping 落下。
    // 替身不含真存储，所以只按固定拍数把 wiping 拉一下，让上层的等待逻辑有东西可等。
    input  wire        zeroize,
    output wire        wiping,

    input  wire        start,
    input  wire [1:0]  op,             // 0=KeyGen 1=Sign 2=Verify
    input  wire [1:0]  pset,           // 0=44 1=65 2=87
    output wire        busy,
    output wire        done,
    output wire        verify_ok,

    input  wire        in_we,
    input  wire [14:0] in_addr,
    input  wire [7:0]  in_data,
    input  wire [15:0] msg_len,
    input  wire [15:0] ctx_len,

    input  wire [14:0] out_addr,
    output wire [7:0]  out_data,
    output wire [15:0] out_len,

    output wire        sha_start,
    output wire [7:0]  sha_rate,
    output wire [7:0]  sha_suffix,
    output wire        sha_in_valid,
    output wire [7:0]  sha_in_data,
    output wire        sha_in_flush,
    input  wire        sha_in_ready,
    input  wire        sha_out_valid,
    input  wire [7:0]  sha_out_data,
    output wire        sha_out_ready
);
    localparam [1:0] OP_KEYGEN = 2'd0, OP_SIGN = 2'd1, OP_VERIFY = 2'd2;

    // ---- 两块存储：输入由外面写，输出由自己写 ----
    reg  [14:0] scan_addr;
    wire [7:0]  scan_data;
    ram_dp #(.DW(8), .AW(15)) u_in (
        .clk(clk),
        .a_we(in_we), .a_addr(in_addr), .a_din(in_data), .a_dout(),
        .b_we(1'b0),  .b_addr(scan_addr), .b_din(8'd0),  .b_dout(scan_data));

    reg        ow_we;
    reg [14:0] ow_addr;
    reg [7:0]  ow_data;
    ram_dp #(.DW(8), .AW(15)) u_out (
        .clk(clk),
        .a_we(ow_we),  .a_addr(ow_addr),  .a_din(ow_data), .a_dout(),
        .b_we(1'b0),   .b_addr(out_addr), .b_din(8'd0),    .b_dout(out_data));

    // ---- 长度表（FIPS 204 表 2）----
    reg [1:0] op_r, pset_r;
    reg [15:0] msg_r, ctx_r;
    wire [12:0] pk_len  = (pset_r == 2'd0) ? 13'd1312
                        : (pset_r == 2'd1) ? 13'd1952 : 13'd2592;
    wire [12:0] sk_len  = (pset_r == 2'd0) ? 13'd2560
                        : (pset_r == 2'd1) ? 13'd4032 : 13'd4896;
    wire [12:0] sig_len = (pset_r == 2'd0) ? 13'd2420
                        : (pset_r == 2'd1) ? 13'd3309 : 13'd4627;

    // 输入排布（与 mldsa_axi 文件头那份契约一致）：
    //   KeyGen : ξ(32)
    //   Sign   : sk ‖ rnd(32) ‖ ctx ‖ msg      —— sk 由软件送或由金库供，
    //                                             对 engine 而言没有区别
    //   Verify : pk ‖ sig ‖ ctx ‖ msg
    wire [16:0] var_len  = {1'b0, ctx_r} + {1'b0, msg_r};
    wire [16:0] in_total = (op_r == OP_KEYGEN) ? 17'd32
                         : (op_r == OP_SIGN)   ? ({4'd0, sk_len} + 17'd32 + var_len)
                         : (var_len + {4'd0, pk_len} + {4'd0, sig_len});
    wire [16:0] out_total = (op_r == OP_KEYGEN) ? ({4'd0, pk_len} + {4'd0, sk_len})
                          : (op_r == OP_SIGN)   ? {4'd0, sig_len} : 17'd0;

    localparam [3:0] S_IDLE = 4'd0, S_SCAN = 4'd1, S_SHA_GO  = 4'd2,
                     S_SHA_PUT = 4'd3, S_SHA_FLUSH = 4'd4, S_SHA_GET = 4'd5,
                     S_WORK = 4'd6, S_EMIT = 4'd7, S_DONE = 4'd8;
    reg [3:0]  state;
    reg [16:0] idx;
    reg [7:0]  h;
    reg [15:0] out_len_r;
    reg        done_r, vok_r;
    reg [5:0]  work_cnt;

    assign busy      = (state != S_IDLE) && (state != S_DONE);
    assign done      = done_r;
    assign verify_ok = vok_r;

    // ---- 擦除：替身没有真存储，但必须把 wiping 拉够一段时间 ----
    // 上层（mldsa_axi）会等 wiping_any 落下才放行启动与读出。若替身把 wiping
    // 恒接 0，那条等待路径在仿真里就**从来没被走过** —— 等真 engine 换上来、
    // 擦除真的要花几万拍时，才第一次暴露上层的等待逻辑对不对。
    // 所以这里给一个短但非零的擦除窗（16 拍），让那条路在替身阶段就被覆盖。
    localparam integer WIPE_CYCLES = 16;
    reg [4:0] wipe_cnt;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)      wipe_cnt <= 5'd0;
        else if (zeroize) wipe_cnt <= WIPE_CYCLES[4:0];
        else if (|wipe_cnt) wipe_cnt <= wipe_cnt - 5'd1;
    end
    assign wiping = |wipe_cnt;
    assign out_len   = out_len_r;

    assign sha_rate      = 8'd136;      // SHAKE256
    assign sha_suffix    = 8'h1F;
    assign sha_start     = (state == S_SHA_GO);
    assign sha_in_valid  = (state == S_SHA_PUT);
    assign sha_in_data   = scan_data;
    assign sha_in_flush  = (state == S_SHA_FLUSH);
    assign sha_out_ready = (state == S_SHA_GET);

    // 地址一直跟着 idx。ram_dp 是同步读，所以每个字节要两拍：
    // S_FETCH 那一拍把地址摆上，S_PUSH 那一拍 scan_data 才是 mem[idx]
    // （idx 不动，地址就不动，被 sha 背压顶住时数据也稳）。
    always @(*) scan_addr = idx[14:0];

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= S_IDLE; idx <= 17'd0;
            h <= 8'd0;
            op_r <= 2'd0; pset_r <= 2'd0; msg_r <= 16'd0; ctx_r <= 16'd0;
            out_len_r <= 16'd0; done_r <= 1'b0; vok_r <= 1'b0;
            ow_we <= 1'b0; ow_addr <= 15'd0; ow_data <= 8'd0;
            work_cnt <= 6'd0;
        end else begin
            ow_we <= 1'b0;
            case (state)
            S_IDLE: if (start) begin
                op_r <= op; pset_r <= pset; msg_r <= msg_len; ctx_r <= ctx_len;
                idx <= 17'd0;
                done_r <= 1'b0; vok_r <= 1'b0; out_len_r <= 16'd0;
                state <= S_SHA_GO;
            end

            // 整个输入缓冲逐字节吸收进 SHAKE256（每字节两拍：摆地址 / 推数据）
            S_SHA_GO:  state <= S_SCAN;
            S_SCAN:    state <= S_SHA_PUT;          // 等 ram_dp 的同步读
            S_SHA_PUT: if (sha_in_ready) begin
                if (idx + 17'd1 == in_total) state <= S_SHA_FLUSH;
                else begin
                    idx   <= idx + 17'd1;
                    state <= S_SCAN;
                end
            end
            S_SHA_FLUSH: state <= S_SHA_GET;        // flush 只在 in_valid 低时被采样
            S_SHA_GET:   if (sha_out_valid) begin
                h        <= sha_out_data;
                work_cnt <= 6'd32;      // 假装又算了一会儿
                state    <= S_WORK;
            end

            S_WORK: begin
                if (work_cnt != 6'd0) begin
                    work_cnt <= work_cnt - 6'd1;
                end else begin
                    idx <= 17'd0;
                    vok_r <= (op_r == OP_VERIFY) ? (h == 8'd0) : 1'b0;
                    state <= (out_total == 17'd0) ? S_DONE : S_EMIT;
                end
            end

            S_EMIT: begin
                ow_we   <= 1'b1;
                ow_addr <= idx[14:0];
                // KeyGen 的 sk 段换一个式子，免得和 pk 段长得一样 ——
                // 用例要能一眼看出"出来的是 pk 还是 sk"
                ow_data <= ((op_r == OP_KEYGEN) && (idx >= {4'd0, pk_len}))
                           ? ((h ^ 8'hA5) + (idx[7:0] - pk_len[7:0]))
                           : (h + idx[7:0]);
                if (idx + 17'd1 == out_total) begin
                    state <= S_DONE;
                end else begin
                    idx <= idx + 17'd1;
                end
            end

            // done 是电平，保持到下一次 start（与三个 ML-KEM 核一致）
            S_DONE: begin
                out_len_r <= out_total[15:0];
                done_r    <= 1'b1;
                state     <= S_IDLE;
            end

            default: state <= S_IDLE;
            endcase
        end
    end

endmodule

`default_nettype wire
