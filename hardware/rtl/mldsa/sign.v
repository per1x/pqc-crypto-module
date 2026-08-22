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

// ============================================================================
// 【参数化：44 / 65 / 87】
// ============================================================================
//   ML-DSA-44  k=4 ℓ=4 η=2 τ=39 γ₁=2¹⁷ γ₂=(q−1)/88 ω=80 β=78  c̃=32  σ=2420
//   ML-DSA-65  k=6 ℓ=5 η=4 τ=49 γ₁=2¹⁹ γ₂=(q−1)/32 ω=55 β=196 c̃=48  σ=3309
//   ML-DSA-87  k=8 ℓ=7 η=2 τ=60 γ₁=2¹⁹ γ₂=(q−1)/32 ω=75 β=120 c̃=64  σ=4627
// 三处结构性分叉：γ₂ 选 decompose/make_hint 的 MODE；γ₁ 决定 z 的位宽（18/20）；
// γ₂ 决定 w1Encode 的位宽（6/4）。存储与位宽按最大的 87 开。
// ⚠️ 参数集是**运行时选**的：K/ℓ/η/τ/γ₁/γ₂/ω/β/c̃ 全部由 start 那一拍锁存的
//    pset 译码（mldsa_params）。存储一律按最大的 87 开，三个参数集共用同一份。
module mldsa_sign (
    input  wire        clk,
    input  wire        rst_n,

    // ---- 擦除广播（engine 里那一台擦除机，见 mldsa_engine.v 文件头）----
    // wipe 期间本核在复位里，下面每一块 ram_dp 的 B 口被强制成
    // addr=wipe_addr、we=1、din=0 —— BRAM 不因复位清零，必须真写一遍。
    input  wire        wipe,
    input  wire [12:0] wipe_addr,

    input  wire [1:0]  pset,         // 0=44 1=65 2=87，start 那一拍锁存
    input  wire        start,        // 脉冲

    // ---- 输入缓冲：start 之前由测试台按字节预载 ----
    input  wire        sk_wr_en,
    input  wire [12:0] sk_wr_addr,   // sk：2560 / 4032 / 4896 字节
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

    // ---- 派生哈希 ----
    output reg [511:0] mu,
    output reg [511:0] rhopp,
    // c̃ 端口固定 **512 位**（c̃ 最长 64 字节），只有低 ctb 字节有效
    output reg [511:0] ctilde,       // c̃ = H(μ‖w1pack)，⑥ 之后有效（当前 κ 轮）

    // ---- 调试读口：done 之后读系数。dbg_sel[5:2]=组，[1:0]=第几条多项式 ----
    input  wire [6:0]  dbg_sel,   // [6:3]=组，[2:0]=第几条多项式
    input  wire [7:0]  dbg_idx,
    output wire signed [31:0] dbg_coef,

    // ---- sig 输出缓冲（后续段填）----
    input  wire [12:0] sig_addr,
    output wire [7:0]  sig_data
);
    // ================= ML-DSA-44 常量 =================
    localparam integer D = 13;
    localparam integer POLYT0_B = 416;

    // ---- 运行时参数集配置 ----
    // 空闲时跟着输入走、一离开 S_IDLE 就冻结在锁存值上（理由见 keygen 同一段注释）
    reg  [1:0] pset_r;
    reg        busy_r;
    wire [1:0] pset_eff = busy_r ? pset_r : pset;
    wire [3:0]  cfg_k, cfg_l, cfg_km1, cfg_lm1, cfg_lkm1;
    wire [2:0]  cfg_eta;
    wire [6:0]  cfg_tau, cfg_ctb;
    wire [7:0]  cfg_omega, cfg_peb, cfg_w1b;
    wire        cfg_g2mode;
    wire [4:0]  cfg_ebits, cfg_zbits, cfg_w1bits;
    wire [9:0]  cfg_zb;
    wire signed [31:0] cfg_gamma1, cfg_gamma2, cfg_eta_s;
    wire [31:0] cfg_zbound, cfg_r0bound, cfg_ct0bound;
    wire [12:0] cfg_sk_t0, cfg_sig_h0;
    mldsa_params u_par (
        .pset(pset_eff),
        .k(cfg_k), .l(cfg_l), .eta(cfg_eta), .tau(cfg_tau), .omega(cfg_omega),
        .ctb(cfg_ctb), .g2mode(cfg_g2mode),
        .km1(cfg_km1), .lm1(cfg_lm1), .lkm1(cfg_lkm1),
        .ebits(cfg_ebits), .peb(cfg_peb), .zbits(cfg_zbits), .zb(cfg_zb),
        .w1bits(cfg_w1bits), .w1b(cfg_w1b),
        .gamma1(cfg_gamma1), .gamma2(cfg_gamma2), .eta_s(cfg_eta_s),
        .zbound(cfg_zbound), .r0bound(cfg_r0bound), .ct0bound(cfg_ct0bound),
        .sk_s2(), .sk_t0(cfg_sk_t0), .sklen(), .pklen(),
        .siglen(), .sig_h0(cfg_sig_h0));
    // 三个 norm 界：别记混（z 用 γ₁−β，r₀ 用 γ₂−β，ct₀ 用 γ₂）
    wire [31:0] ZBOUND = cfg_zbound, R0BOUND = cfg_r0bound, CT0BOUND = cfg_ct0bound;
    // 循环终点（poly 要装下 ℓ+k−1，87 是 14 → 4 位）
    wire [3:0] LM1 = cfg_lm1, KM1 = cfg_km1, LKM1 = cfg_lkm1;
    // NTT 段的终点随对象走：nstore 0=s₁(ℓ) 1=s₂(k) 2=t₀(k) 3=y(ℓ)
    // s₁/s₂ 解包段的分组：poly < ℓ 是 s₁，否则 s₂（下标减 ℓ）
    // sk 段偏移
    localparam [12:0] SK_S1 = 13'd128;   // s₁pack 恒从 128 起，与参数集无关

    localparam [5:0]
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
        // ⑥ c̃ = H(μ‖w1pack)（走 S_D_* 引擎）+ SampleInBall 出 c
        S_W1_PK  = 6'd23,    // w₁ → w1pk 缓冲
        S_SIB_GO = 6'd24,    // 起 SampleInBall
        S_SIB_WT = 6'd25,    // 等 sb_done
        S_SIB_MV = 6'd26,    // sb 的 c → c 存储
        // ⑦ ĉ=NTT(c)；z[j]=y[j]+invNTT(ĉ∘ŝ₁[j])；‖z‖∞ 检查
        S_CN_LD  = 6'd27,    // c → NTT 写口（正变换 ĉ）
        S_CN_GO  = 6'd28,
        S_CN_ST  = 6'd29,
        S_CN_WB  = 6'd30,    // NTT 读口 → c（就地 ĉ）
        S_Z_MUL  = 6'd31,    // ĉ∘ŝ₁[vj] → invNTT 写口
        S_Z_GO   = 6'd32,
        S_Z_ST   = 6'd33,
        S_Z_WB   = 6'd34,    // z[vj]=reduce32(y[vj]+invNTT)；norm 检查
        // ⑧ r₀[i]=w0[i]−invNTT(ĉ∘ŝ₂[i])；‖r₀‖∞ 检查
        S_R_MUL  = 6'd35,
        S_R_GO   = 6'd36,
        S_R_ST   = 6'd37,
        S_R_WB   = 6'd38,    // r0[vi]=reduce32(w0[vi]−invNTT)；norm 检查
        // ⑨ ct₀=invNTT(ĉ∘t̂₀[i])；‖ct₀‖∞ 检查；hint=MakeHint(r0+ct0, w1)；权重
        S_H_MUL  = 6'd39,
        S_H_GO   = 6'd40,
        S_H_ST   = 6'd41,
        S_H_WB   = 6'd42,    // ct0 检查 + hint + 权重
        // ⑩ 拒绝判定 + sigEncode
        S_REJ    = 6'd43,    // 这一轮是否作废：是→回 ④ 重来，否→ sigEncode
        S_SIG_CT = 6'd44,    // sig[0..31] = c̃
        S_SIG_Z  = 6'd45,    // sig z 段：polyz_pack（4 条 z 连续打包）
        S_SIG_ZD = 6'd47,    // 等 z 打包器把最后 1~2 字节吐完
        S_HP_CLR = 6'd49,    // 先把 hint 下标/填充区（ω 字节）清零（BRAM 无复位、跨签名有残留）
        S_HP     = 6'd46,    // HintBitPack：扫描 hint[vi]，1 的下标写进 sig
        S_HP_CNT = 6'd48,    // 写累计计数 sig[SIG_H0+ω+vi] = hidx
        S_FIN    = 6'd63;

    reg [5:0] st;

    // ================= 输入缓冲 =================
    reg  [12:0] sk_raddr;
    wire [7:0]  sk_rdata;
    ram_dp #(.DW(8), .AW(13)) u_sk (
        .clk(clk), .a_we(sk_wr_en), .a_addr(sk_wr_addr), .a_din(sk_wr_data),
        .a_dout(), .b_we(wipe), .b_addr(wipe ? wipe_addr[12:0] : (sk_raddr)), .b_din(wipe ? 8'd0 : (8'd0)), .b_dout(sk_rdata));

    // msg / ctx 缓冲：本段不读，但先把写口接上（测试台可预载；②段起才读）
    reg  [12:0] msg_raddr;
    wire [7:0]  msg_rdata;
    ram_dp #(.DW(8), .AW(13)) u_msg (
        .clk(clk), .a_we(msg_wr_en), .a_addr(msg_wr_addr), .a_din(msg_wr_data),
        .a_dout(), .b_we(wipe), .b_addr(wipe ? wipe_addr[12:0] : (msg_raddr)), .b_din(wipe ? 8'd0 : (8'd0)), .b_dout(msg_rdata));
    reg  [7:0]  ctx_raddr;
    wire [7:0]  ctx_rdata;
    ram_dp #(.DW(8), .AW(8)) u_ctx (
        .clk(clk), .a_we(ctx_wr_en), .a_addr(ctx_wr_addr), .a_din(ctx_wr_data),
        .a_dout(), .b_we(wipe), .b_addr(wipe ? wipe_addr[7:0] : (ctx_raddr)), .b_din(wipe ? 8'd0 : (8'd0)), .b_dout(ctx_rdata));

    // ================= 系数存储 =================
    // s₁/s₂/t₀ 各 4 条 × 256 × 32b。addr = {poly[1:0], idx[7:0]}。
    reg         s1_we; reg [10:0] s1_waddr; reg signed [31:0] s1_din; reg [10:0] s1_raddr;
    wire signed [31:0] s1_dout;
    ram_dp #(.DW(32), .AW(11)) u_s1 (
        .clk(clk), .a_we(s1_we), .a_addr(s1_waddr), .a_din(s1_din), .a_dout(),
        .b_we(wipe), .b_addr(wipe ? wipe_addr[10:0] : (s1_raddr)), .b_din(wipe ? 32'd0 : (32'd0)), .b_dout(s1_dout));
    reg         s2_we; reg [10:0] s2_waddr; reg signed [31:0] s2_din; reg [10:0] s2_raddr;
    wire signed [31:0] s2_dout;
    ram_dp #(.DW(32), .AW(11)) u_s2 (
        .clk(clk), .a_we(s2_we), .a_addr(s2_waddr), .a_din(s2_din), .a_dout(),
        .b_we(wipe), .b_addr(wipe ? wipe_addr[10:0] : (s2_raddr)), .b_din(wipe ? 32'd0 : (32'd0)), .b_dout(s2_dout));
    reg         t0_we; reg [10:0] t0_waddr; reg signed [31:0] t0_din; reg [10:0] t0_raddr;
    wire signed [31:0] t0_dout;
    ram_dp #(.DW(32), .AW(11)) u_t0 (
        .clk(clk), .a_we(t0_we), .a_addr(t0_waddr), .a_din(t0_din), .a_dout(),
        .b_we(wipe), .b_addr(wipe ? wipe_addr[10:0] : (t0_raddr)), .b_din(wipe ? 32'd0 : (32'd0)), .b_dout(t0_dout));

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
    localparam [1:0] OWN_FSM = 2'd0, OWN_EM = 2'd1, OWN_UNI = 2'd2, OWN_SIB = 2'd3;
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
    mldsa_expand_mask u_em (
        .clk(clk), .rst_n(rst_n), .cbits(cfg_zbits),
        .start(em_start), .seed(rhopp), .nonce(em_nonce), .done(em_done),
        .sha_start(em_ss), .sha_rate(em_sr), .sha_suffix(em_su),
        .sha_in_valid(em_siv), .sha_in_data(em_sid), .sha_in_flush(em_sif),
        .sha_in_ready(sha_in_ready && (owner == OWN_EM)),
        .sha_out_valid(sha_out_valid && (owner == OWN_EM)),
        .sha_out_ready(em_sor), .sha_out_data(sha_out_data),
        .rd_addr(em_rd_addr), .rd_data(em_rd_data));

    // ---- y 存储：ℓ 条 × 256 × 32b。y 保留原值（⑦ z=y+cs₁ 要用），ŷ=NTT(y) 另存 ----
    reg         y_we; reg [10:0] y_waddr; reg signed [31:0] y_din; reg [10:0] y_raddr;
    wire signed [31:0] y_dout;
    ram_dp #(.DW(32), .AW(11)) u_y (
        .clk(clk), .a_we(y_we), .a_addr(y_waddr), .a_din(y_din), .a_dout(),
        .b_we(wipe), .b_addr(wipe ? wipe_addr[10:0] : (y_raddr)), .b_din(wipe ? 32'd0 : (32'd0)), .b_dout(y_dout));
    reg         yh_we; reg [10:0] yh_waddr; reg signed [31:0] yh_din; reg [10:0] yh_raddr;
    wire signed [31:0] yh_dout;
    ram_dp #(.DW(32), .AW(11)) u_yh (
        .clk(clk), .a_we(yh_we), .a_addr(yh_waddr), .a_din(yh_din), .a_dout(),
        .b_we(wipe), .b_addr(wipe ? wipe_addr[10:0] : (yh_raddr)), .b_din(wipe ? 32'd0 : (32'd0)), .b_dout(yh_dout));

    // ---- ⑤ MAC 累加缓冲 acc（k×256×32），w0（k×256×32），w1（k×256×6）----
    reg         ac_we; reg [10:0] ac_waddr; reg signed [31:0] ac_din; reg [10:0] ac_raddr;
    wire signed [31:0] ac_dout;
    ram_dp #(.DW(32), .AW(11)) u_acc (
        .clk(clk), .a_we(ac_we), .a_addr(ac_waddr), .a_din(ac_din), .a_dout(),
        .b_we(wipe), .b_addr(wipe ? wipe_addr[10:0] : (ac_raddr)), .b_din(wipe ? 32'd0 : (32'd0)), .b_dout(ac_dout));
    reg         w0_we; reg [10:0] w0_waddr; reg signed [31:0] w0_din; reg [10:0] w0_raddr;
    wire signed [31:0] w0_dout;
    ram_dp #(.DW(32), .AW(11)) u_w0 (
        .clk(clk), .a_we(w0_we), .a_addr(w0_waddr), .a_din(w0_din), .a_dout(),
        .b_we(wipe), .b_addr(wipe ? wipe_addr[10:0] : (w0_raddr)), .b_din(wipe ? 32'd0 : (32'd0)), .b_dout(w0_dout));
    reg         w1_we; reg [10:0] w1_waddr; reg [5:0] w1_din; reg [10:0] w1_raddr;
    wire [5:0]  w1_dout;
    ram_dp #(.DW(6), .AW(11)) u_w1 (
        .clk(clk), .a_we(w1_we), .a_addr(w1_waddr), .a_din(w1_din), .a_dout(),
        .b_we(wipe), .b_addr(wipe ? wipe_addr[10:0] : (w1_raddr)), .b_din(wipe ? 6'd0 : (6'd0)), .b_dout(w1_dout));

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

    // reduce32：MAC 累加值灌进 invNTT 前先规约
    wire signed [31:0] red_out;
    mldsa_reduce32 u_red (.a(ac_dout), .r(red_out));
    // caddq → decompose（MODE=0，γ₂=(q−1)/88）
    wire signed [31:0] cad_out;
    mldsa_caddq u_cad (.a(nt_rdata), .r(cad_out));
    wire signed [31:0] dec_a0;
    wire        [5:0]  dec_a1;
    mldsa_decompose u_dec (.mode(cfg_g2mode), .a(cad_out), .a0(dec_a0), .a1(dec_a1));

    // ---- ⑥ w₁ 打包器（6 位/系数，GAMMA2_88）→ w1pk 缓冲，供 c̃ 吸收 ----
    reg         p6_clr, p6_iv;
    wire        p6_ir, p6_ov;
    wire [7:0]  p6_ob;
    mldsa_bitpack u_p6 (
        .clk(clk), .rst_n(rst_n), .clr(p6_clr), .w(cfg_w1bits),
        .in_val({14'd0, w1_dout}), .in_valid(p6_iv), .in_ready(p6_ir),
        .out_byte(p6_ob), .out_valid(p6_ov));
    // w1pk 缓冲：k×192 = 768 字节
    reg         wp_we; reg [9:0] wp_waddr; reg [7:0] wp_din; reg [9:0] wp_raddr;
    wire [7:0]  wp_dout;
    reg  [9:0]  wp_ptr;         // 打包落盘指针
    ram_dp #(.DW(8), .AW(10)) u_wp (
        .clk(clk), .a_we(wp_we), .a_addr(wp_waddr), .a_din(wp_din), .a_dout(),
        .b_we(wipe), .b_addr(wipe ? wipe_addr[9:0] : (wp_raddr)), .b_din(wipe ? 8'd0 : (8'd0)), .b_dout(wp_dout));

    // ---- ⑥ SampleInBall：c = SampleInBall(c̃)，τ=39 个 ±1 ----
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

    // ---- c 存储：256 × 32b（⑥ 存，⑦ NTT 就地覆盖成 ĉ）----
    reg         c_we; reg [7:0] c_waddr; reg signed [31:0] c_din; reg [7:0] c_raddr;
    wire signed [31:0] c_dout;
    ram_dp #(.DW(32), .AW(8)) u_c (
        .clk(clk), .a_we(c_we), .a_addr(c_waddr), .a_din(c_din), .a_dout(),
        .b_we(wipe), .b_addr(wipe ? wipe_addr[7:0] : (c_raddr)), .b_din(wipe ? 32'd0 : (32'd0)), .b_dout(c_dout));

    // ---- z 存储：ℓ 条 × 256 × 32b（⑦ 存，⑩ sigEncode 读）----
    reg         z_we; reg [10:0] z_waddr; reg signed [31:0] z_din; reg [10:0] z_raddr;
    wire signed [31:0] z_dout;
    ram_dp #(.DW(32), .AW(11)) u_z (
        .clk(clk), .a_we(z_we), .a_addr(z_waddr), .a_din(z_din), .a_dout(),
        .b_we(wipe), .b_addr(wipe ? wipe_addr[10:0] : (z_raddr)), .b_din(wipe ? 32'd0 : (32'd0)), .b_dout(z_dout));

    // ---- hint 存储：k 条 × 256 × 1b（⑨ 存，⑩ HintBitPack 读）----
    reg         hn_we; reg [10:0] hn_waddr; reg hn_din; reg [10:0] hn_raddr;
    wire        hn_dout;
    ram_dp #(.DW(1), .AW(11)) u_hn (
        .clk(clk), .a_we(hn_we), .a_addr(hn_waddr), .a_din(hn_din), .a_dout(),
        .b_we(wipe), .b_addr(wipe ? wipe_addr[10:0] : (hn_raddr)), .b_din(wipe ? 1'd0 : (1'd0)), .b_dout(hn_dout));

    // 合成（invNTT 结果在 nt_rdata）：z=y+cs₁，r0=w0−cs₂，ct0=cs（都过 reduce32）
    wire signed [31:0] comb_in = (st == S_R_WB) ? (w0_dout - nt_rdata)
                               : (st == S_H_WB) ? nt_rdata
                                                : (y_dout + nt_rdata);   // S_Z_WB
    wire signed [31:0] comb_red;
    mldsa_reduce32 u_red2 (.a(comb_in), .r(comb_red));

    // norm 检查：|居中系数| ≥ bound。三个 bound 别记混。
    wire [31:0] cabs = comb_red[31] ? (-comb_red) : comb_red;
    wire [31:0] norm_bound = (st == S_Z_WB) ? ZBOUND        // γ₁−β
                           : (st == S_R_WB) ? R0BOUND       // γ₂−β
                                            : CT0BOUND;      // γ₂（ct₀）
    wire norm_bad = (cabs >= norm_bound);

    // ⑨ 的写回链原来是**一整条组合路径**：
    //   invNTT 结果(BRAM) → reduce32 → +r0 → reduce32 → MakeHint(含 decompose)
    //   → hint 位 → hint 存储 / weight 累加
    // ML-DSA-87 实测这就是 Sign 的关键路径（逻辑层级 35，其中 CARRY8=15）：
    // 单核 post-route WNS −1.611ns，三个核一起进 engine 之后掉到 −2.307ns。
    // 从中间切一刀：ct₀ 与它的越界判定先进寄存器，下一拍再做 +r0 / reduce32 / MakeHint。
    // 代价是 ⑨ 每个系数从 2 拍变 3 拍（只影响这一段，不影响拒绝循环的轮数）。
    reg signed [31:0] ct0_r;    // 第 1 拍锁存的 ct₀ = reduce32(invNTT)
    reg               nb_r;     // 同拍锁存的 ‖ct₀‖∞ 越界判定

    // MakeHint(a0, w1) 用 ⑨：a0 = reduce32(r0 + ct0)。r0 存在 w0 存储（⑧ 就地覆盖）。
    wire signed [31:0] a0_in = w0_dout + ct0_r;      // 用**上一拍锁存**的 ct₀
    wire signed [31:0] a0_red;
    mldsa_reduce32 u_reda0 (.a(a0_in), .r(a0_red));
    wire hint_bit;
    mldsa_make_hint u_mh (.mode(cfg_g2mode), .a0(a0_red), .a1(w1_dout), .hint(hint_bit));

    // ⑦⑧⑨⑩ 拒绝循环状态
    reg        reject;     // 本轮任一 norm / 权重越界 → 作废重来
    reg [10:0] weight;     // hint 总权重（最多 256×4=1024，要 11 位；9 位会在 >511 时回绕）

    // ---- ⑩ sig 输出缓冲：2420 字节；HintBitPack 的运行下标 hidx；打包落盘指针 ----
    reg         sig_we; reg [12:0] sig_waddr; reg [7:0] sig_din;
    wire [7:0]  sig_data_w;
    ram_dp #(.DW(8), .AW(13)) u_sig (
        .clk(clk), .a_we(sig_we), .a_addr(sig_waddr), .a_din(sig_din), .a_dout(),
        .b_we(wipe), .b_addr(wipe ? wipe_addr[12:0] : (sig_addr)), .b_din(wipe ? 8'd0 : (8'd0)), .b_dout(sig_data_w));
    assign sig_data = sig_data_w;

    reg [12:0] sigptr;     // sig 落盘指针
    reg [7:0]  hidx;       // HintBitPack 运行下标（0..ω）

    // ---- z 打包器（18 位/系数，存 γ₁−z）----
    reg         pz_clr, pz_iv;
    wire        pz_ir, pz_ov;
    wire [7:0]  pz_ob;
    wire [31:0] pz_full = cfg_gamma1 - z_dout;             // γ₁ − z，落在 [0, 2γ₁)
    wire [19:0] pz_in = pz_full[19:0];
    mldsa_bitpack u_pz (
        .clk(clk), .rst_n(rst_n), .clr(pz_clr), .w(cfg_zbits),
        .in_val(pz_in), .in_valid(pz_iv), .in_ready(pz_ir),
        .out_byte(pz_ob), .out_valid(pz_ov));

    wire [12:0] SIG_Z0 = {6'd0, cfg_ctb};    // z 段起点 = c̃ 长度
    wire [12:0] SIG_H0 = cfg_sig_h0;         // hint 段起点
    wire [12:0] OMEGA  = {5'd0, cfg_omega};

    // 调试读口挂 b 口（done 后用，与写不重叠）。dbg_sel[4:2] 选组，[1:0] 选第几条。
    //  组（dbg_sel[5:2]）：0 s₁  1 ŷ  2 s₂  3 t₀  4 y  5 w0/r0  6 w1  7 c/ĉ
    //                     8 z  9 hint
    assign dbg_coef =
          (dbg_sel[6:3] == 4'd0)  ? s1_dout
        : (dbg_sel[6:3] == 4'd1)  ? yh_dout
        : (dbg_sel[6:3] == 4'd2)  ? s2_dout
        : (dbg_sel[6:3] == 4'd3)  ? t0_dout
        : (dbg_sel[6:3] == 4'd4)  ? y_dout
        : (dbg_sel[6:3] == 4'd5)  ? w0_dout
        : (dbg_sel[6:3] == 4'd6)  ? {{26{1'b0}}, w1_dout}
        : (dbg_sel[6:3] == 4'd7)  ? c_dout
        : (dbg_sel[6:3] == 4'd8)  ? z_dout
        : (dbg_sel[6:3] == 4'd9)  ? {31'd0, hn_dout}
        : (dbg_sel[6:3] == 4'd10) ? {31'd0, reject}       // 本轮拒绝标志
        : (dbg_sel[6:3] == 4'd11) ? {21'd0, weight}       // hint 总权重
        : 32'd0;


    // ================= 位解包器（skDecode）=================
    // η（3 位）用于 s₁/s₂，t₀（13 位）单独一个。喂字节 / 抽系数按 mode 二选一。
    reg         eu_clr, eu_iv, eu_or;
    wire        eu_ir, eu_ov;
    wire [19:0] eu_val;
    mldsa_bitunpack u_eu (
        .clk(clk), .rst_n(rst_n), .clr(eu_clr), .w(cfg_ebits),
        .in_byte(sk_rdata), .in_valid(eu_iv), .in_ready(eu_ir),
        .out_val(eu_val), .out_valid(eu_ov), .out_ready(eu_or));

    reg         tu_clr, tu_iv, tu_or;
    wire        tu_ir, tu_ov;
    wire [19:0] tu_val;
    mldsa_bitunpack u_tu (
        .clk(clk), .rst_n(rst_n), .clr(tu_clr), .w(5'd13),
        .in_byte(sk_rdata), .in_valid(tu_iv), .in_ready(tu_ir),
        .out_val(tu_val), .out_valid(tu_ov), .out_ready(tu_or));

    // 逆变换（有符号搬移）：η−v / 2^(D−1)−v
    wire signed [31:0] eta_coef = cfg_eta_s - $signed({12'd0, eu_val});
    wire signed [31:0] t0_coef  = $signed(32'sd1 << (D-1)) - $signed({12'd0, tu_val});

    // ================= 控制寄存器 =================
    reg [7:0]  cnt;        // 头部 / 系数计数（0..127 或 0..255）
    reg        ph;         // 同步读两拍相位
    reg [1:0]  hph;        // ⑨ 写回专用的三拍相位（那一段被切成了三拍）
    reg [3:0]  poly;       // 第几条：s₁/s₂ 段用 0..ℓ+k−1，其余段用 0..ℓ−1 或 0..k−1
    reg        t0phase;    // 0 = 正在解 s₁/s₂（3 位），1 = 正在解 t₀（13 位）
    reg [12:0] skp;        // sk 读指针（进解包器的字节）
    reg        feed;       // S_UNP 内两拍：0=抽/摆地址，1=喂字节

    // ② / ⑥ 派生哈希用（共用一套吸收/挤压引擎）
    reg [13:0] ai;         // 吸收字节指针（μ 支最长 66+ctx+msg，≤ ~8500，14 位）
    reg [1:0]  hsel;       // 0 = μ，1 = ρ''，2 = c̃

    // ③ NTT prep 用
    reg [1:0]  nstore;     // 0=s₁, 1=s₂, 2=t₀
    reg        nt_lowseen; // 「done 是电平」：start 后先见它落一次再等它起

    wire [3:0] nt_last  = (nstore == 2'd0 || nstore == 2'd3) ? LM1 : KM1;
    wire       s2_phase = (poly >= cfg_l);
    wire [2:0] sidx     = s2_phase ? (poly[2:0] - cfg_l[2:0]) : poly[2:0];

    // ④ 拒绝循环用
    reg [15:0] kappa;      // ExpandMask 的 nonce 基（每轮 +ℓ）
    // ⑤ MAC / invNTT / decompose 用
    reg [3:0]  vi;         // 第几个 i（0..k−1）
    reg [3:0]  vj;         // 第几个 j（0..ℓ−1）

    // NTT 装载时选中的 store 数据（nstore：0=s₁,1=s₂,2=t₀,3=y）
    wire signed [31:0] store_dout =
        (nstore == 2'd0) ? s1_dout : (nstore == 2'd1) ? s2_dout
      : (nstore == 2'd2) ? t0_dout : y_dout;

    // ======================================================================
    // ⑤⑦⑧⑨ 共用的**流水**乘法链：mont(x·y)
    // ======================================================================
    //
    // 原来这里是两条**全组合**的 mldsa_mont_reduce（⑤ 的 Â∘ŷ 与 ⑦⑧⑨ 的 ĉ∘ŝ）。
    // 它们是把蝶形流水化之后剩下的真正关键路径 —— ML-DSA-87 Sign 的 post-route
    // 实测：蝶形流水化前 WNS = −3.332ns（关键路径在蝶形），流水化后 WNS = −3.356ns，
    // 关键路径原地搬到 u_t0 → pw_prod → u_ntt 这条上。同一个形状：
    // BRAM 读出 → 三次 32×32 乘（x·y、m=p·QINV、m·Q）→ BRAM 写入，逻辑层级 31。
    //
    // 两条链合并成**一条** mldsa_mont_mul_pipe：⑤ 与 ⑦⑧⑨ 在时间上互斥
    // （S_MAC 与 S_*_MUL 是不同状态），分时复用即可，顺带省掉一整套乘法器。
    //
    // 时序结构（与 ntt_core 同一套写法）：
    //   第 T 拍   FSM 用 cnt 发读地址
    //   第 T+1 拍 BRAM 数据出来 → 拉 in_valid，把**写回地址**与**累加旁路**塞进 tag
    //   第 T+6 拍 结果回来，写回地址从 tag 里取
    // 每拍发一个（原来两拍一个），所以这几段反而比改造前**快一倍**。
    // 段与段之间必须排空（mm_empty）：下一段要读的正是这一段刚写完的累加器。
    //
    //   MUL 状态选第二乘子：S_R_MUL→ŝ₂，S_H_MUL→t̂₀，其余（S_Z_MUL）→ŝ₁
    wire signed [31:0] pw_b = (st == S_R_MUL) ? s2_dout
                            : (st == S_H_MUL) ? t0_dout : s1_dout;

    wire mm_is_mac = (st == S_MAC);
    // ⑤ 的第一乘子是 Â（23 位无符号），补零成 32 位有符号；⑦⑧⑨ 是 ĉ
    wire signed [31:0] mm_x = mm_is_mac ? $signed({9'd0, un_rd_data}) : c_dout;
    wire signed [31:0] mm_y = mm_is_mac ? yh_dout : pw_b;

    // 本段 256 个系数是否已全部发出（发完还要等流水排空才能换段）
    reg        mm_last;
    reg        mm_iv;
    reg  [7:0] mm_addr_d;      // 上一拍发出的读地址，随数据一起进 tag
    wire       mm_issue = !mm_last &&
                          (st == S_MAC   || st == S_Z_MUL ||
                           st == S_R_MUL || st == S_H_MUL);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            mm_iv <= 1'b0; mm_addr_d <= 8'd0;
        end else begin
            mm_iv     <= mm_issue;
            mm_addr_d <= cnt;
        end
    end

    // tag = {写回地址 8 位, 累加旁路 32 位}。
    // ⚠️ ac_dout 取的是**当拍**的值（第 T+1 拍才是 cnt 那个地址的内容），
    // 而地址用的是**上一拍**锁存的 mm_addr_d —— 两者都对应第 T 拍发出的那个系数。
    // 把它们交给 tag 之后，"旁路比结果早/晚一拍"这类错位在结构上不可能发生。
    wire        mm_ov;
    wire [39:0] mm_otag;
    wire signed [31:0] mm_t;
    wire        mm_busy;
    mldsa_mont_mul_pipe #(.TAGW(40)) u_mm (
        .clk(clk), .rst_n(rst_n),
        .in_valid(mm_iv), .in_tag({mm_addr_d, ac_dout}),
        .x(mm_x), .y(mm_y),
        .out_valid(mm_ov), .out_tag(mm_otag), .t_out(mm_t),
        .pipe_busy(mm_busy));

    wire [7:0]  mm_owaddr = mm_otag[39:32];
    wire signed [31:0] mm_oacc = $signed(mm_otag[31:0]);
    wire        mm_empty  = ~mm_iv & ~mm_busy;

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

    // ---- c̃ 支的吸收源：μ(64) ‖ w1pk(k×192=768) = 832 字节 ----
    wire [7:0]  ct_byte = (ai < 14'd64) ? mu[ai[5:0]*8 +: 8] : wp_dout;

    // 当前吸收的字节 / 总长 / 结束判据（hsel：0=μ,1=ρ'',2=c̃）
    wire [7:0]  abs_byte  = (hsel == 2'd0) ? mu_byte
                          : (hsel == 2'd1) ? rp_byte : ct_byte;
    wire [13:0] abs_total = (hsel == 2'd0) ? thr_total_mu
                          : (hsel == 2'd1) ? 14'd128 : (14'd64 + {10'd0, cfg_k} * {6'd0, cfg_w1b});
    wire        abs_last  = (ai == abs_total - 14'd1);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            st <= S_IDLE; done <= 1'b0; cnt <= 8'd0; ph <= 1'b0;
            mm_last <= 1'b0; hph <= 2'd0; ct0_r <= 32'sd0; nb_r <= 1'b0;
            pset_r <= 2'd0; busy_r <= 1'b0;
            poly <= 4'd0; t0phase <= 1'b0; skp <= 12'd0; feed <= 1'b0;
            ai <= 14'd0; hsel <= 2'd0;
            nstore <= 2'd0; nt_lowseen <= 1'b0;
            nt_start <= 1'b0; nt_inv <= 1'b0;
            owner <= OWN_FSM; em_start <= 1'b0; em_nonce <= 16'd0;
            kappa <= 16'd0; vi <= 4'd0; vj <= 4'd0;
            un_start <= 1'b0; un_nonce <= 16'd0;
            wp_ptr <= 10'd0; sb_start <= 1'b0; ctilde <= 512'd0;
            reject <= 1'b0; weight <= 11'd0;
            sigptr <= 12'd0; hidx <= 8'd0;
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
            sb_start <= 1'b0;
            // w₁ 打包器每吐一字节，落盘指针前进（同 KeyGen 的 pe_ptr）
            if (p6_ov) wp_ptr <= wp_ptr + 10'd1;
            // z 打包器每吐一字节，sig 落盘指针前进（只在 z 段有效）
            if (pz_ov) sigptr <= sigptr + 13'd1;

            case (st)
            S_IDLE: if (start) begin
                pset_r <= pset; busy_r <= 1'b1;
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
                poly <= 4'd0; t0phase <= 1'b0; cnt <= 8'd0; feed <= 1'b0;
                skp <= SK_S1;
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
                            if (poly == LKM1) begin
                                poly <= 4'd0; t0phase <= 1'b1; skp <= cfg_sk_t0;
                            end else begin
                                poly <= poly + 4'd1;
                            end
                        end else begin
                            cnt <= cnt + 8'd1;
                        end
                    end else begin
                        // 喂字节：feed=0 摆地址，feed=1 数据到位、in_valid
                        if (!feed) begin
                            feed <= 1'b1;
                        end else begin
                            if (eu_ir) skp <= skp + 13'd1;
                            feed <= 1'b0;
                        end
                    end
                end else begin
                    // ---- t₀ 段，13 位 ----
                    if (tu_ov) begin
                        feed <= 1'b0;
                        if (cnt == 8'd255) begin
                            cnt <= 8'd0;
                            if (poly == KM1) begin
                                hsel <= 2'd0;      // 先算 μ
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
                            if (tu_ir) skp <= skp + 13'd1;
                            feed <= 1'b0;
                        end
                    end
                end
            end

            // ---------- ②/⑥ 派生哈希：μ=H(tr‖M')、ρ''=H(K‖rnd‖μ)、c̃=H(μ‖w1pack) ----------
            // hsel=0 算 μ、1 算 ρ''、2 算 c̃，共用这一套吸收/挤压状态。
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
            // ⚠️ 吸收长度恰好是 rate（136）的整数倍时，最后一个字节把一个块填满、
            // 触发一次置换 —— 这拍海绵在置换、in_ready 落下。若照旧只等一拍就拉 flush，
            // flush 会落在「海绵不在 S_ABSORB」的拍次上被整个忽略（in_flush 只在
            // in_valid 低且核在 S_ABSORB 时才被采样），于是永远吸不完、卡在挤压。
            // 所以等 in_ready 重新拉高（核置换完、回到 S_ABSORB）再冲刷。
            // 非整数倍时最后一字节不填满块，in_ready 一直是高，这里立刻通过，行为不变。
            S_D_GAP: if (sha_in_ready) st <= S_D_FLU;
            S_D_FLU: begin fsm_sif <= 1'b1; cnt <= 8'd0; st <= S_D_SQ; end

            // 挤字节：μ/ρ'' 挤 64，c̃ 挤 32；低地址先出、从高位塞右移。
            S_D_SQ: begin
                fsm_sor <= 1'b1;
                if (sha_out_valid) begin
                    if (hsel == 2'd0)      mu     <= {sha_out_data, mu[511:8]};
                    else if (hsel == 2'd1) rhopp  <= {sha_out_data, rhopp[511:8]};
                    // c̃ 现在是定宽 512 位而 ctb 是运行时的，不能再靠"整体右移"对齐 ——
                    // 直接按下标写第 cnt 个字节（cnt 在这一支正好数 0..ctb−1）。
                    else                   ctilde[cnt*8 +: 8] <= sha_out_data;
                    if (cnt == ((hsel == 2'd2) ? ({1'd0, cfg_ctb} - 8'd1) : 8'd63)) begin
                        fsm_sor <= 1'b0;
                        if (hsel == 2'd0) begin
                            hsel <= 2'd1;      // μ 好了，接着算 ρ''
                            st <= S_D_GO;
                        end else if (hsel == 2'd1) begin
                            // ρ'' 好了，进 ③ NTT prep
                            nstore <= 2'd0; poly <= 4'd0; cnt <= 8'd0; ph <= 1'b0;
                            st <= S_NT_LD;
                        end else begin
                            // c̃ 好了，进 SampleInBall
                            owner <= OWN_FSM;
                            st <= S_SIB_GO;
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
                        if (poly == nt_last) begin
                            poly <= 4'd0;
                            if (nstore == 2'd2) begin
                                kappa <= 16'd0;    // 进拒绝循环，κ 从 0 起
                                st <= S_EM_GO;
                            end else if (nstore == 2'd3) begin
                                // ŷ = NTT(y) 做完 → ⑤ MAC
                                vi <= 4'd0; vj <= 4'd0; owner <= OWN_UNI;
                                st <= S_A_GO;
                            end else begin
                                nstore <= nstore + 2'd1; st <= S_NT_LD;
                            end
                        end else begin
                            poly <= poly + 4'd1; st <= S_NT_LD;
                        end
                    end else begin cnt <= cnt + 8'd1; ph <= 1'b0; end
                end
            end

            // ---------- ④ y = ExpandMask(ρ'', κ+r)，r = poly = 0..ℓ-1 ----------
            // 拒绝循环入口。本里程碑只跑 κ=0 一轮，采出 y 存好、经 dbg 验，
            // 然后 done；⑤ 起再往下接 ŷ/w/…，并把 done 往后挪。
            S_EM_GO: begin
                owner <= OWN_EM;
                if (poly == 3'd0) begin reject <= 1'b0; weight <= 11'd0; end  // 新一轮清标志
                em_nonce <= kappa + {12'd0, poly};
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
                        if (poly == LM1) begin
                            poly <= 4'd0;
                            kappa <= kappa + {12'd0, cfg_l}; // κ += ℓ（采完 y 立即加，同 oracle）
                            owner <= OWN_FSM;
                            nstore <= 2'd3;           // ŷ = NTT(y) 就地覆盖
                            st <= S_NT_LD;
                        end else begin
                            poly <= poly + 4'd1; st <= S_EM_GO;
                        end
                    end else begin cnt <= cnt + 8'd1; ph <= 1'b0; end
                end
            end

            // ---------- ⑤ 对每个 i：acc[i]=Σ_j mont(Â[i][j]∘ŷ[j])；
            //            w=caddq(invNTT(reduce32(acc)))；(w0,w1)=Decompose(w) ----------
            // Â 现采现用（un_nonce=256·i+j），不存。逐系数 MAC，j==0 直接放、之后累加。
            S_A_GO: begin
                un_nonce <= {4'd0, vi, 4'd0, vj};   // 256·i + j
                un_start <= 1'b1;
                st <= S_A_WT;
            end
            S_A_WT: if (un_done) begin cnt <= 8'd0; ph <= 1'b0; st <= S_MAC; end
            // 每拍发一个系数；发完 256 个再等流水排空（下一段要读这一段写的累加器）
            S_MAC: begin
                if (!mm_last) begin
                    if (cnt == 8'd255) mm_last <= 1'b1;
                    else               cnt <= cnt + 8'd1;
                end else if (mm_empty) begin
                    cnt <= 8'd0; mm_last <= 1'b0; ph <= 1'b0;
                    if (vj == LM1) begin
                        vj <= 4'd0;
                        owner <= OWN_FSM;   // MAC 完这一 i，进 invNTT（FSM 用 NTT 核）
                        st <= S_RED;
                    end else begin
                        vj <= vj + 4'd1; owner <= OWN_UNI; st <= S_A_GO;
                    end
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
                        if (vi == KM1) begin
                            vi <= 4'd0;
                            // 进 ⑥：先把 w₁ 打包进 w1pk 缓冲
                            vi <= 4'd0; cnt <= 8'd0; ph <= 1'b0; wp_ptr <= 10'd0;
                            st <= S_W1_PK;
                        end else begin
                            vi <= vi + 4'd1;
                            vj <= 4'd0; owner <= OWN_UNI;
                            st <= S_A_GO;       // 下一个 i 的 MAC
                        end
                    end else begin cnt <= cnt + 8'd1; ph <= 1'b0; end
                end
            end

            // ---------- ⑥a w₁ → w1pk 缓冲（6 位/系数打包）----------
            // 与 KeyGen 的 S_S_MOVE 同构：两拍一个系数，pe_ir 反压时不推进。
            S_W1_PK: begin
                if (!ph) begin
                    ph <= 1'b1;
                end else if (p6_ir) begin
                    if (cnt == 8'd255) begin
                        cnt <= 8'd0; ph <= 1'b0;
                        if (vi == KM1) begin
                            vi <= 4'd0;
                            hsel <= 2'd2;      // 进 c̃ = H(μ‖w1pk)
                            st <= S_D_GO;
                        end else begin
                            vi <= vi + 4'd1;
                        end
                    end else begin
                        cnt <= cnt + 8'd1; ph <= 1'b0;
                    end
                end
            end

            // ---------- ⑥b c = SampleInBall(c̃) ----------
            S_SIB_GO: begin owner <= OWN_SIB; sb_start <= 1'b1; st <= S_SIB_WT; end
            S_SIB_WT: if (sb_done) begin cnt <= 8'd0; ph <= 1'b0; owner <= OWN_FSM; st <= S_SIB_MV; end
            S_SIB_MV: begin
                if (!ph) begin
                    ph <= 1'b1;
                end else begin
                    if (cnt == 8'd255) begin
                        cnt <= 8'd0; ph <= 1'b0;
                        st <= S_CN_LD;        // 进 ⑦：ĉ = NTT(c)
                    end else begin cnt <= cnt + 8'd1; ph <= 1'b0; end
                end
            end

            // ---------- ⑦ ĉ=NTT(c)；z[j]=reduce32(y[j]+invNTT(ĉ∘ŝ₁[j]))；‖z‖∞ 检查 ----------
            // ĉ = NTT(c) 就地覆盖 c 存储（c 之后不再需要原值）。
            S_CN_LD: begin
                if (!ph) begin ph <= 1'b1; end
                else begin
                    if (cnt == 8'd255) begin cnt <= 8'd0; ph <= 1'b0; st <= S_CN_GO; end
                    else begin cnt <= cnt + 8'd1; ph <= 1'b0; end
                end
            end
            S_CN_GO: begin nt_start <= 1'b1; nt_inv <= 1'b0; nt_lowseen <= 1'b0; st <= S_CN_ST; end
            S_CN_ST: begin
                if (!nt_done) nt_lowseen <= 1'b1;
                if (nt_lowseen && nt_done) begin cnt <= 8'd0; ph <= 1'b0; st <= S_CN_WB; end
            end
            S_CN_WB: begin
                if (!ph) begin ph <= 1'b1; end
                else begin
                    if (cnt == 8'd255) begin cnt <= 8'd0; ph <= 1'b0; vj <= 4'd0; st <= S_Z_MUL; end
                    else begin cnt <= cnt + 8'd1; ph <= 1'b0; end
                end
            end

            // 对每个 j：pointwise ĉ∘ŝ₁[vj] → invNTT 写口
            S_Z_MUL: begin
                if (!mm_last) begin
                    if (cnt == 8'd255) mm_last <= 1'b1;
                    else               cnt <= cnt + 8'd1;
                end else if (mm_empty) begin
                    cnt <= 8'd0; mm_last <= 1'b0; ph <= 1'b0; st <= S_Z_GO;
                end
            end
            S_Z_GO: begin nt_start <= 1'b1; nt_inv <= 1'b1; nt_lowseen <= 1'b0; st <= S_Z_ST; end
            S_Z_ST: begin
                if (!nt_done) nt_lowseen <= 1'b1;
                if (nt_lowseen && nt_done) begin cnt <= 8'd0; ph <= 1'b0; st <= S_Z_WB; end
            end
            // z[vj][n]=reduce32(y[vj][n]+cs₁[n])；越界置 reject
            S_Z_WB: begin
                if (!ph) begin ph <= 1'b1; end
                else begin
                    if (norm_bad) reject <= 1'b1;
                    if (cnt == 8'd255) begin
                        cnt <= 8'd0; ph <= 1'b0;
                        if (vj == LM1) begin
                            vj <= 4'd0; vi <= 4'd0;
                            st <= S_R_MUL;    // 进 ⑧ r₀
                        end else begin
                            vj <= vj + 4'd1; st <= S_Z_MUL;
                        end
                    end else begin cnt <= cnt + 8'd1; ph <= 1'b0; end
                end
            end

            // ---------- ⑧ r₀[i]=reduce32(w0[i]−invNTT(ĉ∘ŝ₂[i]))；‖r₀‖∞ 检查 ----------
            // r₀ 就地覆盖 w0 存储（w0 之后只在 ⑨ 用作 r₀）。
            S_R_MUL: begin
                if (!mm_last) begin
                    if (cnt == 8'd255) mm_last <= 1'b1;
                    else               cnt <= cnt + 8'd1;
                end else if (mm_empty) begin
                    cnt <= 8'd0; mm_last <= 1'b0; ph <= 1'b0; st <= S_R_GO;
                end
            end
            S_R_GO: begin nt_start <= 1'b1; nt_inv <= 1'b1; nt_lowseen <= 1'b0; st <= S_R_ST; end
            S_R_ST: begin
                if (!nt_done) nt_lowseen <= 1'b1;
                if (nt_lowseen && nt_done) begin cnt <= 8'd0; ph <= 1'b0; st <= S_R_WB; end
            end
            S_R_WB: begin
                if (!ph) begin ph <= 1'b1; end
                else begin
                    if (norm_bad) reject <= 1'b1;
                    if (cnt == 8'd255) begin
                        cnt <= 8'd0; ph <= 1'b0;
                        if (vi == KM1) begin vi <= 4'd0; st <= S_H_MUL; end
                        else begin vi <= vi + 4'd1; st <= S_R_MUL; end
                    end else begin cnt <= cnt + 8'd1; ph <= 1'b0; end
                end
            end

            // ---------- ⑨ ct₀=reduce32(invNTT(ĉ∘t̂₀[i]))；‖ct₀‖∞ 检查；
            //            hint=MakeHint(reduce32(r₀+ct₀), w₁[i])；权重累加 ----------
            S_H_MUL: begin
                if (!mm_last) begin
                    if (cnt == 8'd255) mm_last <= 1'b1;
                    else               cnt <= cnt + 8'd1;
                end else if (mm_empty) begin
                    cnt <= 8'd0; mm_last <= 1'b0; ph <= 1'b0; st <= S_H_GO;
                end
            end
            S_H_GO: begin nt_start <= 1'b1; nt_inv <= 1'b1; nt_lowseen <= 1'b0; st <= S_H_ST; end
            S_H_ST: begin
                if (!nt_done) nt_lowseen <= 1'b1;
                if (nt_lowseen && nt_done) begin cnt <= 8'd0; ph <= 1'b0; st <= S_H_WB; end
            end
            // 三拍：0 摆地址 / 1 锁存 ct₀ 与越界判定 / 2 算 hint 并写回
            S_H_WB: begin
                if (hph == 2'd0) begin
                    hph <= 2'd1;
                end else if (hph == 2'd1) begin
                    ct0_r <= comb_red;                          // comb_red 此时是 ct₀
                    nb_r  <= norm_bad;
                    hph   <= 2'd2;
                end else begin
                    hph <= 2'd0;
                    if (nb_r) reject <= 1'b1;                   // ‖ct₀‖∞ ≥ γ₂
                    if (hint_bit) weight <= weight + 11'd1;      // 累加 hint 权重
                    if (cnt == 8'd255) begin
                        cnt <= 8'd0; ph <= 1'b0;
                        if (vi == KM1) begin
                            vi <= 4'd0;
                            st <= S_REJ;      // ⑨ 完，进 ⑩ 拒绝判定
                        end else begin vi <= vi + 4'd1; st <= S_H_MUL; end
                    end else begin cnt <= cnt + 8'd1; ph <= 1'b0; end
                end
            end

            // ---------- ⑩ 拒绝判定 + sigEncode ----------
            // reject（z/r0/ct0 越界）或 权重>ω → 本轮作废，回 ④ 重采（κ 已在 ④ 递增）。
            S_REJ: begin
                if (reject || (weight > {3'd0, cfg_omega})) begin
                    poly <= 4'd0;             // 新一轮从 r=0 开始
                    st <= S_EM_GO;
                end else begin
                    cnt <= 8'd0; st <= S_SIG_CT;
                end
            end

            // sig[0..31] = c̃（ctilde 是寄存器，直接按字节写，无同步读延迟）
            S_SIG_CT: begin
                if (cnt == {1'd0, cfg_ctb} - 8'd1) begin
                    vj <= 4'd0; cnt <= 8'd0; ph <= 1'b0; sigptr <= SIG_Z0;
                    st <= S_SIG_Z;
                end else begin
                    cnt <= cnt + 8'd1;
                end
            end

            // z 段：4 条 z 连续打包（各 576 字节字节对齐，与逐条打包等价）。
            // 只在最开头 clr 一次（同 w₁ 的坑：换条 clr 会抹掉滞后的待吐字节）。
            S_SIG_Z: begin
                if (!ph) begin
                    ph <= 1'b1;
                end else if (pz_ir) begin
                    if (cnt == 8'd255) begin
                        cnt <= 8'd0; ph <= 1'b0;
                        if (vj == LM1) begin vj <= 4'd0; st <= S_SIG_ZD; end
                        else vj <= vj + 4'd1;
                    end else begin
                        cnt <= cnt + 8'd1; ph <= 1'b0;
                    end
                end
            end
            // 等打包器把最后 1~2 字节吐完（sigptr 走到 hint 段起点）
            S_SIG_ZD: if (sigptr == SIG_H0) begin
                cnt <= 8'd0; st <= S_HP_CLR;
            end

            // 清 hint 下标/填充区（ω=80 字节）。sig RAM 无复位，上一条签名的残留
            // 会污染填充区 —— 本条 hint 权重更小时那些字节没被覆盖，就对不上 ACVP。
            S_HP_CLR: begin
                if (cnt == OMEGA[7:0] - 8'd1) begin
                    vi <= 4'd0; cnt <= 8'd0; ph <= 1'b0; hidx <= 8'd0;
                    st <= S_HP;
                end else begin
                    cnt <= cnt + 8'd1;
                end
            end

            // HintBitPack：对每个 i 扫 hint[i][0..255]，为 1 的下标顺次写进 sig；
            // 每条末尾写累计计数。填充区靠 sig RAM 的 0 初值天然为 0。
            S_HP: begin
                if (!ph) begin
                    ph <= 1'b1;                       // 摆 hint 读地址
                end else begin
                    if (hn_dout) hidx <= hidx + 8'd1; // 命中：写下标（组合）+ hidx++
                    if (cnt == 8'd255) begin
                        cnt <= 8'd0; ph <= 1'b0; st <= S_HP_CNT;
                    end else begin
                        cnt <= cnt + 8'd1; ph <= 1'b0;
                    end
                end
            end
            // 写累计计数 sig[SIG_H0+ω+vi] = hidx
            S_HP_CNT: begin
                if (vi == KM1) st <= S_FIN;
                else begin vi <= vi + 4'd1; cnt <= 8'd0; ph <= 1'b0; st <= S_HP; end
            end

            // 回到 S_IDLE 的同时解冻配置：下一次 start 才好按新的 pset 译码
            S_FIN: begin done <= 1'b1; busy_r <= 1'b0; st <= S_IDLE; end
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
        OWN_SIB: begin
            sha_start = sb_ss; sha_rate = sb_sr; sha_suffix = sb_su;
            sha_in_valid = sb_siv; sha_in_data = sb_sid; sha_in_flush = sb_sif;
            sha_out_ready = sb_sor;
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
        s1_raddr = {dbg_sel[2:0], dbg_idx};   // 默认给调试口
        s2_raddr = {dbg_sel[2:0], dbg_idx};
        t0_raddr = {dbg_sel[2:0], dbg_idx};
        // S_HDR 读 sk[0..127]（地址=cnt）；S_UNP 读打包区（地址=skp）
        sk_raddr = (st == S_HDR) ? {5'd0, cnt} : skp;
        // ② μ 支吸收 ctx/msg 时，按 ai 摆读地址（ai 跨两拍稳定，组合驱动即可）
        msg_raddr = msg_off[12:0];
        ctx_raddr = ctx_off[7:0];

        eu_clr = 1'b0; eu_iv = 1'b0; eu_or = 1'b0;
        tu_clr = 1'b0; tu_iv = 1'b0; tu_or = 1'b0;

        nt_we = 1'b0; nt_waddr = 8'd0; nt_wdata = 32'd0; nt_raddr = cnt;

        y_we = 1'b0; y_waddr = 10'd0; y_din = 32'd0;
        y_raddr = {dbg_sel[2:0], dbg_idx};    // 默认给调试口
        yh_we = 1'b0; yh_waddr = 10'd0; yh_din = 32'd0;
        yh_raddr = {dbg_sel[2:0], dbg_idx};
        em_rd_addr = cnt;
        ac_we = 1'b0; ac_waddr = 10'd0; ac_din = 32'd0;
        ac_raddr = {dbg_sel[2:0], dbg_idx};
        w0_we = 1'b0; w0_waddr = 10'd0; w0_din = 32'd0;
        w0_raddr = {dbg_sel[2:0], dbg_idx};
        w1_we = 1'b0; w1_waddr = 10'd0; w1_din = 6'd0;
        w1_raddr = {dbg_sel[2:0], dbg_idx};
        un_rd_addr = cnt;
        c_we = 1'b0; c_waddr = 8'd0; c_din = 32'd0;
        c_raddr = dbg_idx;                   // c 只有 256 项，dbg_idx 直接寻址
        z_we = 1'b0; z_waddr = 10'd0; z_din = 32'd0;
        z_raddr = {dbg_sel[2:0], dbg_idx};
        hn_we = 1'b0; hn_waddr = 10'd0; hn_din = 1'b0;
        hn_raddr = {dbg_sel[2:0], dbg_idx};
        p6_clr = 1'b0; p6_iv = 1'b0;
        wp_we = 1'b0; wp_waddr = 10'd0; wp_din = 8'd0;
        wp_raddr = (ai >= 14'd64) ? (ai[9:0] - 10'd64) : 10'd0;   // c̃ 吸收时读 w1pk
        sb_rd_addr = cnt;
        sig_we = 1'b0; sig_waddr = 12'd0; sig_din = 8'd0;
        pz_clr = 1'b0; pz_iv = 1'b0;

        // S_UNP_I：进循环前清两个累加器
        if (st == S_UNP_I) begin eu_clr = 1'b1; tu_clr = 1'b1; end

        // ④ 采样结果 → y[poly]（两拍相位：ph=0 摆 em 读地址，ph=1 写 y）
        if (st == S_EM_MV) begin
            em_rd_addr = cnt;
            if (ph) begin y_we = 1'b1; y_waddr = {poly[2:0], cnt}; y_din = em_rd_data; end
        end

        // ⑤ MAC：Â[cnt]·ŷ[vj][cnt] 累加到 acc[vi][cnt]（j==0 直接放）
        // 写回地址与"旧累加值"都来自 tag，不再用当拍的 cnt / ac_dout ——
        // 结果比读地址晚 6 拍，用当拍的 cnt 会写到错误的系数上。
        if (st == S_MAC) begin
            un_rd_addr = cnt;
            yh_raddr   = {vj[2:0], cnt};
            ac_raddr   = {vi[2:0], cnt};
            if (mm_ov) begin
                ac_we    = 1'b1;
                ac_waddr = {vi[2:0], mm_owaddr};
                ac_din   = (vj == 3'd0) ? mm_t : (mm_oacc + mm_t);
            end
        end
        // ⑤ 装载：reduce32(acc[vi]) → invNTT 写口
        if (st == S_RED) begin
            ac_raddr = {vi[2:0], cnt};
            if (ph) begin nt_we = 1'b1; nt_waddr = cnt; nt_wdata = red_out; end
        end
        // ⑤ 写回：caddq(invNTT[cnt]) → decompose → w0[vi]/w1[vi]
        if (st == S_DEC) begin
            nt_raddr = cnt;
            if (ph) begin
                w0_we = 1'b1; w0_waddr = {vi[2:0], cnt}; w0_din = dec_a0;
                w1_we = 1'b1; w1_waddr = {vi[2:0], cnt}; w1_din = dec_a1;
            end
        end

        // ⑥a w₁ → w1pk：读 w1[vi][cnt] 喂 6 位打包器
        // ⚠️ 只在最开头清一次累加器，**不能每条清**：每条 w₁ 恰好 192 字节（256×6=1536
        // 位，字节对齐），4 条连续打包与逐条打包等价；而打包器最后 1~2 字节比最后一个
        // 系数晚出（滞后），若在换条时 clr 会把这些待吐字节抹掉 —— KeyGen 靠采样器的
        // 间隔盖过了这段滞后，这里 w₁ 各条背靠背没有间隔，会中招。
        if (st == S_W1_PK) begin
            w1_raddr = {vi[2:0], cnt};
            if (!ph && cnt == 8'd0 && vi == 3'd0) p6_clr = 1'b1;   // 仅最开头清一次
            if (ph) p6_iv = 1'b1;                                 // 数据到位，喂系数
        end
        // 打包器吐字节 → w1pk 缓冲
        if (p6_ov) begin wp_we = 1'b1; wp_waddr = wp_ptr; wp_din = p6_ob; end

        // ⑥b SampleInBall 结果 → c 存储（两拍：ph=0 摆 sb 读地址，ph=1 写 c）
        if (st == S_SIB_MV) begin
            sb_rd_addr = cnt;
            if (ph) begin c_we = 1'b1; c_waddr = cnt; c_din = sb_rd_data; end
        end

        // ⑦ ĉ = NTT(c)：装载 c → NTT，写回 NTT → c（就地）
        if (st == S_CN_LD) begin
            c_raddr = cnt;
            if (ph) begin nt_we = 1'b1; nt_waddr = cnt; nt_wdata = c_dout; end
        end
        if (st == S_CN_WB) begin
            nt_raddr = cnt;
            if (ph) begin c_we = 1'b1; c_waddr = cnt; c_din = nt_rdata; end
        end
        // ⑦ z：pointwise ĉ∘ŝ₁[vj] → invNTT 写口
        if (st == S_Z_MUL) begin
            c_raddr  = cnt;
            s1_raddr = {vj[2:0], cnt};
            if (mm_ov) begin nt_we = 1'b1; nt_waddr = mm_owaddr; nt_wdata = mm_t; end
        end
        // ⑦ z 写回：z[vj]=reduce32(y[vj]+invNTT)
        if (st == S_Z_WB) begin
            nt_raddr = cnt;
            y_raddr  = {vj[2:0], cnt};
            if (ph) begin z_we = 1'b1; z_waddr = {vj[2:0], cnt}; z_din = comb_red; end
        end

        // ⑧ r₀：pointwise ĉ∘ŝ₂[vi] → invNTT 写口
        if (st == S_R_MUL) begin
            c_raddr  = cnt;
            s2_raddr = {vi[2:0], cnt};
            if (mm_ov) begin nt_we = 1'b1; nt_waddr = mm_owaddr; nt_wdata = mm_t; end
        end
        // ⑧ r₀ 写回：r0[vi]=reduce32(w0[vi]−cs₂)，就地覆盖 w0（read-first：读旧 w0）
        if (st == S_R_WB) begin
            nt_raddr = cnt;
            w0_raddr = {vi[2:0], cnt};
            if (ph) begin w0_we = 1'b1; w0_waddr = {vi[2:0], cnt}; w0_din = comb_red; end
        end

        // ⑨ hint：pointwise ĉ∘t̂₀[vi] → invNTT 写口
        if (st == S_H_MUL) begin
            c_raddr  = cnt;
            t0_raddr = {vi[2:0], cnt};
            if (mm_ov) begin nt_we = 1'b1; nt_waddr = mm_owaddr; nt_wdata = mm_t; end
        end
        // ⑨ hint 写回：ct0=reduce32(invNTT)；a0=reduce32(r0[vi]+ct0)；hint=MakeHint(a0,w1[vi])
        if (st == S_H_WB) begin
            nt_raddr = cnt;
            w0_raddr = {vi[2:0], cnt};   // r0（⑧ 覆盖进 w0）
            w1_raddr = {vi[2:0], cnt};
            // 第 3 拍才写：hint 位这时才算得出来（ct₀ 上一拍才进寄存器）
            if (hph == 2'd2) begin hn_we = 1'b1; hn_waddr = {vi[2:0], cnt}; hn_din = hint_bit; end
        end

        // ⑩ sig[0..31] = c̃
        if (st == S_SIG_CT) begin
            sig_we = 1'b1; sig_waddr = {5'd0, cnt}; sig_din = ctilde[cnt*8 +: 8];
        end
        // ⑩ z 段：读 z[vj][cnt] 喂 18 位打包器；只在最开头 clr 一次
        if (st == S_SIG_Z) begin
            z_raddr = {vj[2:0], cnt};
            if (!ph && cnt == 8'd0 && vj == 3'd0) pz_clr = 1'b1;
            if (ph) pz_iv = 1'b1;
        end
        // z 打包器吐字节 → sig[sigptr]（S_SIG_Z / S_SIG_ZD 都可能吐）
        if (pz_ov) begin sig_we = 1'b1; sig_waddr = sigptr; sig_din = pz_ob; end
        // ⑩ 清 hint 下标/填充区：sig[SIG_H0+cnt] = 0（cnt=0..ω-1）
        if (st == S_HP_CLR) begin
            sig_we = 1'b1; sig_waddr = SIG_H0 + {5'd0, cnt}; sig_din = 8'd0;
        end
        // ⑩ HintBitPack 扫描：命中就把下标 cnt 写进 sig[SIG_H0+hidx]
        if (st == S_HP) begin
            hn_raddr = {vi[2:0], cnt};
            if (ph && hn_dout) begin
                sig_we = 1'b1; sig_waddr = SIG_H0 + {5'd0, hidx}; sig_din = cnt;
            end
        end
        // ⑩ 每条 hint 末尾写累计计数
        if (st == S_HP_CNT) begin
            sig_we = 1'b1;
            sig_waddr = SIG_H0 + OMEGA + {9'd0, vi};
            sig_din = hidx;
        end

        // ③/⑤a NTT 装载：选中 store[poly] → NTT 写口（nstore：0=s₁,1=s₂,2=t₀,3=y）
        if (st == S_NT_LD) begin
            case (nstore)
                2'd0: s1_raddr = {poly[2:0], cnt};
                2'd1: s2_raddr = {poly[2:0], cnt};
                2'd2: t0_raddr = {poly[2:0], cnt};
                default: y_raddr = {poly[2:0], cnt};
            endcase
            if (ph) begin nt_we = 1'b1; nt_waddr = cnt; nt_wdata = store_dout; end
        end
        // ③/⑤a NTT 写回：NTT 读口 → 选中 store[poly]
        if (st == S_NT_WB) begin
            nt_raddr = cnt;
            if (ph) begin
                case (nstore)
                    2'd0: begin s1_we = 1'b1; s1_waddr = {poly[2:0], cnt}; s1_din = nt_rdata; end
                    2'd1: begin s2_we = 1'b1; s2_waddr = {poly[2:0], cnt}; s2_din = nt_rdata; end
                    2'd2: begin t0_we = 1'b1; t0_waddr = {poly[2:0], cnt}; t0_din = nt_rdata; end
                    default: begin yh_we = 1'b1; yh_waddr = {poly[2:0], cnt}; yh_din = nt_rdata; end
                endcase
            end
        end

        if (st == S_UNP && !t0phase) begin
            if (eu_ov) begin
                eu_or = 1'b1;                 // 抽系数
                // 存进 s₁（poly 0..3）或 s₂（poly 4..7）
                // poly < ℓ 是 s₁，之后是 s₂（下标要减 ℓ）—— 不能用 poly[2] 分组，
                // 那是 ℓ=k=4 时的巧合；65 是 ℓ=5,k=6，87 是 ℓ=7,k=8。
                if (!s2_phase) begin
                    s1_we = 1'b1; s1_waddr = {sidx, cnt}; s1_din = eta_coef;
                end else begin
                    s2_we = 1'b1; s2_waddr = {sidx, cnt}; s2_din = eta_coef;
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
                t0_we = 1'b1; t0_waddr = {poly[2:0], cnt}; t0_din = t0_coef;
                if (cnt == 8'd255) tu_clr = 1'b1;
            end else if (feed) begin
                tu_iv = 1'b1;
            end
        end
    end
endmodule

`default_nettype wire
