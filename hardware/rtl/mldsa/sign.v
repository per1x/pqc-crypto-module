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

    // ---- 共享 sha3_core（FSM 与 ExpandMask 采样器按 owner 三选一，同 KeyGen）----
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
        // ② μ = H(tr‖M')，ρ'' = H(K‖rnd‖μ)：共用一套吸收/挤压状态
        S_D_GO   = 5'd4,     // 起海绵：start 脉冲、ai=0
        S_D_ABS  = 5'd5,     // 吸收（μ 支：tr‖0‖|ctx|‖ctx‖msg；ρ'' 支：K‖rnd‖μ）
        S_D_GAP  = 5'd6,     // 让 in_valid 落下，flush 才被采样
        S_D_FLU  = 5'd7,     // in_flush
        S_D_SQ   = 5'd8,     // 挤 64 字节 → μ 或 ρ''
        // ③ 对 s₁/s₂/t₀ 就地 NTT
        S_NT_LD  = 5'd9,     // store[poly] → NTT 写口
        S_NT_GO  = 5'd10,    // nt_start
        S_NT_ST  = 5'd11,    // 等 done 落一次再起
        S_NT_WB  = 5'd12,    // NTT 读口 → store[poly]
        // ④ y = ExpandMask(ρ'', κ+r)（拒绝循环入口）
        S_EM_GO  = 5'd13,    // 起 ExpandMask 采样器
        S_EM_WT  = 5'd14,    // 等 em_done
        S_EM_MV  = 5'd15,    // 采样结果 → y[poly]
        // ⑤ w=invNTT(Â∘ŷ)，(w0,w1)=Decompose(caddq(w))
        S_A_GO   = 5'd16,    // 起 ExpandA 采 Â[vi][vj]
        S_A_WT   = 5'd17,    // 等 un_done
        S_MAC    = 5'd18,    // acc[vi] += mont(Â∘ŷ[vj])
        S_RED    = 5'd19,    // reduce32(acc[vi]) → invNTT 写口
        S_INV_GO = 5'd20,    // nt_start（inverse）
        S_INV_ST = 5'd21,    // 等 done 落一次再起
        S_DEC    = 5'd22,    // caddq(invNTT) → decompose → w0/w1
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

    // ---- NTT 核（③：对 s₁/s₂/t₀ 就地正变换成 ŝ₁/ŝ₂/t̂₀）----
    reg         nt_start, nt_inv, nt_we;
    reg  [7:0]  nt_waddr, nt_raddr;
    reg signed [31:0] nt_wdata;
    wire        nt_done;
    wire signed [31:0] nt_rdata;
    mldsa_ntt_core u_ntt (
        .clk(clk), .rst_n(rst_n),
        .start(nt_start), .inverse(nt_inv), .done(nt_done),
        .wr_en(nt_we), .wr_addr(nt_waddr), .wr_data(nt_wdata),
        .rd_addr(nt_raddr), .rd_data(nt_rdata));

    // ================= 海绵归属（FSM ↔ ExpandMask 采样器）=================
    // 只在换手方空闲时切（KeyGen 坑表第 1 条）。后续段（SampleInBall / ExpandA）
    // 再往 owner 里加成员。
    localparam [1:0] OWN_FSM = 2'd0, OWN_EM = 2'd1, OWN_UNI = 2'd2;
    reg [1:0] owner;

    // FSM 自己驱动海绵时用的那组线
    reg        fsm_ss, fsm_siv, fsm_sif, fsm_sor;
    reg [7:0]  fsm_sr, fsm_su, fsm_sid;

    // ---- ExpandMask 采样器（④：y = ExpandMask(ρ'', κ+r)）----
    reg         em_start;
    reg  [15:0] em_nonce;
    wire        em_done;
    reg  [7:0]  em_rd_addr;
    wire signed [31:0] em_rd_data;
    wire        em_ss, em_siv, em_sif, em_sor;
    wire [7:0]  em_sr, em_su, em_sid;
    mldsa_expand_mask #(.GAMMA1(1 << 17), .CBITS(18)) u_em (
        .clk(clk), .rst_n(rst_n),
        .start(em_start), .seed(rhopp), .nonce(em_nonce), .done(em_done),
        .sha_start(em_ss), .sha_rate(em_sr), .sha_suffix(em_su),
        .sha_in_valid(em_siv), .sha_in_data(em_sid), .sha_in_flush(em_sif),
        .sha_in_ready(sha_in_ready && (owner == OWN_EM)),
        .sha_out_valid(sha_out_valid && (owner == OWN_EM)),
        .sha_out_ready(em_sor), .sha_out_data(sha_out_data),
        .rd_addr(em_rd_addr), .rd_data(em_rd_data));

    // ---- y 存储：ℓ 条 × 256 × 32b（④ 存，⑤ NTT 就地覆盖成 ŷ）----
    reg         y_we; reg [9:0] y_waddr; reg signed [31:0] y_din; reg [9:0] y_raddr;
    wire signed [31:0] y_dout;
    ram_dp #(.DW(32), .AW(10)) u_y (
        .clk(clk), .a_we(y_we), .a_addr(y_waddr), .a_din(y_din), .a_dout(),
        .b_we(1'b0), .b_addr(y_raddr), .b_din(32'd0), .b_dout(y_dout));

    // ---- ⑤ MAC 累加缓冲 acc（k×256×32），w0（k×256×32），w1（k×256×6）----
    reg         ac_we; reg [9:0] ac_waddr; reg signed [31:0] ac_din; reg [9:0] ac_raddr;
    wire signed [31:0] ac_dout;
    ram_dp #(.DW(32), .AW(10)) u_acc (
        .clk(clk), .a_we(ac_we), .a_addr(ac_waddr), .a_din(ac_din), .a_dout(),
        .b_we(1'b0), .b_addr(ac_raddr), .b_din(32'd0), .b_dout(ac_dout));
    reg         w0_we; reg [9:0] w0_waddr; reg signed [31:0] w0_din; reg [9:0] w0_raddr;
    wire signed [31:0] w0_dout;
    ram_dp #(.DW(32), .AW(10)) u_w0 (
        .clk(clk), .a_we(w0_we), .a_addr(w0_waddr), .a_din(w0_din), .a_dout(),
        .b_we(1'b0), .b_addr(w0_raddr), .b_din(32'd0), .b_dout(w0_dout));
    reg         w1_we; reg [9:0] w1_waddr; reg [5:0] w1_din; reg [9:0] w1_raddr;
    wire [5:0]  w1_dout;
    ram_dp #(.DW(6), .AW(10)) u_w1 (
        .clk(clk), .a_we(w1_we), .a_addr(w1_waddr), .a_din(w1_din), .a_dout(),
        .b_we(1'b0), .b_addr(w1_raddr), .b_din(6'd0), .b_dout(w1_dout));

    // ---- ExpandA 均匀采样器（⑤：Â 现采现用，seed=ρ，nonce=256·i+j）----
    reg         un_start;
    reg  [15:0] un_nonce;
    wire        un_done;
    reg  [7:0]  un_rd_addr;
    wire [22:0] un_rd_data;
    wire        un_ss, un_siv, un_sif, un_sor;
    wire [7:0]  un_sr, un_su, un_sid;
    mldsa_poly_uniform u_uni (
        .clk(clk), .rst_n(rst_n),
        .start(un_start), .seed(rho), .nonce(un_nonce), .done(un_done),
        .sha_start(un_ss), .sha_rate(un_sr), .sha_suffix(un_su),
        .sha_in_valid(un_siv), .sha_in_data(un_sid), .sha_in_flush(un_sif),
        .sha_in_ready(sha_in_ready && (owner == OWN_UNI)),
        .sha_out_valid(sha_out_valid && (owner == OWN_UNI)),
        .sha_out_ready(un_sor), .sha_out_data(sha_out_data),
        .rd_addr(un_rd_addr), .rd_data(un_rd_data), .count());

    // 逐系数 mont 乘：Â(23 无符号) × ŷ(32 有符号)
    wire signed [63:0] mac_prod = $signed({41'd0, un_rd_data}) * y_dout;
    wire signed [31:0] mac_mont;
    mldsa_mont_reduce u_mont (.a(mac_prod), .t_out(mac_mont));
    // reduce32：MAC 累加值灌进 invNTT 前先规约
    wire signed [31:0] red_out;
    mldsa_reduce32 u_red (.a(ac_dout), .r(red_out));
    // caddq → decompose（MODE=0，γ₂=(q−1)/88）
    wire signed [31:0] cad_out;
    mldsa_caddq u_cad (.a(nt_rdata), .r(cad_out));
    wire signed [31:0] dec_a0;
    wire        [5:0]  dec_a1;
    mldsa_decompose #(.MODE(0)) u_dec (.a(cad_out), .a0(dec_a0), .a1(dec_a1));

    // 调试读口挂 b 口（done 后用，与写不重叠）。dbg_sel[4:2] 选组，[1:0] 选第几条。
    //   000 s₁  010 s₂  011 t₀  100 y  101 w0  110 w1
    assign dbg_coef =
          (dbg_sel[4:2] == 3'b000) ? s1_dout
        : (dbg_sel[4:2] == 3'b010) ? s2_dout
        : (dbg_sel[4:2] == 3'b011) ? t0_dout
        : (dbg_sel[4:2] == 3'b100) ? y_dout
        : (dbg_sel[4:2] == 3'b101) ? w0_dout
        : (dbg_sel[4:2] == 3'b110) ? {{26{1'b0}}, w1_dout}
        : 32'd0;

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

    // ② 派生哈希用
    reg [13:0] ai;         // 吸收字节指针（μ 支最长 66+ctx+msg，≤ ~8500，14 位）
    reg        dsel;       // 0 = 正在算 μ，1 = 正在算 ρ''

    // ③ NTT prep 用
    reg [1:0]  nstore;     // 0=s₁, 1=s₂, 2=t₀
    reg        nt_lowseen; // 「done 是电平」：start 后先见它落一次再等它起

    // ④ 拒绝循环用
    reg [15:0] kappa;      // ExpandMask 的 nonce 基（每轮 +ℓ）
    // ⑤ MAC / invNTT / decompose 用
    reg [2:0]  vi;         // 第几个 i（0..k-1）
    reg [2:0]  vj;         // 第几个 j（0..ℓ-1）

    // NTT 装载时选中的 store 数据（nstore：0=s₁,1=s₂,2=t₀,3=y）
    wire signed [31:0] store_dout =
        (nstore == 2'd0) ? s1_dout : (nstore == 2'd1) ? s2_dout
      : (nstore == 2'd2) ? t0_dout : y_dout;

    // ---- μ 支的吸收源：tr(64) ‖ 0x00 ‖ |ctx| ‖ ctx ‖ msg ----
    wire [5:0]  tr_idx      = ai[5:0];
    wire [13:0] thr_ctxend  = 14'd66 + {6'd0, ctx_len};              // ctx 占 [66, thr_ctxend)
    wire [13:0] thr_total_mu= thr_ctxend + msg_len;                  // 66+|ctx|+|msg|
    wire [13:0] ctx_off     = ai - 14'd66;
    wire [13:0] msg_off     = ai - thr_ctxend;
    reg  [7:0]  mu_byte;
    always @(*) begin
        if (ai < 14'd64)            mu_byte = tr_out[tr_idx*8 +: 8];
        else if (ai == 14'd64)      mu_byte = 8'd0;
        else if (ai == 14'd65)      mu_byte = ctx_len;
        else if (ai <  thr_ctxend)  mu_byte = ctx_rdata;
        else                        mu_byte = msg_rdata;
    end

    // ---- ρ'' 支的吸收源：K(32) ‖ rnd(32) ‖ μ(64) = 128 字节 ----
    wire [6:0]  rp_i = ai[6:0];
    reg  [7:0]  rp_byte;
    always @(*) begin
        if (ai < 14'd32)       rp_byte = key_out[rp_i[4:0]*8 +: 8];
        else if (ai < 14'd64)  rp_byte = rnd[(rp_i[4:0])*8 +: 8];    // ai−32 的低 5 位
        else                   rp_byte = mu[(rp_i[5:0])*8 +: 8];     // ai−64 的低 6 位
    end

    // 当前吸收的字节 / 总长 / 结束判据
    wire [7:0]  abs_byte  = dsel ? rp_byte : mu_byte;
    wire [13:0] abs_total = dsel ? 14'd128 : thr_total_mu;
    wire        abs_last  = (ai == abs_total - 14'd1);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            st <= S_IDLE; done <= 1'b0; cnt <= 8'd0; ph <= 1'b0;
            poly <= 3'd0; t0phase <= 1'b0; skp <= 12'd0; feed <= 1'b0;
            ai <= 14'd0; dsel <= 1'b0;
            nstore <= 2'd0; nt_lowseen <= 1'b0;
            nt_start <= 1'b0; nt_inv <= 1'b0;
            owner <= OWN_FSM; em_start <= 1'b0; em_nonce <= 16'd0;
            kappa <= 16'd0; vi <= 3'd0; vj <= 3'd0;
            un_start <= 1'b0; un_nonce <= 16'd0;
            rho <= 256'd0; key_out <= 256'd0; tr_out <= 512'd0;
            mu <= 512'd0; rhopp <= 512'd0;
            fsm_ss <= 1'b0; fsm_sr <= 8'd136; fsm_su <= 8'h1F;
            fsm_siv <= 1'b0; fsm_sid <= 8'd0; fsm_sif <= 1'b0;
            fsm_sor <= 1'b0;
        end else begin
            done <= 1'b0;
            fsm_ss <= 1'b0;
            fsm_sif <= 1'b0;
            nt_start <= 1'b0;
            em_start <= 1'b0;
            un_start <= 1'b0;

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
                                dsel <= 1'b0;      // 先算 μ
                                st <= S_D_GO;
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

            // ---------- ② μ = H(tr‖M')，ρ'' = H(K‖rnd‖μ) ----------
            // dsel=0 算 μ，dsel=1 算 ρ''，共用这一套吸收/挤压状态。
            S_D_GO: begin
                owner <= OWN_FSM;
                fsm_sr <= 8'd136; fsm_su <= 8'h1F;   // SHAKE256
                fsm_ss <= 1'b1;
                ai <= 14'd0; ph <= 1'b0; cnt <= 8'd0;
                st <= S_D_ABS;
            end

            // 2 拍一个字节：ph=0 摆 ctx/msg 读地址（μ 支才用），ph=1 数据到位、驱 valid。
            // 与 KeyGen 的 S_TR_ABS 同构：从缓冲取字节喂海绵，SHAKE 反压时保持不前进。
            S_D_ABS: begin
                if (!ph) begin
                    ph <= 1'b1;
                end else begin
                    fsm_siv <= 1'b1;
                    fsm_sid <= abs_byte;
                    if (fsm_siv && sha_in_ready) begin
                        fsm_siv <= 1'b0;
                        if (abs_last) begin
                            st <= S_D_GAP;
                        end else begin
                            ai <= ai + 14'd1; ph <= 1'b0;
                        end
                    end
                end
            end
            S_D_GAP: st <= S_D_FLU;
            S_D_FLU: begin fsm_sif <= 1'b1; cnt <= 8'd0; st <= S_D_SQ; end

            // 挤 64 字节，低地址先出、从高位塞右移，进 μ 或 ρ''。
            S_D_SQ: begin
                fsm_sor <= 1'b1;
                if (sha_out_valid) begin
                    if (!dsel) mu    <= {sha_out_data, mu[511:8]};
                    else       rhopp <= {sha_out_data, rhopp[511:8]};
                    if (cnt == 8'd63) begin
                        fsm_sor <= 1'b0;
                        if (!dsel) begin
                            dsel <= 1'b1;      // μ 好了，接着算 ρ''
                            st <= S_D_GO;
                        end else begin
                            // ρ'' 好了，进 ③ NTT prep
                            nstore <= 2'd0; poly <= 3'd0; cnt <= 8'd0; ph <= 1'b0;
                            st <= S_NT_LD;
                        end
                    end else begin
                        cnt <= cnt + 8'd1;
                    end
                end
            end

            // ---------- ③ ŝ₁/ŝ₂/t̂₀ = NTT(s₁/s₂/t₀)，就地覆盖 ----------
            // 遍历 nstore∈{s₁,s₂,t₀} × poly∈0..3，共 12 条。装载 / 写回都是两拍一个系数。
            S_NT_LD: begin
                if (!ph) begin
                    ph <= 1'b1;
                end else begin
                    if (cnt == 8'd255) begin cnt <= 8'd0; ph <= 1'b0; st <= S_NT_GO; end
                    else begin cnt <= cnt + 8'd1; ph <= 1'b0; end
                end
            end
            S_NT_GO: begin nt_start <= 1'b1; nt_inv <= 1'b0; nt_lowseen <= 1'b0; st <= S_NT_ST; end
            // 「done 是电平」坑（KeyGen 坑表第 7 条）：先等 done 落一次，再等它起。
            S_NT_ST: begin
                if (!nt_done) nt_lowseen <= 1'b1;
                if (nt_lowseen && nt_done) begin cnt <= 8'd0; ph <= 1'b0; st <= S_NT_WB; end
            end
            S_NT_WB: begin
                if (!ph) begin
                    ph <= 1'b1;
                end else begin
                    if (cnt == 8'd255) begin
                        cnt <= 8'd0; ph <= 1'b0;
                        if (poly == 3'd3) begin
                            poly <= 3'd0;
                            if (nstore == 2'd2) begin
                                kappa <= 16'd0;    // 进拒绝循环，κ 从 0 起
                                st <= S_EM_GO;
                            end else if (nstore == 2'd3) begin
                                // ŷ = NTT(y) 做完 → ⑤ MAC
                                vi <= 3'd0; vj <= 3'd0; owner <= OWN_UNI;
                                st <= S_A_GO;
                            end else begin
                                nstore <= nstore + 2'd1; st <= S_NT_LD;
                            end
                        end else begin
                            poly <= poly + 3'd1; st <= S_NT_LD;
                        end
                    end else begin cnt <= cnt + 8'd1; ph <= 1'b0; end
                end
            end

            // ---------- ④ y = ExpandMask(ρ'', κ+r)，r = poly = 0..ℓ-1 ----------
            // 拒绝循环入口。本里程碑只跑 κ=0 一轮，采出 y 存好、经 dbg 验，
            // 然后 done；⑤ 起再往下接 ŷ/w/…，并把 done 往后挪。
            S_EM_GO: begin
                owner <= OWN_EM;
                em_nonce <= kappa + {13'd0, poly};
                em_start <= 1'b1;
                st <= S_EM_WT;
            end
            S_EM_WT: if (em_done) begin cnt <= 8'd0; ph <= 1'b0; st <= S_EM_MV; end

            // 采样器读口同步，两拍一个系数：ph=0 摆地址，ph=1 写进 y[poly]。
            S_EM_MV: begin
                if (!ph) begin
                    ph <= 1'b1;
                end else begin
                    if (cnt == 8'd255) begin
                        cnt <= 8'd0; ph <= 1'b0;
                        if (poly == 3'd3) begin
                            poly <= 3'd0;
                            kappa <= kappa + 16'd4;   // κ += ℓ（采完 y 立即加，同 oracle）
                            owner <= OWN_FSM;
                            nstore <= 2'd3;           // ŷ = NTT(y) 就地覆盖
                            st <= S_NT_LD;
                        end else begin
                            poly <= poly + 3'd1; st <= S_EM_GO;
                        end
                    end else begin cnt <= cnt + 8'd1; ph <= 1'b0; end
                end
            end

            // ---------- ⑤ 对每个 i：acc[i]=Σ_j mont(Â[i][j]∘ŷ[j])；
            //            w=caddq(invNTT(reduce32(acc)))；(w0,w1)=Decompose(w) ----------
            // Â 现采现用（un_nonce=256·i+j），不存。逐系数 MAC，j==0 直接放、之后累加。
            S_A_GO: begin
                un_nonce <= {5'd0, vi, 5'd0, vj};   // 256·i + j
                un_start <= 1'b1;
                st <= S_A_WT;
            end
            S_A_WT: if (un_done) begin cnt <= 8'd0; ph <= 1'b0; st <= S_MAC; end
            S_MAC: begin
                if (!ph) begin
                    ph <= 1'b1;
                end else begin
                    if (cnt == 8'd255) begin
                        cnt <= 8'd0; ph <= 1'b0;
                        if (vj == 3'd3) begin
                            vj <= 3'd0;
                            owner <= OWN_FSM;   // MAC 完这一 i，进 invNTT（FSM 用 NTT 核）
                            st <= S_RED;
                        end else begin
                            vj <= vj + 3'd1; owner <= OWN_UNI; st <= S_A_GO;
                        end
                    end else begin cnt <= cnt + 8'd1; ph <= 1'b0; end
                end
            end
            // 装载：reduce32(acc[vi]) → NTT 写口（inverse）
            S_RED: begin
                if (!ph) begin ph <= 1'b1; end
                else begin
                    if (cnt == 8'd255) begin cnt <= 8'd0; ph <= 1'b0; st <= S_INV_GO; end
                    else begin cnt <= cnt + 8'd1; ph <= 1'b0; end
                end
            end
            S_INV_GO: begin nt_start <= 1'b1; nt_inv <= 1'b1; nt_lowseen <= 1'b0; st <= S_INV_ST; end
            S_INV_ST: begin
                if (!nt_done) nt_lowseen <= 1'b1;
                if (nt_lowseen && nt_done) begin cnt <= 8'd0; ph <= 1'b0; st <= S_DEC; end
            end
            // 写回：caddq(invNTT[cnt]) → decompose → w0[vi]/w1[vi]
            S_DEC: begin
                if (!ph) begin ph <= 1'b1; end
                else begin
                    if (cnt == 8'd255) begin
                        cnt <= 8'd0; ph <= 1'b0;
                        if (vi == 3'd3) begin
                            vi <= 3'd0;
                            st <= S_FIN;        // 本里程碑到此（⑥ 起接 c̃/c）
                        end else begin
                            vi <= vi + 3'd1;
                            vj <= 3'd0; owner <= OWN_UNI;
                            st <= S_A_GO;       // 下一个 i 的 MAC
                        end
                    end else begin cnt <= cnt + 8'd1; ph <= 1'b0; end
                end
            end

            S_FIN: begin done <= 1'b1; st <= S_IDLE; end
            default: st <= S_IDLE;
            endcase
        end
    end

    // ================= 海绵接口三选一（组合）=================
    always @(*) begin
        case (owner)
        OWN_EM: begin
            sha_start = em_ss; sha_rate = em_sr; sha_suffix = em_su;
            sha_in_valid = em_siv; sha_in_data = em_sid; sha_in_flush = em_sif;
            sha_out_ready = em_sor;
        end
        OWN_UNI: begin
            sha_start = un_ss; sha_rate = un_sr; sha_suffix = un_su;
            sha_in_valid = un_siv; sha_in_data = un_sid; sha_in_flush = un_sif;
            sha_out_ready = un_sor;
        end
        default: begin
            sha_start = fsm_ss; sha_rate = fsm_sr; sha_suffix = fsm_su;
            sha_in_valid = fsm_siv; sha_in_data = fsm_sid; sha_in_flush = fsm_sif;
            sha_out_ready = fsm_sor;
        end
        endcase
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
        // ② μ 支吸收 ctx/msg 时，按 ai 摆读地址（ai 跨两拍稳定，组合驱动即可）
        msg_raddr = msg_off[12:0];
        ctx_raddr = ctx_off[7:0];

        eu_clr = 1'b0; eu_iv = 1'b0; eu_or = 1'b0;
        tu_clr = 1'b0; tu_iv = 1'b0; tu_or = 1'b0;

        nt_we = 1'b0; nt_waddr = 8'd0; nt_wdata = 32'd0; nt_raddr = cnt;

        y_we = 1'b0; y_waddr = 10'd0; y_din = 32'd0;
        y_raddr = {dbg_sel[1:0], dbg_idx};    // 默认给调试口
        em_rd_addr = cnt;
        ac_we = 1'b0; ac_waddr = 10'd0; ac_din = 32'd0;
        ac_raddr = {dbg_sel[1:0], dbg_idx};
        w0_we = 1'b0; w0_waddr = 10'd0; w0_din = 32'd0;
        w0_raddr = {dbg_sel[1:0], dbg_idx};
        w1_we = 1'b0; w1_waddr = 10'd0; w1_din = 6'd0;
        w1_raddr = {dbg_sel[1:0], dbg_idx};
        un_rd_addr = cnt;

        // S_UNP_I：进循环前清两个累加器
        if (st == S_UNP_I) begin eu_clr = 1'b1; tu_clr = 1'b1; end

        // ④ 采样结果 → y[poly]（两拍相位：ph=0 摆 em 读地址，ph=1 写 y）
        if (st == S_EM_MV) begin
            em_rd_addr = cnt;
            if (ph) begin y_we = 1'b1; y_waddr = {poly[1:0], cnt}; y_din = em_rd_data; end
        end

        // ⑤ MAC：Â[cnt]·ŷ[vj][cnt] 累加到 acc[vi][cnt]（j==0 直接放）
        if (st == S_MAC) begin
            un_rd_addr = cnt;
            y_raddr    = {vj[1:0], cnt};
            ac_raddr   = {vi[1:0], cnt};
            if (ph) begin
                ac_we    = 1'b1;
                ac_waddr = {vi[1:0], cnt};
                ac_din   = (vj == 3'd0) ? mac_mont : (ac_dout + mac_mont);
            end
        end
        // ⑤ 装载：reduce32(acc[vi]) → invNTT 写口
        if (st == S_RED) begin
            ac_raddr = {vi[1:0], cnt};
            if (ph) begin nt_we = 1'b1; nt_waddr = cnt; nt_wdata = red_out; end
        end
        // ⑤ 写回：caddq(invNTT[cnt]) → decompose → w0[vi]/w1[vi]
        if (st == S_DEC) begin
            nt_raddr = cnt;
            if (ph) begin
                w0_we = 1'b1; w0_waddr = {vi[1:0], cnt}; w0_din = dec_a0;
                w1_we = 1'b1; w1_waddr = {vi[1:0], cnt}; w1_din = dec_a1;
            end
        end

        // ③/⑤a NTT 装载：选中 store[poly] → NTT 写口（nstore：0=s₁,1=s₂,2=t₀,3=y）
        if (st == S_NT_LD) begin
            case (nstore)
                2'd0: s1_raddr = {poly[1:0], cnt};
                2'd1: s2_raddr = {poly[1:0], cnt};
                2'd2: t0_raddr = {poly[1:0], cnt};
                default: y_raddr = {poly[1:0], cnt};
            endcase
            if (ph) begin nt_we = 1'b1; nt_waddr = cnt; nt_wdata = store_dout; end
        end
        // ③/⑤a NTT 写回：NTT 读口 → 选中 store[poly]
        if (st == S_NT_WB) begin
            nt_raddr = cnt;
            if (ph) begin
                case (nstore)
                    2'd0: begin s1_we = 1'b1; s1_waddr = {poly[1:0], cnt}; s1_din = nt_rdata; end
                    2'd1: begin s2_we = 1'b1; s2_waddr = {poly[1:0], cnt}; s2_din = nt_rdata; end
                    2'd2: begin t0_we = 1'b1; t0_waddr = {poly[1:0], cnt}; t0_din = nt_rdata; end
                    default: begin y_we = 1'b1; y_waddr = {poly[1:0], cnt}; y_din = nt_rdata; end
                endcase
            end
        end

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
