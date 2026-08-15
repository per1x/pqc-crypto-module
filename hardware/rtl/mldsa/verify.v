// ML-DSA Verify（FIPS 204 §6，external + pure），增量搭建 —— 目前做到第 ① 段 sigDecode/pkDecode
//
// ============================================================================
// 【为什么一段一段建】
// ============================================================================
// 同 KeyGen / Sign 的教训（见 docs/reference/mldsa-verify-design.zh-CN.md）：
// 每加一段就有一个 cocotb 用例把它对上黄金模型，绿了才加下一段。
//
// Verify 与 Sign 的关键不同：没有拒绝循环（一趟直线跑完），但**签名是攻击者可控的
// 输入**，所以 sigDecode 必须做结构合法性检查。而"错误地返回 true"是最危险的失败
// 模式，因此拒绝类要逐类覆盖。
//
// 目前实现：S_IDLE → 清 h → c̃ ← sig[0..31] → z 位解包(18 位, γ₁−v, 带 ‖z‖∞ 检查)
//           → HintBitUnpack(计数单调/下标严格递增/填充全零) → t₁ 位解包(10 位) → done。
//
// 端口按最终形态一次开全（sha/msg/ctx/valid/μ/tr），本段用不到的先驱惰性值。
`default_nettype none

module mldsa_verify (
    input  wire        clk,
    input  wire        rst_n,

    input  wire        start,

    // ---- 输入缓冲：start 之前由测试台按字节预载 ----
    input  wire        pk_wr_en,
    input  wire [10:0] pk_wr_addr,    // pk 1312 字节
    input  wire [7:0]  pk_wr_data,
    input  wire        sig_wr_en,
    input  wire [11:0] sig_wr_addr,   // σ 2420 字节
    input  wire [7:0]  sig_wr_data,
    input  wire        msg_wr_en,
    input  wire [12:0] msg_wr_addr,   // msg ≤ 8192
    input  wire [7:0]  msg_wr_data,
    input  wire        ctx_wr_en,
    input  wire [7:0]  ctx_wr_addr,   // ctx ≤ 255
    input  wire [7:0]  ctx_wr_data,
    input  wire [13:0] msg_len,
    input  wire [7:0]  ctx_len,

    output reg         done,
    output reg         valid,          // done 时有效：签名是否通过

    // ---- 共享 sha3_core（FSM / ExpandA / SampleInBall 三选一）----
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

    // ---- 观察口（逐段验证用）----
    output reg [255:0] ctilde,         // σ 里的 c̃
    output reg [255:0] ctilde_p,       // 算出来的 c̃'
    output reg [511:0] tr_out,
    output reg [511:0] mu,
    output reg         zbad,           // ‖z‖∞ ≥ γ₁−β
    output reg         hbad,           // hint 编码结构非法

    // ---- 调试读口：dbg_sel[5:2] 选组，[1:0] 选第几条 ----
    //   0 z(后被 NTT 覆盖成 ẑ)  1 t₁(后 t̂₁)  2 hint 位  3 c(后 ĉ)  4 acc
    input  wire [5:0]  dbg_sel,
    input  wire [7:0]  dbg_idx,
    output wire signed [31:0] dbg_coef
);
    // ================= ML-DSA-44 常量 =================
    localparam integer K = 4, L = 4, D = 13;
    localparam [11:0] SIG_Z0 = 12'd32;      // z 段起点
    localparam [11:0] SIG_H0 = 12'd2336;    // hint 段起点 = 32 + ℓ·576
    localparam [7:0]  OMEGA  = 8'd80;
    localparam [10:0] PK_T1  = 11'd32;      // pk 里 t₁ 起点
    // ‖z‖∞ 的界：γ₁−β = 131072−78
    localparam [31:0] ZBOUND = 32'd130994;

    localparam [5:0]
        S_IDLE  = 6'd0,
        S_HCLR  = 6'd1,      // 清 h 存储（BRAM 无复位，残留会造成假阳性）
        S_CT    = 6'd2,      // c̃ ← sig[0..31]
        S_ZU    = 6'd3,      // z 位解包（18 位，γ₁−v）+ ‖z‖∞
        S_HC    = 6'd4,      // 读 k 个累计计数字节
        S_HI    = 6'd5,      // 扫下标、置 h 位、查严格递增
        S_HP    = 6'd6,      // 查填充区全零
        S_T1    = 6'd7,      // t₁ 位解包（10 位）
        S_FIN   = 6'd63;

    reg [5:0] st;

    // ================= 输入缓冲 =================
    reg  [10:0] pk_raddr;  wire [7:0] pk_rdata;
    ram_dp #(.DW(8), .AW(11)) u_pk (
        .clk(clk), .a_we(pk_wr_en), .a_addr(pk_wr_addr), .a_din(pk_wr_data),
        .a_dout(), .b_we(1'b0), .b_addr(pk_raddr), .b_din(8'd0), .b_dout(pk_rdata));
    reg  [11:0] sig_raddr; wire [7:0] sig_rdata;
    ram_dp #(.DW(8), .AW(12)) u_sig (
        .clk(clk), .a_we(sig_wr_en), .a_addr(sig_wr_addr), .a_din(sig_wr_data),
        .a_dout(), .b_we(1'b0), .b_addr(sig_raddr), .b_din(8'd0), .b_dout(sig_rdata));
    reg  [12:0] msg_raddr; wire [7:0] msg_rdata;
    ram_dp #(.DW(8), .AW(13)) u_msg (
        .clk(clk), .a_we(msg_wr_en), .a_addr(msg_wr_addr), .a_din(msg_wr_data),
        .a_dout(), .b_we(1'b0), .b_addr(msg_raddr), .b_din(8'd0), .b_dout(msg_rdata));
    reg  [7:0]  ctx_raddr; wire [7:0] ctx_rdata;
    ram_dp #(.DW(8), .AW(8)) u_ctx (
        .clk(clk), .a_we(ctx_wr_en), .a_addr(ctx_wr_addr), .a_din(ctx_wr_data),
        .a_dout(), .b_we(1'b0), .b_addr(ctx_raddr), .b_din(8'd0), .b_dout(ctx_rdata));

    // ================= 系数存储 =================
    reg         z_we;  reg [9:0] z_waddr;  reg signed [31:0] z_din;  reg [9:0] z_raddr;
    wire signed [31:0] z_dout;
    ram_dp #(.DW(32), .AW(10)) u_z (
        .clk(clk), .a_we(z_we), .a_addr(z_waddr), .a_din(z_din), .a_dout(),
        .b_we(1'b0), .b_addr(z_raddr), .b_din(32'd0), .b_dout(z_dout));
    reg         t1_we; reg [9:0] t1_waddr; reg signed [31:0] t1_din; reg [9:0] t1_raddr;
    wire signed [31:0] t1_dout;
    ram_dp #(.DW(32), .AW(10)) u_t1 (
        .clk(clk), .a_we(t1_we), .a_addr(t1_waddr), .a_din(t1_din), .a_dout(),
        .b_we(1'b0), .b_addr(t1_raddr), .b_din(32'd0), .b_dout(t1_dout));
    // hint：k×256 位
    reg         h_we;  reg [9:0] h_waddr;  reg h_din;  reg [9:0] h_raddr;
    wire        h_dout;
    ram_dp #(.DW(1), .AW(10)) u_h (
        .clk(clk), .a_we(h_we), .a_addr(h_waddr), .a_din(h_din), .a_dout(),
        .b_we(1'b0), .b_addr(h_raddr), .b_din(1'b0), .b_dout(h_dout));

    assign dbg_coef =
          (dbg_sel[5:2] == 4'd0) ? z_dout
        : (dbg_sel[5:2] == 4'd1) ? t1_dout
        : (dbg_sel[5:2] == 4'd2) ? {31'd0, h_dout}
        : 32'd0;

    // ================= 位解包器 =================
    // z：18 位 → γ₁−v；t₁：10 位 → 直接用
    reg         zu_clr, zu_iv, zu_or;
    wire        zu_ir, zu_ov;
    wire [17:0] zu_val;
    mldsa_bitunpack #(.W(18)) u_zu (
        .clk(clk), .rst_n(rst_n), .clr(zu_clr),
        .in_byte(sig_rdata), .in_valid(zu_iv), .in_ready(zu_ir),
        .out_val(zu_val), .out_valid(zu_ov), .out_ready(zu_or));
    reg         tu_clr, tu_iv, tu_or;
    wire        tu_ir, tu_ov;
    wire [9:0]  tu_val;
    mldsa_bitunpack #(.W(10)) u_tu (
        .clk(clk), .rst_n(rst_n), .clr(tu_clr),
        .in_byte(pk_rdata), .in_valid(tu_iv), .in_ready(tu_ir),
        .out_val(tu_val), .out_valid(tu_ov), .out_ready(tu_or));

    wire signed [31:0] z_coef  = $signed(32'sd131072) - $signed({14'd0, zu_val});
    wire signed [31:0] t1_coef = $signed({22'd0, tu_val});
    // ‖z‖∞：z 已居中，直接取绝对值比
    wire [31:0] z_abs = z_coef[31] ? (-z_coef) : z_coef;
    wire        z_over = (z_abs >= ZBOUND);

    // ================= 控制寄存器 =================
    reg [7:0]  cnt;
    reg        ph;
    reg [2:0]  poly;
    reg [11:0] rdp;        // 读指针（喂解包器的字节）
    reg        feed;
    reg [9:0]  hclr;       // 清 h 的计数
    // HintBitUnpack 用
    reg [7:0]  hcnt [0:K-1];   // 每条的累计计数
    reg [7:0]  hidx;           // 运行下标
    reg [7:0]  hfirst;         // 本条起点（判"严格递增"用）
    reg [7:0]  hprev;          // 上一个下标
    integer    ii;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            st <= S_IDLE; done <= 1'b0; valid <= 1'b0;
            cnt <= 8'd0; ph <= 1'b0; poly <= 3'd0; rdp <= 12'd0; feed <= 1'b0;
            hclr <= 10'd0; hidx <= 8'd0; hfirst <= 8'd0; hprev <= 8'd0;
            ctilde <= 256'd0; ctilde_p <= 256'd0; tr_out <= 512'd0; mu <= 512'd0;
            zbad <= 1'b0; hbad <= 1'b0;
            sha_start <= 1'b0; sha_rate <= 8'd136; sha_suffix <= 8'h1F;
            sha_in_valid <= 1'b0; sha_in_data <= 8'd0; sha_in_flush <= 1'b0;
            sha_out_ready <= 1'b0;
            for (ii = 0; ii < K; ii = ii + 1) hcnt[ii] <= 8'd0;
        end else begin
            done <= 1'b0;

            case (st)
            S_IDLE: if (start) begin
                zbad <= 1'b0; hbad <= 1'b0; valid <= 1'b0;
                hclr <= 10'd0; cnt <= 8'd0; ph <= 1'b0; poly <= 3'd0;
                st <= S_HCLR;
            end

            // ---------- 清 h 存储 ----------
            // ⚠️ 稀疏置位（只写 1、不写 0）意味着没被置到的位靠初值。BRAM 无复位口，
            // 连续验多条时上一条的 1 会残留 → 上一条的 hint 泄漏进这一条，可能让
            // **本该拒绝的签名通过**（假阳性）。所以入口必须显式清 k×256 位。
            S_HCLR: begin
                if (hclr == 10'd1023) begin
                    cnt <= 8'd0; st <= S_CT;
                end else begin
                    hclr <= hclr + 10'd1;
                end
            end

            // ---------- c̃ ← sig[0..31]（同步读，两拍相位）----------
            S_CT: begin
                if (!ph) begin
                    ph <= 1'b1;
                end else begin
                    ctilde <= {sig_rdata, ctilde[255:8]};
                    if (cnt == 8'd31) begin
                        cnt <= 8'd0; ph <= 1'b0; poly <= 3'd0; feed <= 1'b0;
                        rdp <= SIG_Z0;
                        st <= S_ZU;
                    end else begin
                        cnt <= cnt + 8'd1; ph <= 1'b0;
                    end
                end
            end

            // ---------- z 位解包（ℓ 条 × 256 系数，18 位）+ ‖z‖∞ ----------
            // 与 Sign 的 skDecode 同构：out_valid 时抽系数，否则喂字节（两拍摆地址）。
            S_ZU: begin
                if (zu_ov) begin
                    feed <= 1'b0;
                    if (z_over) zbad <= 1'b1;
                    if (cnt == 8'd255) begin
                        cnt <= 8'd0;
                        if (poly == 3'd3) begin
                            poly <= 3'd0; cnt <= 8'd0; ph <= 1'b0;
                            rdp <= SIG_H0 + {4'd0, OMEGA};   // 指向计数区
                            st <= S_HC;
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
                        if (zu_ir) rdp <= rdp + 12'd1;
                        feed <= 1'b0;
                    end
                end
            end

            // ---------- HintBitUnpack ① 读 k 个累计计数字节 ----------
            S_HC: begin
                if (!ph) begin
                    ph <= 1'b1;
                end else begin
                    hcnt[cnt[1:0]] <= sig_rdata;
                    if (cnt == K[7:0] - 8'd1) begin
                        cnt <= 8'd0; ph <= 1'b0; poly <= 3'd0;
                        hidx <= 8'd0; hfirst <= 8'd0; hprev <= 8'd0;
                        rdp <= SIG_H0;
                        st <= S_HI;
                    end else begin
                        cnt <= cnt + 8'd1; ph <= 1'b0;
                    end
                end
            end

            // ---------- HintBitUnpack ② 逐条扫下标 ----------
            // end=hcnt[i]：end<hidx（非单调）或 end>ω ⇒ 非法。
            // 同一条内下标必须**严格递增**（hfirst 重置比较链，跨条不比 —— 漏了会把
            // 合法签名判成非法）。h[i][sig_rdata] 置 1。
            S_HI: begin
                if (hcnt[poly[1:0]] < hidx || hcnt[poly[1:0]] > OMEGA) begin
                    hbad <= 1'b1;
                    // 结构已非法，不再按它扫（继续跑到底，最后统一判 valid）
                    if (poly == 3'd3) begin cnt <= 8'd0; ph <= 1'b0; st <= S_HP; end
                    else begin poly <= poly + 3'd1; end
                end else if (hidx == hcnt[poly[1:0]]) begin
                    // 这一条扫完
                    if (poly == 3'd3) begin
                        cnt <= 8'd0; ph <= 1'b0; st <= S_HP;
                    end else begin
                        poly <= poly + 3'd1;
                        hfirst <= hidx;          // 下一条的比较链起点
                    end
                end else begin
                    if (!ph) begin
                        ph <= 1'b1;              // 摆 sig 读地址（组合里 rdp=hidx 偏移）
                    end else begin
                        if (hidx > hfirst && hprev >= sig_rdata) hbad <= 1'b1;
                        hprev <= sig_rdata;
                        hidx  <= hidx + 8'd1;
                        ph    <= 1'b0;
                    end
                end
            end

            // ---------- HintBitUnpack ③ 填充区必须全零 ----------
            S_HP: begin
                if (hidx + cnt >= OMEGA) begin
                    cnt <= 8'd0; ph <= 1'b0; poly <= 3'd0; feed <= 1'b0;
                    rdp <= {1'b0, PK_T1};
                    st <= S_T1;
                end else if (!ph) begin
                    ph <= 1'b1;
                end else begin
                    if (sig_rdata != 8'd0) hbad <= 1'b1;
                    cnt <= cnt + 8'd1; ph <= 1'b0;
                end
            end

            // ---------- t₁ 位解包（k 条 × 256 系数，10 位，无变换）----------
            S_T1: begin
                if (tu_ov) begin
                    feed <= 1'b0;
                    if (cnt == 8'd255) begin
                        cnt <= 8'd0;
                        if (poly == 3'd3) begin
                            poly <= 3'd0;
                            st <= S_FIN;      // 本里程碑到此（② 起接 tr/μ）
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
                        if (tu_ir) rdp <= rdp + 12'd1;
                        feed <= 1'b0;
                    end
                end
            end

            S_FIN: begin done <= 1'b1; valid <= !zbad && !hbad; st <= S_IDLE; end
            default: st <= S_IDLE;
            endcase
        end
    end

    // ================= 端口/使能（组合）=================
    always @(*) begin
        z_we = 1'b0; z_waddr = 10'd0; z_din = 32'd0;
        z_raddr = {dbg_sel[1:0], dbg_idx};
        t1_we = 1'b0; t1_waddr = 10'd0; t1_din = 32'd0;
        t1_raddr = {dbg_sel[1:0], dbg_idx};
        h_we = 1'b0; h_waddr = 10'd0; h_din = 1'b0;
        h_raddr = {dbg_sel[1:0], dbg_idx};
        zu_clr = 1'b0; zu_iv = 1'b0; zu_or = 1'b0;
        tu_clr = 1'b0; tu_iv = 1'b0; tu_or = 1'b0;
        pk_raddr = rdp[10:0];
        sig_raddr = rdp;
        msg_raddr = 13'd0; ctx_raddr = 8'd0;

        // 清 h
        if (st == S_HCLR) begin h_we = 1'b1; h_waddr = hclr; h_din = 1'b0; end
        // 进解包段前清累加器
        if (st == S_CT) begin zu_clr = 1'b1; tu_clr = 1'b1; end

        // c̃ 读 sig[cnt]
        if (st == S_CT) sig_raddr = {4'd0, cnt};

        // z 解包：抽系数写 z[poly]，否则喂字节
        if (st == S_ZU) begin
            if (zu_ov) begin
                zu_or = 1'b1;
                z_we = 1'b1; z_waddr = {poly[1:0], cnt}; z_din = z_coef;
            end else if (feed) begin
                zu_iv = 1'b1;
            end
        end

        // hint 计数区：sig[SIG_H0+ω+cnt]
        if (st == S_HC) sig_raddr = SIG_H0 + {4'd0, OMEGA} + {4'd0, cnt};
        // hint 下标区：sig[SIG_H0+hidx]；命中就置 h[poly][sig_rdata]
        if (st == S_HI) begin
            sig_raddr = SIG_H0 + {4'd0, hidx};
            if (ph) begin
                h_we = 1'b1; h_waddr = {poly[1:0], sig_rdata}; h_din = 1'b1;
            end
        end
        // 填充区：sig[SIG_H0+hidx+cnt]
        if (st == S_HP) sig_raddr = SIG_H0 + {4'd0, hidx} + {4'd0, cnt};

        // t₁ 解包：抽系数写 t1[poly]，否则喂字节
        if (st == S_T1) begin
            if (tu_ov) begin
                tu_or = 1'b1;
                t1_we = 1'b1; t1_waddr = {poly[1:0], cnt}; t1_din = t1_coef;
            end else if (feed) begin
                tu_iv = 1'b1;
            end
        end
    end
endmodule

`default_nettype wire
