// ML-DSA Sign（FIPS 204 §6，external + pure），增量搭建 —— 目前做到第 ① 段 skDecode
//
// ============================================================================
// 【为什么一段一段建】
// ============================================================================
// 与 KeyGen 同一个教训（见 keygen.v 头注 / docs/reference/mldsa-sign-design.zh-CN.md）：
// Sign 的复杂度全在拒绝循环的编排上（海绵反复重开、十几次 invNTT 连算撞「done 是
// 电平」、三个 norm bound）。一次写完只会得到「能编译但没验过」的大块头。这次每加
// 一段就有一个 cocotb 用例把它对上黄金模型（mldsa_oracle.py 的 mldsa_sign，预言机 D）。
//
// 目前实现：S_IDLE → skDecode → done。
//   skDecode(sk) = ρ(32) ‖ K(32) ‖ tr(64) ‖ s₁(ℓ·96) ‖ s₂(k·96) ‖ t₀(k·416)
//   s₁/s₂ 每系数 3 位、逆变换 η−v；t₀ 每系数 13 位、逆变换 2^(D−1)−v。
//   ρ/K/tr 引成输出端口，s₁/s₂/t₀ 系数经 dbg 口读，都对 oracle 的 sk_decode。
//
// 端口按最终形态一次开全（msg/ctx/rnd/sha/sig/μ/ρ''），本段用不到的先驱惰性值，
// 这样测试台 wrapper 不必每加一段就改。
`default_nettype none

module mldsa_sign (
    input  wire        clk,
    input  wire        rst_n,

    input  wire        start,        // 脉冲

    // ---- 输入缓冲：start 之前由测试台按字节预载 ----
    input  wire        sk_wr_en,
    input  wire [11:0] sk_wr_addr,   // sk 2560 字节
    input  wire [7:0]  sk_wr_data,
    input  wire        msg_wr_en,
    input  wire [12:0] msg_wr_addr,  // msg ≤ 8192 字节
    input  wire [7:0]  msg_wr_data,
    input  wire        ctx_wr_en,
    input  wire [7:0]  ctx_wr_addr,  // ctx ≤ 255 字节
    input  wire [7:0]  ctx_wr_data,
    input  wire [13:0] msg_len,
    input  wire [7:0]  ctx_len,
    input  wire [255:0] rnd,         // 32 字节

    output reg         done,

    // ---- 共享 sha3_core（同 KeyGen；本段还没用到，驱惰性值）----
    output reg         sha_start,
    output reg  [7:0]  sha_rate,
    output reg  [7:0]  sha_suffix,
    output reg         sha_in_valid,
    output reg  [7:0]  sha_in_data,
    output reg         sha_in_flush,
    input  wire        sha_in_ready,
    input  wire        sha_out_valid,
    output reg         sha_out_ready,
    input  wire [7:0]  sha_out_data,

    // ---- skDecode 出来的头三段（done 之后有效）----
    output reg [255:0] rho,
    output reg [255:0] key_out,
    output reg [511:0] tr_out,

    // ---- 派生哈希（后续段填；本段恒 0）----
    output reg [511:0] mu,
    output reg [511:0] rhopp,

    // ---- 调试读口：done 之后读系数 ----
    //   dbg_sel[3:2]=00 → s₁，=10 → s₂，=11 → t₀；dbg_sel[1:0] = 第几条多项式
    input  wire [4:0]  dbg_sel,
    input  wire [7:0]  dbg_idx,
    output wire signed [31:0] dbg_coef,

    // ---- sig 输出缓冲（后续段填）----
    input  wire [11:0] sig_addr,
    output wire [7:0]  sig_data
);
    // ================= ML-DSA-44 常量 =================
    localparam integer K = 4, L = 4, ETA = 2, D = 13;
    localparam integer POLYETA_B = 96, POLYT0_B = 416;
    // sk 段偏移
    localparam integer SK_RHO = 0, SK_KEY = 32, SK_TR = 64;
    localparam integer SK_S1 = 128;
    localparam integer SK_S2 = SK_S1 + L*POLYETA_B;      // 512
    localparam integer SK_T0 = SK_S2 + K*POLYETA_B;      // 896

    localparam [4:0]
        S_IDLE   = 5'd0,
        S_HDR    = 5'd1,     // ρ/K/tr ← sk[0..127]
        S_UNP_I  = 5'd2,     // 清 unpacker、置初值
        S_UNP    = 5'd3,     // 位解包主循环
        S_FIN    = 5'd31;

    reg [4:0] st;

    // ================= 输入缓冲 =================
    reg  [11:0] sk_raddr;
    wire [7:0]  sk_rdata;
    ram_dp #(.DW(8), .AW(12)) u_sk (
        .clk(clk), .a_we(sk_wr_en), .a_addr(sk_wr_addr), .a_din(sk_wr_data),
        .a_dout(), .b_we(1'b0), .b_addr(sk_raddr), .b_din(8'd0), .b_dout(sk_rdata));

    // msg / ctx 缓冲：本段不读，但先把写口接上（测试台可预载；②段起才读）
    reg  [12:0] msg_raddr;
    wire [7:0]  msg_rdata;
    ram_dp #(.DW(8), .AW(13)) u_msg (
        .clk(clk), .a_we(msg_wr_en), .a_addr(msg_wr_addr), .a_din(msg_wr_data),
        .a_dout(), .b_we(1'b0), .b_addr(msg_raddr), .b_din(8'd0), .b_dout(msg_rdata));
    reg  [7:0]  ctx_raddr;
    wire [7:0]  ctx_rdata;
    ram_dp #(.DW(8), .AW(8)) u_ctx (
        .clk(clk), .a_we(ctx_wr_en), .a_addr(ctx_wr_addr), .a_din(ctx_wr_data),
        .a_dout(), .b_we(1'b0), .b_addr(ctx_raddr), .b_din(8'd0), .b_dout(ctx_rdata));

    // ================= 系数存储 =================
    // s₁/s₂/t₀ 各 4 条 × 256 × 32b。addr = {poly[1:0], idx[7:0]}。
    reg         s1_we; reg [9:0] s1_waddr; reg signed [31:0] s1_din; reg [9:0] s1_raddr;
    wire signed [31:0] s1_dout;
    ram_dp #(.DW(32), .AW(10)) u_s1 (
        .clk(clk), .a_we(s1_we), .a_addr(s1_waddr), .a_din(s1_din), .a_dout(),
        .b_we(1'b0), .b_addr(s1_raddr), .b_din(32'd0), .b_dout(s1_dout));
    reg         s2_we; reg [9:0] s2_waddr; reg signed [31:0] s2_din; reg [9:0] s2_raddr;
    wire signed [31:0] s2_dout;
    ram_dp #(.DW(32), .AW(10)) u_s2 (
        .clk(clk), .a_we(s2_we), .a_addr(s2_waddr), .a_din(s2_din), .a_dout(),
        .b_we(1'b0), .b_addr(s2_raddr), .b_din(32'd0), .b_dout(s2_dout));
    reg         t0_we; reg [9:0] t0_waddr; reg signed [31:0] t0_din; reg [9:0] t0_raddr;
    wire signed [31:0] t0_dout;
    ram_dp #(.DW(32), .AW(10)) u_t0 (
        .clk(clk), .a_we(t0_we), .a_addr(t0_waddr), .a_din(t0_din), .a_dout(),
        .b_we(1'b0), .b_addr(t0_raddr), .b_din(32'd0), .b_dout(t0_dout));

    // 调试读口挂 b 口（done 后用，与写不重叠）
    assign dbg_coef = dbg_sel[3] ? (dbg_sel[2] ? t0_dout : s2_dout) : s1_dout;

    assign sig_data = 8'd0;   // 后续段填

    // ================= 位解包器（skDecode）=================
    // η（3 位）用于 s₁/s₂，t₀（13 位）单独一个。喂字节 / 抽系数按 mode 二选一。
    reg         eu_clr, eu_iv, eu_or;
    wire        eu_ir, eu_ov;
    wire [2:0]  eu_val;
    mldsa_bitunpack #(.W(3)) u_eu (
        .clk(clk), .rst_n(rst_n), .clr(eu_clr),
        .in_byte(sk_rdata), .in_valid(eu_iv), .in_ready(eu_ir),
        .out_val(eu_val), .out_valid(eu_ov), .out_ready(eu_or));

    reg         tu_clr, tu_iv, tu_or;
    wire        tu_ir, tu_ov;
    wire [12:0] tu_val;
    mldsa_bitunpack #(.W(13)) u_tu (
        .clk(clk), .rst_n(rst_n), .clr(tu_clr),
        .in_byte(sk_rdata), .in_valid(tu_iv), .in_ready(tu_ir),
        .out_val(tu_val), .out_valid(tu_ov), .out_ready(tu_or));

    // 逆变换（有符号搬移）：η−v / 2^(D−1)−v
    wire signed [31:0] eta_coef = $signed(32'sd2) - $signed({29'd0, eu_val});
    wire signed [31:0] t0_coef  = $signed(32'sd1 << (D-1)) - $signed({19'd0, tu_val});

    // ================= 控制寄存器 =================
    reg [7:0]  cnt;        // 头部 / 系数计数（0..127 或 0..255）
    reg        ph;         // 同步读两拍相位
    reg [2:0]  poly;       // 第几条多项式：s₁/s₂ 用 0..7（0..3=s₁,4..7=s₂），t₀ 用 0..3
    reg        t0phase;    // 0 = 正在解 s₁/s₂（3 位），1 = 正在解 t₀（13 位）
    reg [11:0] skp;        // sk 读指针（进解包器的字节）
    reg        feed;       // S_UNP 内两拍：0=抽/摆地址，1=喂字节

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            st <= S_IDLE; done <= 1'b0; cnt <= 8'd0; ph <= 1'b0;
            poly <= 3'd0; t0phase <= 1'b0; skp <= 12'd0; feed <= 1'b0;
            rho <= 256'd0; key_out <= 256'd0; tr_out <= 512'd0;
            mu <= 512'd0; rhopp <= 512'd0;
            sha_start <= 1'b0; sha_rate <= 8'd136; sha_suffix <= 8'h1F;
            sha_in_valid <= 1'b0; sha_in_data <= 8'd0; sha_in_flush <= 1'b0;
            sha_out_ready <= 1'b0;
        end else begin
            done <= 1'b0;

            case (st)
            S_IDLE: if (start) begin
                cnt <= 8'd0; ph <= 1'b0; st <= S_HDR;
            end

            // ---------- ① skDecode 头三段：ρ/K/tr ← sk[0..127] ----------
            // sk 缓冲同步读：ph=0 摆地址，ph=1 数据到位、移进寄存器。
            // 低地址字节先出、从高位塞右移 —— 与 to_bytes(..,'little') 对齐。
            S_HDR: begin
                if (!ph) begin
                    ph <= 1'b1;
                end else begin
                    if (cnt < 8'd32)       rho     <= {sk_rdata, rho[255:8]};
                    else if (cnt < 8'd64)  key_out <= {sk_rdata, key_out[255:8]};
                    else                   tr_out  <= {sk_rdata, tr_out[511:8]};
                    if (cnt == 8'd127) begin
                        cnt <= 8'd0; ph <= 1'b0; st <= S_UNP_I;
                    end else begin
                        cnt <= cnt + 8'd1; ph <= 1'b0;
                    end
                end
            end

            // ---------- ① skDecode 系数段：s₁/s₂（3 位）→ t₀（13 位）----------
            S_UNP_I: begin
                poly <= 3'd0; t0phase <= 1'b0; cnt <= 8'd0; feed <= 1'b0;
                skp <= SK_S1[11:0];
                st <= S_UNP;
            end

            // 位解包主循环。unpacker 的 in_ready / out_valid 互斥：
            //   out_valid（够 W 位）→ 抽一个系数、存进目标 RAM，n++。
            //   否则 → 喂一个 sk 字节。喂要两拍（摆地址 / 数据到位），用 feed 相位。
            S_UNP: begin
                if (!t0phase) begin
                    // ---- s₁/s₂ 段，3 位 ----
                    if (eu_ov) begin
                        // 抽系数：eu_or 这拍拉高、下一拍累加器下移；同拍把系数写进 RAM
                        feed <= 1'b0;
                        if (cnt == 8'd255) begin
                            cnt <= 8'd0;                   // 换条前清累加器由组合块按 cnt==255 判
                            if (poly == 3'd7) begin
                                poly <= 3'd0; t0phase <= 1'b1; skp <= SK_T0[11:0];
                            end else begin
                                poly <= poly + 3'd1;
                            end
                        end else begin
                            cnt <= cnt + 8'd1;
                        end
                    end else begin
                        // 喂字节：feed=0 摆地址，feed=1 数据到位、in_valid
                        if (!feed) begin
                            feed <= 1'b1;
                        end else begin
                            if (eu_ir) skp <= skp + 12'd1;
                            feed <= 1'b0;
                        end
                    end
                end else begin
                    // ---- t₀ 段，13 位 ----
                    if (tu_ov) begin
                        feed <= 1'b0;
                        if (cnt == 8'd255) begin
                            cnt <= 8'd0;
                            if (poly == 3'd3) begin
                                st <= S_FIN;
                            end else begin
                                poly <= poly + 3'd1;
                            end
                        end else begin
                            cnt <= cnt + 8'd1;
                        end
                    end else begin
                        if (!feed) begin
                            feed <= 1'b1;
                        end else begin
                            if (tu_ir) skp <= skp + 12'd1;
                            feed <= 1'b0;
                        end
                    end
                end
            end

            S_FIN: begin done <= 1'b1; st <= S_IDLE; end
            default: st <= S_IDLE;
            endcase
        end
    end

    // ================= 端口/使能（组合）=================
    // clr 需要在「换条那一拍」拉一次。上面时序块里对 eu_clr 做了非阻塞置位，
    // 但组合默认值会覆盖它 —— 所以 clr 统一在组合里按状态判，避免两处打架。
    always @(*) begin
        s1_we = 1'b0; s1_waddr = 10'd0; s1_din = 32'd0;
        s2_we = 1'b0; s2_waddr = 10'd0; s2_din = 32'd0;
        t0_we = 1'b0; t0_waddr = 10'd0; t0_din = 32'd0;
        s1_raddr = {dbg_sel[1:0], dbg_idx};   // 默认给调试口
        s2_raddr = {dbg_sel[1:0], dbg_idx};
        t0_raddr = {dbg_sel[1:0], dbg_idx};
        // S_HDR 读 sk[0..127]（地址=cnt）；S_UNP 读打包区（地址=skp）
        sk_raddr = (st == S_HDR) ? {4'd0, cnt} : skp;
        msg_raddr = 13'd0; ctx_raddr = 8'd0;

        eu_clr = 1'b0; eu_iv = 1'b0; eu_or = 1'b0;
        tu_clr = 1'b0; tu_iv = 1'b0; tu_or = 1'b0;

        // S_UNP_I：进循环前清两个累加器
        if (st == S_UNP_I) begin eu_clr = 1'b1; tu_clr = 1'b1; end

        if (st == S_UNP && !t0phase) begin
            if (eu_ov) begin
                eu_or = 1'b1;                 // 抽系数
                // 存进 s₁（poly 0..3）或 s₂（poly 4..7）
                if (!poly[2]) begin
                    s1_we = 1'b1; s1_waddr = {poly[1:0], cnt}; s1_din = eta_coef;
                end else begin
                    s2_we = 1'b1; s2_waddr = {poly[1:0], cnt}; s2_din = eta_coef;
                end
                // 每条 256 个系数解完，下一拍换条前清累加器
                if (cnt == 8'd255) eu_clr = 1'b1;
            end else if (feed) begin
                eu_iv = 1'b1;                 // 数据到位，喂字节（sk_raddr=skp 已摆好）
            end
        end

        if (st == S_UNP && t0phase) begin
            if (tu_ov) begin
                tu_or = 1'b1;
                t0_we = 1'b1; t0_waddr = {poly[1:0], cnt}; t0_din = t0_coef;
                if (cnt == 8'd255) tu_clr = 1'b1;
            end else if (feed) begin
                tu_iv = 1'b1;
            end
        end
    end
endmodule

`default_nettype wire
