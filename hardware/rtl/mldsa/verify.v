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

// ============================================================================
// 【参数化：44 / 65 / 87】
// ============================================================================
//   ML-DSA-44  k=4 ℓ=4 τ=39 γ₁=2¹⁷ γ₂=(q−1)/88 ω=80 β=78  c̃=32 pk=1312 σ=2420
//   ML-DSA-65  k=6 ℓ=5 τ=49 γ₁=2¹⁹ γ₂=(q−1)/32 ω=55 β=196 c̃=48 pk=1952 σ=3309
//   ML-DSA-87  k=8 ℓ=7 τ=60 γ₁=2¹⁹ γ₂=(q−1)/32 ω=75 β=120 c̃=64 pk=2592 σ=4627
// 分叉与 Sign 同源：γ₂ 选 use_hint 的 MODE；γ₁ 决定 z 解包位宽（18/20）；
// γ₂ 决定 w1Encode 位宽（6/4）。存储与位宽按最大的 87 开。
// ⚠️ 参数集是**运行时选**的：K/ℓ/τ/γ₁/γ₂/ω/β/c̃ 全部由 start 那一拍锁存的
//    pset 译码（mldsa_params）。存储一律按最大的 87 开。
module mldsa_verify (
    input  wire        clk,
    input  wire        rst_n,

    // ---- 擦除广播（engine 里那一台擦除机，见 mldsa_engine.v 文件头）----
    // wipe 期间本核在复位里，下面每一块 ram_dp 的 B 口被强制成
    // addr=wipe_addr、we=1、din=0 —— BRAM 不因复位清零，必须真写一遍。
    input  wire        wipe,
    input  wire [12:0] wipe_addr,

    input  wire [1:0]  pset,          // 0=44 1=65 2=87，start 那一拍锁存
    input  wire        start,

    // ---- 输入缓冲：start 之前由测试台按字节预载 ----
    input  wire        pk_wr_en,
    input  wire [12:0] pk_wr_addr,    // pk：1312 / 1952 / 2592 字节
    input  wire [7:0]  pk_wr_data,
    input  wire        sig_wr_en,
    input  wire [12:0] sig_wr_addr,   // σ：2420 / 3309 / 4627 字节
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
    // 两个 c̃ 端口固定 **512 位**（c̃ 最长 64 字节），只有低 ctb 字节有效
    output reg [511:0] ctilde,         // σ 里的 c̃
    output reg [511:0] ctilde_p,       // 算出来的 c̃'
    output reg [511:0] tr_out,
    output reg [511:0] mu,
    output reg         zbad,           // ‖z‖∞ ≥ γ₁−β
    output reg         hbad,           // hint 编码结构非法

    // ---- 调试读口：dbg_sel[5:2] 选组，[1:0] 选第几条 ----
    //   0 z(后被 NTT 覆盖成 ẑ)  1 t₁(后 t̂₁)  2 hint 位  3 c(后 ĉ)  4 acc
    input  wire [6:0]  dbg_sel,   // [6:3]=组，[2:0]=第几条多项式
    input  wire [7:0]  dbg_idx,
    output wire signed [31:0] dbg_coef
);
    // ================= ML-DSA-44 常量 =================
    localparam integer D = 13;
    // ---- 运行时参数集配置 ----
    // 空闲时跟着输入走、一离开 S_IDLE 就冻结（理由见 keygen 同一段注释）
    reg  [1:0] pset_r;
    reg        busy_r;
    wire [1:0] pset_eff = busy_r ? pset_r : pset;
    wire [3:0]  cfg_k, cfg_km1, cfg_lm1;   // verify 只用 km1/lm1，不需要 l
    wire [6:0]  cfg_tau, cfg_ctb;
    wire [7:0]  cfg_omega, cfg_w1b;
    wire        cfg_g2mode;
    wire [4:0]  cfg_zbits, cfg_w1bits;
    wire signed [31:0] cfg_gamma1;
    wire [31:0] cfg_zbound;
    wire [12:0] cfg_sig_h0;
    wire [13:0] cfg_pklen;
    mldsa_params u_par (
        .pset(pset_eff),
        .k(cfg_k), .l(), .eta(), .tau(cfg_tau), .omega(cfg_omega),
        .ctb(cfg_ctb), .g2mode(cfg_g2mode),
        .km1(cfg_km1), .lm1(cfg_lm1), .lkm1(),
        .ebits(), .peb(), .zbits(cfg_zbits), .zb(), .w1bits(cfg_w1bits), .w1b(cfg_w1b),
        .gamma1(cfg_gamma1), .gamma2(), .eta_s(),
        .zbound(cfg_zbound), .r0bound(), .ct0bound(),
        .sk_s2(), .sk_t0(), .sklen(), .pklen(cfg_pklen),
        .siglen(), .sig_h0(cfg_sig_h0));

    wire signed [31:0] G1_S = cfg_gamma1;
    wire [11:0] HCLR_N = {cfg_k, 8'd0} - 12'd1;   // k·256 − 1
    wire [12:0] SIG_Z0 = {6'd0, cfg_ctb};
    wire [12:0] SIG_H0 = cfg_sig_h0;
    wire [7:0]  OMEGA  = cfg_omega;
    localparam [12:0] PK_T1  = 13'd32;      // pk 里 t₁ 起点
    // ‖z‖∞ 的界：γ₁−β
    wire [31:0] ZBOUND = cfg_zbound;
    wire [3:0] LM1 = cfg_lm1, KM1 = cfg_km1;

    // ========================================================================
    // 【c̃ 的比对只看低 ctb 字节 —— 高位是**上一次运算**留下的】
    // ========================================================================
    // ctilde / ctilde_p 是定宽 512 位（c̃ 最长 64 字节），而**每次只写低 ctb
    // 字节**（44/65/87 → 32/48/64）。所以整 512 位比一刀切下去，比的是
    // "这一次的 c̃" ‖ "上一次留下的高位"。
    //
    // ⚠️ 板上抓到的就是这个（2026-08-16，开发形态位流，每套 300 次）：
    //        纯 Verify、不夹同参数集的运算
    //            ML-DSA-44 失败 300/300   ML-DSA-65 失败 300/300
    //            ML-DSA-87 失败   0/300
    //    87 恒对，因为 ctb=64、64 个字节全写，**它根本没有"高位"**；
    //    而 44/65 的高位只要被一次**判否**的大参数集 Verify 弄脏
    //    （c̃' ≠ c̃ ⇒ 两边高位不等），之后每一条合法签名都被判否，
    //    直到又来一条 87 把 64 字节整个覆盖掉。100%/0% 的确定性由此而来，
    //    与时序、温度都无关。
    //
    //    仿真一直是绿的，是因为**跨参数集那条用例只用 result=pass 的向量**：
    //    通过的那些 c̃'==c̃，连高位一起相等，脏不起来。板上跑的是完整
    //    sigver 向量集（45 条里 36 条 fail），于是天天踩。
    //
    // 修法不是"在别处把高位清干净"，而是**让判定根本不看高位**：c̃ 就是
    // ctb 个字节，多出来的那些字节在这一次运算里没有任何含义。
    // （若靠"入口清零"来维持"高位恒为 0"这条不变量，就得给 1024 个触发器
    //  各加一条同步清零 —— 87 那版 CLB 已经 95.98%，而且那样一来判定的正确性
    //  仍旧依赖别处维持的不变量，而不是它自己。）
    //
    // 只放宽、不收紧：忽略的字节永远不属于 c̃，所以不会把一条**该拒**的签名
    // 放过去（低 ctb 字节仍然逐字节比）。
    wire [63:0] ct_eq;
    genvar gi;
    generate
        for (gi = 0; gi < 64; gi = gi + 1) begin : g_ct_cmp
            assign ct_eq[gi] = ({25'd0, cfg_ctb} <= gi)
                               || (ctilde_p[gi*8 +: 8] == ctilde[gi*8 +: 8]);
        end
    endgenerate
    wire ct_match = &ct_eq;

    localparam [5:0]
        S_IDLE  = 6'd0,
        S_HCLR  = 6'd1,      // 清 h 存储（BRAM 无复位，残留会造成假阳性）
        S_CT    = 6'd2,      // c̃ ← sig[0..31]
        S_ZU    = 6'd3,      // z 位解包（18 位，γ₁−v）+ ‖z‖∞
        S_HC    = 6'd4,      // 读 k 个累计计数字节
        S_HI    = 6'd5,      // 扫下标、置 h 位、查严格递增
        S_HP    = 6'd6,      // 查填充区全零
        S_T1    = 6'd7,      // t₁ 位解包（10 位）
        // ② tr=H(pk)、μ=H(tr‖M')：共用一套吸收/挤压状态（hsel 选支）
        S_D_GO  = 6'd8,
        S_D_ABS = 6'd9,
        S_D_GAP = 6'd10,
        S_D_FLU = 6'd11,
        S_D_SQ  = 6'd12,
        // ③ c=SampleInBall(c̃)，以及 ĉ/ẑ/t̂₁ 三组 NTT
        S_SIB_GO = 6'd13,
        S_SIB_WT = 6'd14,
        S_SIB_MV = 6'd15,
        S_NT_LD  = 6'd16,
        S_NT_GO  = 6'd17,
        S_NT_ST  = 6'd18,
        S_NT_WB  = 6'd19,
        // ④ 对每个 i：acc=Σ_j Â∘ẑ − ĉ∘t̂₁；w'=caddq(invNTT(reduce32(acc)))；
        //    w'₁=UseHint(h,w')；6 位打包进 w1pk；最后 c̃'=H(μ‖w1pk) 比对
        S_A_GO   = 6'd20,
        S_A_WT   = 6'd21,
        S_MAC    = 6'd22,
        S_CT1    = 6'd23,
        S_RED    = 6'd24,
        S_INV_GO = 6'd25,
        S_INV_ST = 6'd26,
        S_UH     = 6'd27,
        S_FIN   = 6'd63;

    reg [5:0] st;

    // ================= 输入缓冲 =================
    reg  [12:0] pk_raddr;  wire [7:0] pk_rdata;
    ram_dp #(.DW(8), .AW(13)) u_pk (
        .clk(clk), .a_we(pk_wr_en), .a_addr(pk_wr_addr), .a_din(pk_wr_data),
        .a_dout(), .b_we(wipe), .b_addr(wipe ? wipe_addr[12:0] : (pk_raddr)), .b_din(wipe ? 8'd0 : (8'd0)), .b_dout(pk_rdata));
    reg  [12:0] sig_raddr; wire [7:0] sig_rdata;
    ram_dp #(.DW(8), .AW(13)) u_sig (
        .clk(clk), .a_we(sig_wr_en), .a_addr(sig_wr_addr), .a_din(sig_wr_data),
        .a_dout(), .b_we(wipe), .b_addr(wipe ? wipe_addr[12:0] : (sig_raddr)), .b_din(wipe ? 8'd0 : (8'd0)), .b_dout(sig_rdata));
    reg  [12:0] msg_raddr; wire [7:0] msg_rdata;
    ram_dp #(.DW(8), .AW(13)) u_msg (
        .clk(clk), .a_we(msg_wr_en), .a_addr(msg_wr_addr), .a_din(msg_wr_data),
        .a_dout(), .b_we(wipe), .b_addr(wipe ? wipe_addr[12:0] : (msg_raddr)), .b_din(wipe ? 8'd0 : (8'd0)), .b_dout(msg_rdata));
    reg  [7:0]  ctx_raddr; wire [7:0] ctx_rdata;
    ram_dp #(.DW(8), .AW(8)) u_ctx (
        .clk(clk), .a_we(ctx_wr_en), .a_addr(ctx_wr_addr), .a_din(ctx_wr_data),
        .a_dout(), .b_we(wipe), .b_addr(wipe ? wipe_addr[7:0] : (ctx_raddr)), .b_din(wipe ? 8'd0 : (8'd0)), .b_dout(ctx_rdata));

    // ================= 系数存储 =================
    reg         z_we;  reg [10:0] z_waddr;  reg signed [31:0] z_din;  reg [10:0] z_raddr;
    wire signed [31:0] z_dout;
    ram_dp #(.DW(32), .AW(11)) u_z (
        .clk(clk), .a_we(z_we), .a_addr(z_waddr), .a_din(z_din), .a_dout(),
        .b_we(wipe), .b_addr(wipe ? wipe_addr[10:0] : (z_raddr)), .b_din(wipe ? 32'd0 : (32'd0)), .b_dout(z_dout));
    reg         t1_we; reg [10:0] t1_waddr; reg signed [31:0] t1_din; reg [10:0] t1_raddr;
    wire signed [31:0] t1_dout;
    ram_dp #(.DW(32), .AW(11)) u_t1 (
        .clk(clk), .a_we(t1_we), .a_addr(t1_waddr), .a_din(t1_din), .a_dout(),
        .b_we(wipe), .b_addr(wipe ? wipe_addr[10:0] : (t1_raddr)), .b_din(wipe ? 32'd0 : (32'd0)), .b_dout(t1_dout));
    // hint：k×256 位
    reg         h_we;  reg [10:0] h_waddr;  reg h_din;  reg [10:0] h_raddr;
    wire        h_dout;
    ram_dp #(.DW(1), .AW(11)) u_h (
        .clk(clk), .a_we(h_we), .a_addr(h_waddr), .a_din(h_din), .a_dout(),
        .b_we(wipe), .b_addr(wipe ? wipe_addr[10:0] : (h_raddr)), .b_din(wipe ? 1'd0 : (1'b0)), .b_dout(h_dout));

    // c 存储：256×32（③ 存 c，随后就地 NTT 成 ĉ）
    reg         c_we;  reg [7:0] c_waddr;  reg signed [31:0] c_din;  reg [7:0] c_raddr;
    wire signed [31:0] c_dout;
    ram_dp #(.DW(32), .AW(8)) u_c (
        .clk(clk), .a_we(c_we), .a_addr(c_waddr), .a_din(c_din), .a_dout(),
        .b_we(wipe), .b_addr(wipe ? wipe_addr[7:0] : (c_raddr)), .b_din(wipe ? 32'd0 : (32'd0)), .b_dout(c_dout));

    assign dbg_coef =
          (dbg_sel[6:3] == 4'd0) ? z_dout
        : (dbg_sel[6:3] == 4'd1) ? t1_dout
        : (dbg_sel[6:3] == 4'd2) ? {31'd0, h_dout}
        : (dbg_sel[6:3] == 4'd3) ? c_dout
        : 32'd0;

    // ================= 海绵归属（FSM ↔ SampleInBall ↔ ExpandA）=================
    // 只在换手方空闲时切（KeyGen 坑表第 1 条）。
    localparam [1:0] OWN_FSM = 2'd0, OWN_SIB = 2'd1, OWN_UNI = 2'd2;
    reg [1:0] owner;
    reg       fsm_ss, fsm_siv, fsm_sif, fsm_sor;
    reg [7:0] fsm_sr, fsm_su, fsm_sid;

    // ---- SampleInBall（复用 Sign 建好的模块）----
    reg         sb_start;
    wire        sb_done;
    reg  [7:0]  sb_rd_addr;
    wire signed [31:0] sb_rd_data;
    wire        sb_ss, sb_siv, sb_sif, sb_sor;
    wire [7:0]  sb_sr, sb_su, sb_sid;
    // seed 端口已固定 512 位，c̃ 短于 64 字节时高位补零（模块只读前 ctb 字节）
    mldsa_sample_in_ball u_sib (
        .clk(clk), .rst_n(rst_n), .tau(cfg_tau), .ctb(cfg_ctb),
        .start(sb_start), .seed(ctilde), .done(sb_done),
        .sha_start(sb_ss), .sha_rate(sb_sr), .sha_suffix(sb_su),
        .sha_in_valid(sb_siv), .sha_in_data(sb_sid), .sha_in_flush(sb_sif),
        .sha_in_ready(sha_in_ready && (owner == OWN_SIB)),
        .sha_out_valid(sha_out_valid && (owner == OWN_SIB)),
        .sha_out_ready(sb_sor), .sha_out_data(sha_out_data),
        .rd_addr(sb_rd_addr), .rd_data(sb_rd_data));

    // ---- ExpandA 均匀采样器（④：Â 现采现用，seed=ρ、nonce=256·i+j）----
    reg  [255:0] rho;
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

    // ---- 累加缓冲（一次只处理一个 i）与 w1pk 缓冲 ----
    reg         ac_we; reg [7:0] ac_waddr; reg signed [31:0] ac_din; reg [7:0] ac_raddr;
    wire signed [31:0] ac_dout;
    ram_dp #(.DW(32), .AW(8)) u_acc (
        .clk(clk), .a_we(ac_we), .a_addr(ac_waddr), .a_din(ac_din), .a_dout(),
        .b_we(wipe), .b_addr(wipe ? wipe_addr[7:0] : (ac_raddr)), .b_din(wipe ? 32'd0 : (32'd0)), .b_dout(ac_dout));
    reg         wp_we; reg [9:0] wp_waddr; reg [7:0] wp_din; reg [9:0] wp_raddr;
    wire [7:0]  wp_dout;
    reg  [9:0]  wp_ptr;
    ram_dp #(.DW(8), .AW(10)) u_wp (
        .clk(clk), .a_we(wp_we), .a_addr(wp_waddr), .a_din(wp_din), .a_dout(),
        .b_we(wipe), .b_addr(wipe ? wipe_addr[9:0] : (wp_raddr)), .b_din(wipe ? 8'd0 : (8'd0)), .b_dout(wp_dout));

    // ---- NTT 核（③：c/z/t₁ 三组正变换，就地覆盖）----
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

    // ======================================================================
    // ④ 逐点乘：S_MAC 用 Â×ẑ，S_CT1 用 ĉ×t̂₁ —— **流水版**
    // ======================================================================
    //
    // 原来这里是一条全组合的 mldsa_mont_reduce。把 NTT 蝶形流水化之后它就是
    // verify 剩下的关键路径 —— ML-DSA-87 post-route 实测 WNS = −3.487ns，
    // 路径 u_z →（三次 32×32 乘）→ u_acc，逻辑层级 33。
    // 与 keygen/sign 的那几条是同一个形状，用同一个 mldsa_mont_mul_pipe。
    //
    // 时序：第 T 拍发读地址 → 第 T+1 拍数据出来、拉 in_valid（写回地址与
    // 旧累加值一起进 tag）→ 第 T+6 拍结果回来。每拍发一个，比原来两拍一个快一倍。
    // 两段之间必须排空：S_CT1 要读 S_MAC 刚写完的累加器。
    // （流水乘法链的例化在控制寄存器之后 —— 它要用 cnt）
    // reduce32 在 invNTT **之前**（照 oracle 的顺序，别挪到后面）
    wire signed [31:0] red_out;
    mldsa_reduce32 u_red (.a(ac_dout), .r(red_out));
    // caddq 后才能喂 use_hint（decompose 假定输入已在 [0,q)）
    wire signed [31:0] cad_out;
    mldsa_caddq u_cad (.a(nt_rdata), .r(cad_out));
    wire [5:0] uh_a1;
    mldsa_use_hint u_uh (.mode(cfg_g2mode), .a(cad_out), .hint(h_dout), .a1_out(uh_a1));
    // ④ 的写回链原来是一整条组合路径：
    //   invNTT 结果(BRAM) → caddq → UseHint(含 decompose 的乘法与比较)
    //   → w₁ → bitpack 的累加器
    // 逻辑层级 33，是 verify（以及整个 engine）的关键路径。
    // 与 Sign ⑨ 那条同形，解法也一样：从中间切一刀，w₁ 先进寄存器再喂打包器。
    // 代价是 ④ 每个系数从 2 拍变 3 拍，只影响这一段。
    reg  [5:0] w1_r;
    reg  [1:0] uph;      // ④ 专用三拍相位

    // ---- w1Encode：6 位/系数打包进 w1pk ----
    reg         p6_clr, p6_iv;
    wire        p6_ir, p6_ov;
    wire [7:0]  p6_ob;
    mldsa_bitpack u_p6 (
        .clk(clk), .rst_n(rst_n), .clr(p6_clr), .w(cfg_w1bits),
        .in_val({14'd0, w1_r}), .in_valid(p6_iv), .in_ready(p6_ir),
        .out_byte(p6_ob), .out_valid(p6_ov));

    // ================= 位解包器 =================
    // z：18 位 → γ₁−v；t₁：10 位 → 直接用
    reg         zu_clr, zu_iv, zu_or;
    wire        zu_ir, zu_ov;
    wire [19:0] zu_val;
    mldsa_bitunpack u_zu (
        .clk(clk), .rst_n(rst_n), .clr(zu_clr), .w(cfg_zbits),
        .in_byte(sig_rdata), .in_valid(zu_iv), .in_ready(zu_ir),
        .out_val(zu_val), .out_valid(zu_ov), .out_ready(zu_or));
    reg         tu_clr, tu_iv, tu_or;
    wire        tu_ir, tu_ov;
    wire [19:0] tu_val;
    mldsa_bitunpack u_tu (
        .clk(clk), .rst_n(rst_n), .clr(tu_clr), .w(5'd10),
        .in_byte(pk_rdata), .in_valid(tu_iv), .in_ready(tu_ir),
        .out_val(tu_val), .out_valid(tu_ov), .out_ready(tu_or));

    wire signed [31:0] z_coef  = G1_S - $signed({12'd0, zu_val});
    wire signed [31:0] t1_coef = $signed({12'd0, tu_val});
    // ‖z‖∞：z 已居中，直接取绝对值比
    wire [31:0] z_abs = z_coef[31] ? (-z_coef) : z_coef;
    wire        z_over = (z_abs >= ZBOUND);

    // ================= 控制寄存器 =================
    reg [7:0]  cnt;
    reg        ph;
    reg [3:0]  poly;
    reg [12:0] rdp;        // 读指针（喂解包器的字节）
    reg        feed;
    reg [10:0] hclr;       // 清 h 的计数
    // HintBitUnpack 用
    reg [7:0]  hcnt [0:7];   // 每条的累计计数
    reg [7:0]  hidx;           // 运行下标
    reg [7:0]  hfirst;         // 本条起点（判"严格递增"用）
    reg [7:0]  hprev;          // 上一个下标
    integer    ii;

    // ②③ 用
    reg [13:0] ai;             // 吸收字节指针（μ 支最长 66+ctx+msg，14 位）
    reg [1:0]  hsel;           // 0 = tr(吸收 pk)，1 = μ，2 = c̃'
    reg [1:0]  nstore;         // NTT 对象：0=c, 1=z, 2=t₁
    wire [3:0] nt_last = (nstore == 2'd1) ? LM1 : KM1;   // z 是 ℓ 条，t₁ 是 k 条
    reg        nt_lowseen;     // 「done 是电平」：先见它落一次再等它起
    reg [3:0]  vi, vj;         // ④ 的 i / j

    // ---- ④ 逐点乘的流水链（接上面 259 行处那条注释）----
    reg  mm_last, mm_iv;
    reg  [7:0] mm_addr_d;
    wire mm_issue = !mm_last && (st == S_MAC || st == S_CT1);
    wire mm_is_ct1 = (st == S_CT1);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            mm_iv <= 1'b0; mm_addr_d <= 8'd0;
        end else begin
            mm_iv     <= mm_issue;
            mm_addr_d <= cnt;
        end
    end

    // tag = {写回地址 8 位, 旧累加值 32 位}。
    // ⚠️ ac_dout 取当拍的值、地址取上一拍锁存的 —— 两者都对应第 T 拍发出的那个系数。
    wire        mm_ov, mm_busy;
    wire [39:0] mm_otag;
    wire signed [31:0] mm_t;
    mldsa_mont_mul_pipe #(.TAGW(40)) u_mm (
        .clk(clk), .rst_n(rst_n),
        .in_valid(mm_iv), .in_tag({mm_addr_d, ac_dout}),
        .x(mm_is_ct1 ? c_dout : $signed({9'd0, un_rd_data})),
        .y(mm_is_ct1 ? t1_dout : z_dout),
        .out_valid(mm_ov), .out_tag(mm_otag), .t_out(mm_t),
        .pipe_busy(mm_busy));

    wire [7:0]  mm_owaddr = mm_otag[39:32];
    wire signed [31:0] mm_oacc = $signed(mm_otag[31:0]);
    wire        mm_empty  = ~mm_iv & ~mm_busy;

    wire [13:0] PKLEN = cfg_pklen;

    // ---- tr 支的吸收源：整个 pk ----
    // ---- μ 支的吸收源：tr(64) ‖ 0x00 ‖ |ctx| ‖ ctx ‖ msg ----
    wire [13:0] thr_ctxend = 14'd66 + {6'd0, ctx_len};
    wire [13:0] thr_mu     = thr_ctxend + msg_len;
    wire [13:0] ctx_off    = ai - 14'd66;
    wire [13:0] msg_off    = ai - thr_ctxend;
    reg  [7:0]  mu_byte;
    always @(*) begin
        if (ai < 14'd64)            mu_byte = tr_out[ai[5:0]*8 +: 8];
        else if (ai == 14'd64)      mu_byte = 8'd0;
        else if (ai == 14'd65)      mu_byte = ctx_len;
        else if (ai <  thr_ctxend)  mu_byte = ctx_rdata;
        else                        mu_byte = msg_rdata;
    end

    // ---- c̃' 支的吸收源：μ(64) ‖ w1pk(k·192=768) = 832 字节 ----
    wire [7:0] ct_byte = (ai < 14'd64) ? mu[ai[5:0]*8 +: 8] : wp_dout;

    wire [7:0]  abs_byte  = (hsel == 2'd0) ? pk_rdata
                          : (hsel == 2'd1) ? mu_byte : ct_byte;
    wire [13:0] abs_total = (hsel == 2'd0) ? PKLEN
                          : (hsel == 2'd1) ? thr_mu : (14'd64 + {10'd0, cfg_k} * {6'd0, cfg_w1b});
    wire        abs_last  = (ai == abs_total - 14'd1);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            st <= S_IDLE; done <= 1'b0; valid <= 1'b0;
            cnt <= 8'd0; ph <= 1'b0; poly <= 4'd0; rdp <= 13'd0; feed <= 1'b0;
            pset_r <= 2'd0; busy_r <= 1'b0; uph <= 2'd0; w1_r <= 6'd0;
            mm_last <= 1'b0;
            hclr <= 11'd0; hidx <= 8'd0; hfirst <= 8'd0; hprev <= 8'd0;
            ctilde <= 512'd0; ctilde_p <= 512'd0; tr_out <= 512'd0; mu <= 512'd0;
            zbad <= 1'b0; hbad <= 1'b0;
            ai <= 14'd0; hsel <= 2'd0; nstore <= 2'd0; nt_lowseen <= 1'b0;
            owner <= OWN_FSM; sb_start <= 1'b0;
            nt_start <= 1'b0; nt_inv <= 1'b0;
            vi <= 4'd0; vj <= 4'd0; un_start <= 1'b0; un_nonce <= 16'd0;
            rho <= 256'd0; wp_ptr <= 10'd0;
            fsm_ss <= 1'b0; fsm_sr <= 8'd136; fsm_su <= 8'h1F;
            fsm_siv <= 1'b0; fsm_sid <= 8'd0; fsm_sif <= 1'b0; fsm_sor <= 1'b0;
            for (ii = 0; ii < 8; ii = ii + 1) hcnt[ii] <= 8'd0;   // 存储按最大的 k=8 开
        end else begin
            done <= 1'b0;
            fsm_ss <= 1'b0;
            fsm_sif <= 1'b0;
            nt_start <= 1'b0;
            sb_start <= 1'b0;
            un_start <= 1'b0;
            // w₁ 打包器每吐一字节，w1pk 落盘指针前进
            if (p6_ov) wp_ptr <= wp_ptr + 10'd1;

            case (st)
            S_IDLE: if (start) begin
                pset_r <= pset; busy_r <= 1'b1;
                zbad <= 1'b0; hbad <= 1'b0; valid <= 1'b0;
                hclr <= 11'd0; cnt <= 8'd0; ph <= 1'b0; poly <= 4'd0;
                st <= S_HCLR;
            end

            // ---------- 清 h 存储 ----------
            // ⚠️ 稀疏置位（只写 1、不写 0）意味着没被置到的位靠初值。BRAM 无复位口，
            // 连续验多条时上一条的 1 会残留 → 上一条的 hint 泄漏进这一条，可能让
            // **本该拒绝的签名通过**（假阳性）。所以入口必须显式清 k×256 位。
            S_HCLR: begin
                if (hclr == HCLR_N[10:0]) begin
                    cnt <= 8'd0; st <= S_CT;
                end else begin
                    hclr <= hclr + 11'd1;
                end
            end

            // ---------- c̃ ← sig[0..31]（同步读，两拍相位）----------
            S_CT: begin
                if (!ph) begin
                    ph <= 1'b1;
                end else begin
                    // 定宽 512 位、ctb 运行时 ⇒ 按下标写第 cnt 个字节
                    ctilde[cnt*8 +: 8] <= sig_rdata;
                    if (cnt == {1'd0, cfg_ctb} - 8'd1) begin
                        cnt <= 8'd0; ph <= 1'b0; poly <= 4'd0; feed <= 1'b0;
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
                        if (poly == LM1) begin
                            poly <= 4'd0; cnt <= 8'd0; ph <= 1'b0;
                            rdp <= SIG_H0 + {5'd0, OMEGA};   // 指向计数区
                            st <= S_HC;
                        end else begin
                            poly <= poly + 4'd1;
                        end
                    end else begin
                        cnt <= cnt + 8'd1;
                    end
                end else begin
                    if (!feed) begin
                        feed <= 1'b1;
                    end else begin
                        if (zu_ir) rdp <= rdp + 13'd1;
                        feed <= 1'b0;
                    end
                end
            end

            // ---------- HintBitUnpack ① 读 k 个累计计数字节 ----------
            S_HC: begin
                if (!ph) begin
                    ph <= 1'b1;
                end else begin
                    hcnt[cnt[2:0]] <= sig_rdata;
                    if (cnt == {4'd0, cfg_k} - 8'd1) begin
                        cnt <= 8'd0; ph <= 1'b0; poly <= 4'd0;
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
                if (hcnt[poly[2:0]] < hidx || hcnt[poly[2:0]] > OMEGA) begin
                    hbad <= 1'b1;
                    // 结构已非法，不再按它扫（继续跑到底，最后统一判 valid）
                    if (poly == KM1) begin cnt <= 8'd0; ph <= 1'b0; st <= S_HP; end
                    else begin poly <= poly + 4'd1; end
                end else if (hidx == hcnt[poly[2:0]]) begin
                    // 这一条扫完
                    if (poly == KM1) begin
                        cnt <= 8'd0; ph <= 1'b0; st <= S_HP;
                    end else begin
                        poly <= poly + 4'd1;
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
                    cnt <= 8'd0; ph <= 1'b0; poly <= 4'd0; feed <= 1'b0;
                    rdp <= PK_T1;
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
                        if (poly == KM1) begin
                            poly <= 4'd0;
                            hsel <= 2'd0;     // 进 ②：先算 tr = H(pk)
                            st <= S_D_GO;
                        end else begin
                            poly <= poly + 4'd1;
                        end
                    end else begin
                        cnt <= cnt + 8'd1;
                    end
                end else begin
                    if (!feed) begin
                        feed <= 1'b1;
                    end else begin
                        if (tu_ir) rdp <= rdp + 13'd1;
                        feed <= 1'b0;
                    end
                end
            end

            // ---------- ② tr = H(pk)、μ = H(tr‖M') ----------
            S_D_GO: begin
                owner <= OWN_FSM;
                fsm_sr <= 8'd136; fsm_su <= 8'h1F;   // SHAKE256
                fsm_ss <= 1'b1;
                ai <= 14'd0; ph <= 1'b0; cnt <= 8'd0;
                st <= S_D_ABS;
            end
            // 两拍一个字节：ph=0 摆 pk/ctx/msg 读地址，ph=1 数据到位、驱 valid。
            S_D_ABS: begin
                if (!ph) begin
                    ph <= 1'b1;
                end else begin
                    fsm_siv <= 1'b1;
                    fsm_sid <= abs_byte;
                    if (fsm_siv && sha_in_ready) begin
                        fsm_siv <= 1'b0;
                        // ρ = pk[0..31]：趁 tr 吸收 pk 时顺手截下来（ExpandA 的种子），
                        // 不必再单开一段读 pk。低地址字节先出，从高位塞右移。
                        if (hsel == 2'd0 && ai < 14'd32) rho <= {abs_byte, rho[255:8]};
                        if (abs_last) begin
                            st <= S_D_GAP;
                        end else begin
                            ai <= ai + 14'd1; ph <= 1'b0;
                        end
                    end
                end
            end
            // ⚠️ 吸收长度恰为 rate(136) 整数倍时，最后一字节填满块触发置换、in_ready 落下；
            // 此时拉 flush 会被整个忽略（只在核处于 S_ABSORB 且 in_valid 低时采样）
            // → 永远吸不完。所以等 in_ready 重新拉高再冲刷（Sign 实测坑第 6 条）。
            // μ 的吸收长度是变长的（66+|ctx|+|msg|），一定会有向量踩中。
            S_D_GAP: if (sha_in_ready) st <= S_D_FLU;
            S_D_FLU: begin fsm_sif <= 1'b1; cnt <= 8'd0; st <= S_D_SQ; end
            // 挤字节：tr/μ 挤 64，c̃' 挤 32
            S_D_SQ: begin
                fsm_sor <= 1'b1;
                if (sha_out_valid) begin
                    if (hsel == 2'd0)      tr_out   <= {sha_out_data, tr_out[511:8]};
                    else if (hsel == 2'd1) mu       <= {sha_out_data, mu[511:8]};
                    else                   ctilde_p[cnt*8 +: 8] <= sha_out_data;
                    if (cnt == ((hsel == 2'd2) ? ({1'd0, cfg_ctb} - 8'd1) : 8'd63)) begin
                        fsm_sor <= 1'b0;
                        if (hsel == 2'd0) begin
                            hsel <= 2'd1;          // tr 好了，接着算 μ
                            st <= S_D_GO;
                        end else if (hsel == 2'd1) begin
                            st <= S_SIB_GO;        // μ 好了，进 ③
                        end else begin
                            st <= S_FIN;           // c̃' 好了，判定
                        end
                    end else begin
                        cnt <= cnt + 8'd1;
                    end
                end
            end

            // ---------- ③a c = SampleInBall(c̃) ----------
            S_SIB_GO: begin owner <= OWN_SIB; sb_start <= 1'b1; st <= S_SIB_WT; end
            S_SIB_WT: if (sb_done) begin
                cnt <= 8'd0; ph <= 1'b0; owner <= OWN_FSM; st <= S_SIB_MV;
            end
            S_SIB_MV: begin
                if (!ph) begin
                    ph <= 1'b1;
                end else begin
                    if (cnt == 8'd255) begin
                        cnt <= 8'd0; ph <= 1'b0; nstore <= 2'd0; poly <= 4'd0;
                        st <= S_NT_LD;             // 进 ③b：ĉ = NTT(c)
                    end else begin cnt <= cnt + 8'd1; ph <= 1'b0; end
                end
            end

            // ---------- ③b ĉ/ẑ/t̂₁ 三组正变换（就地覆盖）----------
            // nstore：0=c（1 条）、1=z（ℓ 条）、2=t₁（k 条，装载时左移 D）
            S_NT_LD: begin
                if (!ph) begin ph <= 1'b1; end
                else begin
                    if (cnt == 8'd255) begin cnt <= 8'd0; ph <= 1'b0; st <= S_NT_GO; end
                    else begin cnt <= cnt + 8'd1; ph <= 1'b0; end
                end
            end
            S_NT_GO: begin nt_start <= 1'b1; nt_inv <= 1'b0; nt_lowseen <= 1'b0; st <= S_NT_ST; end
            S_NT_ST: begin
                if (!nt_done) nt_lowseen <= 1'b1;
                if (nt_lowseen && nt_done) begin cnt <= 8'd0; ph <= 1'b0; st <= S_NT_WB; end
            end
            S_NT_WB: begin
                if (!ph) begin ph <= 1'b1; end
                else begin
                    if (cnt == 8'd255) begin
                        cnt <= 8'd0; ph <= 1'b0;
                        if (nstore == 2'd0) begin
                            nstore <= 2'd1; poly <= 4'd0; st <= S_NT_LD;   // 接着 z
                        end else if (poly == nt_last) begin
                            poly <= 4'd0;
                            if (nstore == 2'd1) begin
                                nstore <= 2'd2; st <= S_NT_LD;             // 接着 t₁
                            end else begin
                                // 三组 NTT 都好了，进 ④
                                vi <= 4'd0; vj <= 4'd0; wp_ptr <= 10'd0;
                                owner <= OWN_UNI;
                                st <= S_A_GO;
                            end
                        end else begin
                            poly <= poly + 4'd1; st <= S_NT_LD;
                        end
                    end else begin cnt <= cnt + 8'd1; ph <= 1'b0; end
                end
            end

            // ---------- ④ 对每个 i：acc = Σ_j Â[i][j]∘ẑ[j] − ĉ∘t̂₁[i] ----------
            S_A_GO: begin
                // ⚠️ vi/vj 现在是 4 位，拼接必须配成 4+4：写成 {5'd0,vi,5'd0,vj} 会是
                // 18 位塞进 16 位、从高位截掉，nonce 全错（Â 采错 → c̃' 对不上）。
                un_nonce <= {4'd0, vi, 4'd0, vj};   // 256·i + j
                un_start <= 1'b1;
                st <= S_A_WT;
            end
            S_A_WT: if (un_done) begin cnt <= 8'd0; ph <= 1'b0; st <= S_MAC; end
            // 每拍发一个；256 个发完等流水排空，再换 vj（下一段要读这一段写的累加器）
            S_MAC: begin
                if (!mm_last) begin
                    if (cnt == 8'd255) mm_last <= 1'b1;
                    else               cnt <= cnt + 8'd1;
                end else if (mm_empty) begin
                    cnt <= 8'd0; ph <= 1'b0; mm_last <= 1'b0;
                    if (vj == LM1) begin
                        vj <= 4'd0; owner <= OWN_FSM; st <= S_CT1;
                    end else begin
                        vj <= vj + 4'd1; owner <= OWN_UNI; st <= S_A_GO;
                    end
                end
            end
            // acc −= mont(ĉ∘t̂₁[vi])
            S_CT1: begin
                if (!mm_last) begin
                    if (cnt == 8'd255) mm_last <= 1'b1;
                    else               cnt <= cnt + 8'd1;
                end else if (mm_empty) begin
                    cnt <= 8'd0; ph <= 1'b0; mm_last <= 1'b0; st <= S_RED;
                end
            end
            // reduce32(acc) → invNTT 写口
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
                if (nt_lowseen && nt_done) begin cnt <= 8'd0; ph <= 1'b0; st <= S_UH; end
            end
            // w' = caddq(invNTT)；w'₁ = UseHint(h[vi][cnt], w')；6 位打包进 w1pk
            // 打包器只在最开头 clr 一次（每条 192 字节字节对齐，连续打包与逐条等价；
            // 换条 clr 会抹掉比最后一个系数晚出的待吐字节 —— Sign 实测坑第 3 条）。
            // 三拍：0 摆地址 / 1 锁存 w₁ / 2 喂打包器（要等 p6_ir）
            S_UH: begin
                if (uph == 2'd0) begin
                    uph <= 2'd1;
                end else if (uph == 2'd1) begin
                    w1_r <= uh_a1;
                    uph  <= 2'd2;
                end else if (p6_ir) begin
                    uph <= 2'd0;
                    if (cnt == 8'd255) begin
                        cnt <= 8'd0; ph <= 1'b0;
                        if (vi == KM1) begin
                            vi <= 4'd0;
                            hsel <= 2'd2;          // 进 c̃' = H(μ‖w1pk)
                            st <= S_D_GO;
                        end else begin
                            vi <= vi + 4'd1; vj <= 4'd0; owner <= OWN_UNI;
                            st <= S_A_GO;
                        end
                    end else begin cnt <= cnt + 8'd1; ph <= 1'b0; end
                end
            end

            // 判定：c̃' == c̃ 且结构检查都通过
            S_FIN: begin
                done  <= 1'b1;
                // 两个 c̃ 都是定宽 512 位而**只有低 ctb 字节属于这一次运算**，
                // 高位是上一次留下的 —— 所以比的是 ct_match，不是整 512 位。
                // 那条整体比较在板上是 44/65 全红、87 全绿的成因，见 ct_eq 处。
                valid <= ct_match && !zbad && !hbad;
                busy_r <= 1'b0;      // 解冻配置，下一次 start 才好按新 pset 译码
                st <= S_IDLE;
            end
            default: st <= S_IDLE;
            endcase
        end
    end

    // ================= 海绵接口三选一（组合）=================
    always @(*) begin
        case (owner)
        OWN_SIB: begin
            sha_start = sb_ss; sha_rate = sb_sr; sha_suffix = sb_su;
            sha_in_valid = sb_siv; sha_in_data = sb_sid; sha_in_flush = sb_sif;
            sha_out_ready = sb_sor;
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
    always @(*) begin
        z_we = 1'b0; z_waddr = 10'd0; z_din = 32'd0;
        z_raddr = {dbg_sel[2:0], dbg_idx};
        t1_we = 1'b0; t1_waddr = 10'd0; t1_din = 32'd0;
        t1_raddr = {dbg_sel[2:0], dbg_idx};
        h_we = 1'b0; h_waddr = 10'd0; h_din = 1'b0;
        h_raddr = {dbg_sel[2:0], dbg_idx};
        zu_clr = 1'b0; zu_iv = 1'b0; zu_or = 1'b0;
        tu_clr = 1'b0; tu_iv = 1'b0; tu_or = 1'b0;
        pk_raddr = rdp;
        sig_raddr = rdp;
        // ② μ 支按 ai 摆 ctx/msg 读地址；tr 支按 ai 读 pk
        msg_raddr = msg_off[12:0];
        ctx_raddr = ctx_off[7:0];
        c_we = 1'b0; c_waddr = 8'd0; c_din = 32'd0; c_raddr = dbg_idx;
        sb_rd_addr = cnt;
        nt_we = 1'b0; nt_waddr = 8'd0; nt_wdata = 32'd0; nt_raddr = cnt;
        ac_we = 1'b0; ac_waddr = 8'd0; ac_din = 32'd0; ac_raddr = dbg_idx;
        un_rd_addr = cnt;
        p6_clr = 1'b0; p6_iv = 1'b0;
        wp_we = 1'b0; wp_waddr = 10'd0; wp_din = 8'd0;
        // c̃' 吸收时读 w1pk（ai≥64 之后）
        wp_raddr = (ai >= 14'd64) ? (ai[9:0] - 10'd64) : 10'd0;

        if (st == S_D_ABS && hsel == 2'd0) pk_raddr = ai[12:0];

        // ④ MAC：Â[cnt]·ẑ[vj][cnt] 累加到 acc[cnt]（j==0 直接放，省清零）
        // 写回地址与"旧累加值"都来自 tag，不再用当拍的 cnt / ac_dout ——
        // 结果比读地址晚 6 拍，用当拍的 cnt 会写到错误的系数上。
        if (st == S_MAC) begin
            un_rd_addr = cnt;
            z_raddr    = {vj[2:0], cnt};
            ac_raddr   = cnt;
            if (mm_ov) begin
                ac_we    = 1'b1; ac_waddr = mm_owaddr;
                ac_din   = (vj == 3'd0) ? mm_t : (mm_oacc + mm_t);
            end
        end
        // ④ acc −= mont(ĉ∘t̂₁[vi])
        if (st == S_CT1) begin
            c_raddr  = cnt;
            t1_raddr = {vi[2:0], cnt};
            ac_raddr = cnt;
            if (mm_ov) begin
                ac_we = 1'b1; ac_waddr = mm_owaddr; ac_din = mm_oacc - mm_t;
            end
        end
        // ④ 装载：reduce32(acc) → invNTT 写口
        if (st == S_RED) begin
            ac_raddr = cnt;
            if (ph) begin nt_we = 1'b1; nt_waddr = cnt; nt_wdata = red_out; end
        end
        // ④ UseHint + 6 位打包
        if (st == S_UH) begin
            nt_raddr = cnt;
            h_raddr  = {vi[2:0], cnt};
            if (uph == 2'd0 && cnt == 8'd0 && vi == 3'd0) p6_clr = 1'b1;  // 仅最开头清一次
            if (uph == 2'd2) p6_iv = 1'b1;   // 第 3 拍才喂：w₁ 上一拍才进寄存器
        end
        // 打包器吐字节 → w1pk
        if (p6_ov) begin wp_we = 1'b1; wp_waddr = wp_ptr; wp_din = p6_ob; end

        // ③a SampleInBall 结果 → c 存储
        if (st == S_SIB_MV) begin
            sb_rd_addr = cnt;
            if (ph) begin c_we = 1'b1; c_waddr = cnt; c_din = sb_rd_data; end
        end
        // ③b NTT 装载：选中对象 → NTT 写口（t₁ 装载时左移 D，即 t₁·2ᴰ）
        if (st == S_NT_LD) begin
            case (nstore)
                2'd0: c_raddr  = cnt;
                2'd1: z_raddr  = {poly[2:0], cnt};
                default: t1_raddr = {poly[2:0], cnt};
            endcase
            if (ph) begin
                nt_we = 1'b1; nt_waddr = cnt;
                nt_wdata = (nstore == 2'd0) ? c_dout
                         : (nstore == 2'd1) ? z_dout
                                            : (t1_dout <<< D);
            end
        end
        // ③b NTT 写回：NTT 读口 → 选中对象（就地）
        if (st == S_NT_WB) begin
            nt_raddr = cnt;
            if (ph) begin
                case (nstore)
                    2'd0: begin c_we = 1'b1; c_waddr = cnt; c_din = nt_rdata; end
                    2'd1: begin z_we = 1'b1; z_waddr = {poly[2:0], cnt}; z_din = nt_rdata; end
                    default: begin t1_we = 1'b1; t1_waddr = {poly[2:0], cnt}; t1_din = nt_rdata; end
                endcase
            end
        end

        // 清 h
        if (st == S_HCLR) begin h_we = 1'b1; h_waddr = hclr; h_din = 1'b0; end
        // 进解包段前清累加器
        if (st == S_CT) begin zu_clr = 1'b1; tu_clr = 1'b1; end

        // c̃ 读 sig[cnt]
        if (st == S_CT) sig_raddr = {5'd0, cnt};

        // z 解包：抽系数写 z[poly]，否则喂字节
        if (st == S_ZU) begin
            if (zu_ov) begin
                zu_or = 1'b1;
                z_we = 1'b1; z_waddr = {poly[2:0], cnt}; z_din = z_coef;
            end else if (feed) begin
                zu_iv = 1'b1;
            end
        end

        // hint 计数区：sig[SIG_H0+ω+cnt]
        if (st == S_HC) sig_raddr = SIG_H0 + {5'd0, OMEGA} + {5'd0, cnt};
        // hint 下标区：sig[SIG_H0+hidx]；命中就置 h[poly][sig_rdata]
        if (st == S_HI) begin
            sig_raddr = SIG_H0 + {5'd0, hidx};
            if (ph) begin
                // ⚠️ 多项式下标要 3 位：写成 poly[1:0] 时 k>4 的高位被截掉 ——
                // 65 的 poly=4/5 会绕回 0/1，把它们的 hint 位并进 poly0/poly1
                // （读侧是 11 位、读 poly4/5 全空），c̃' 自然对不上。
                h_we = 1'b1; h_waddr = {poly[2:0], sig_rdata}; h_din = 1'b1;
            end
        end
        // 填充区：sig[SIG_H0+hidx+cnt]
        if (st == S_HP) sig_raddr = SIG_H0 + {5'd0, hidx} + {5'd0, cnt};

        // t₁ 解包：抽系数写 t1[poly]，否则喂字节
        if (st == S_T1) begin
            if (tu_ov) begin
                tu_or = 1'b1;
                t1_we = 1'b1; t1_waddr = {poly[2:0], cnt}; t1_din = t1_coef;
            end else if (feed) begin
                tu_iv = 1'b1;
            end
        end
    end
endmodule

`default_nettype wire
