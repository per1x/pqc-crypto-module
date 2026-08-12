// mlkem_decaps —— 完整的 ML-KEM 解封装核（FIPS 203 Alg 18 + Alg 15）
//
//     dk 字节流 = ŝ(384k) ‖ ek(384k+32) ‖ h(32) ‖ z(32)
//     c  字节流 = c₁(32·du·k) ‖ c₂(32·dv)
//
//     ① 解密   u = Decompress_du(c₁)、v = Decompress_dv(c₂)
//              w = v − NTT⁻¹(ŝᵀ ∘ NTT(u))
//              m′ = ByteEncode₁(Compress₁(w))
//     ② 重加密 (K′, c′) = Encaps_internal(ek, m′)
//     ③ 选择   K = (c′ == c) ? K′ : J(z‖c)
//
// ============================================================================
// 【为什么重加密是直接例化 mlkem_encaps，而不是再写一遍】
// ============================================================================
// 这不是取巧，是 FIPS 203 的定义本身。Decaps_internal 第 3~5 步是
//
//     (K′, r′) = G(m′ ‖ h)          c′ = K-PKE.Encrypt(ek, m′, r′)
//
// 而 Encaps_internal(ek, m′) 第 1~2 步是
//
//     (K,  r)  = G(m′ ‖ H(ek))      c  = K-PKE.Encrypt(ek, m′, r)
//
// 只要 h == H(ek)（合法 dk 的定义），两者**逐比特相同**。所以"重加密"就是
// 原样跑一遍封装核 —— 再写一份 Encrypt 只会多出一个可以和 Encaps 各错各的地方。
//
// ⚠️ 一处与标准字面的差别，说清楚：标准里 G 的第二个输入取自 dk 里存的 h，
//    本核取的是**从 ek 重新算出来的 H(ek)**（因为它在封装核内部）。
//    对任何合法 dk 这两者相等；不等则说明 dk 自身不自洽，而 FIPS 203 §7.3
//    本来就要求解封装前做这项检查。本核顺手把这项检查做了，结果由
//    `dk_hash_ok` 给出 —— 也就是说这里比照字面实现**更严**，不是更松。
//
// ============================================================================
// 【常量时间：这是本模块唯一不能妥协的地方】
// ============================================================================
// 密文比对一旦提前退出，就能从"解封装用了多久"读出 c′ 与 c 从第几个字节开始
// 不同 —— 而 c′ 是攻击者送进来的 c 解密再加密的结果，这条时序信道足以
// 逐字节恢复明文（Fujisaki-Okamoto 变换里最经典的一个坑）。所以：
//
//   · `cmp_diff` 只做**累积或**，状态机的任何跳转都不看它；
//   · 比对固定跑满 clen 个字节，没有任何 early-out 分支；
//   · J(z‖c) **无条件**先算好，不是"发现不同了才去算"；
//   · 最后用位掩码在 K′ 与 J(z‖c) 之间选，不是 if。
//
// 整个核的拍数与数据无关：dk/c 的长度只由 param_set 决定，采样、NTT、基乘、
// 位解包的推进节奏都只看比特计数不看比特取值。**同一个参数集下，
// 解封装成功与失败的拍数完全一样。**
//
// ============================================================================
// 【存储分工】
// ============================================================================
//   u_bank  多项式存储：槽 0..k-1 = ŝ（从 dk 解出），槽 4 = 基乘累加器
//   u_ntt   NTT 核的系数存储：正变换时装 u[i]，逆变换后装 NTT⁻¹(ŝᵀû)
//   u_ekbuf ek 字节缓冲（≤1568 B）：dk 进来时存下，重加密时回放给封装核
//   u_cbuf  c 字节缓冲（≤1568 B）：先整条存下，再按自己的节奏解包
//
// c 先整条缓冲再解包，是因为 c 那一路要同时喂三个去处（J 的海绵、缓冲、
// 位解包器），而位解包的下游还要停下来跑 NTT。三者的背压耦在一起会让
// c_ready 的条件变成一团 —— 多花 1568 拍换一个说得清的时序，值。
`default_nettype none

module mlkem_decaps #(
    // ⚠️ 仿真专用：接出多项式存储的读口。综合时必须是 0 —— 那个口直连 ŝ。
    parameter integer DEBUG_BANK = 0
) (
    input  wire        clk,
    input  wire        rst_n,

    input  wire [1:0]  param_set,    // 0=512 1=768 2=1024

    input  wire        start,        // 脉冲
    output reg         done,         // 电平，保持到下一次 start

    // ---- dk 输入字节流（768k + 96 字节）----
    input  wire        dk_valid,
    output wire        dk_ready,
    input  wire [7:0]  dk_data,

    // ---- c 输入字节流（32·(du·k + dv) 字节）----
    input  wire        c_valid,
    output wire        c_ready,
    input  wire [7:0]  c_data,

    // ---- 输出：共享密钥 K，32 字节 ----
    output wire        out_valid,
    input  wire        out_ready,
    output wire [7:0]  out_data,
    output wire        out_last,

    // FIPS 203 §7.3 的 dk 自洽性检查：H(ek) 是否等于 dk 里存的 h。
    // done 为高时有效。为低说明送进来的 dk 本身不合法。
    output reg         dk_hash_ok,

    input  wire [10:0] dbg_addr,
    output wire signed [15:0] dbg_data
);
    // ================= 参数集 =================
    wire [2:0] k_now   = (param_set == 2'd0) ? 3'd2 :
                         (param_set == 2'd1) ? 3'd3 : 3'd4;
    wire       d11_now = (param_set == 2'd2);

    reg [1:0] ps_r;                  // 转给封装核用，整个操作期间不变
    reg [2:0] k_r;
    reg       d11_r;

    wire [3:0] du = d11_r ? 4'd11 : 4'd10;
    wire [3:0] dv = d11_r ? 4'd5  : 4'd4;

    // dk 的四段边界。384 = 256+128、768 = 512+256，都是两项移位和 ——
    // 这类常数拆错过一次（encaps 的 tbytes 拆成了 240），所以每个都标出算式。
    wire [12:0] sbytes = {2'b0, k_r, 8'd0} + {3'b0, k_r, 7'd0};        // 384k
    wire [12:0] eklen  = sbytes + 13'd32;                              // 384k+32
    wire [12:0] ekend  = sbytes + eklen;                               // 768k+32
    wire [12:0] hend   = ekend  + 13'd32;
    wire [12:0] dklen  = ekend  + 13'd64;                              // 768k+96

    // c 的两段。32·du·k = 320k（du=10）或 352k（du=11）：
    //   320 = 256+64，352 = 256+64+32。
    wire [12:0] c1len = {2'b0, k_r, 8'd0} + {4'b0, k_r, 6'd0}
                        + (d11_r ? {5'b0, k_r, 5'd0} : 13'd0);
    wire [12:0] clen  = c1len + {4'b0, dv, 5'd0};                      // + 32·dv

    // ================= 多项式存储 =================
    //   槽 0..k-1 : ŝ[0..k-1]（从 dk 的 ByteDecode12 解出，原值 [0, 2¹²)）
    //   槽 4      : 基乘累加器
    localparam [2:0] SL_S   = 3'd0;
    localparam [2:0] SL_ACC = 3'd4;

    reg         ba_we, bb_we;
    reg  [10:0] ba_addr, bb_addr;
    reg  signed [15:0] ba_din, bb_din;
    wire signed [15:0] ba_dout, bb_dout;

    ram_dp #(.DW(16), .AW(11)) u_bank (
        .clk(clk),
        .a_we(ba_we), .a_addr(ba_addr), .a_din(ba_din), .a_dout(ba_dout),
        .b_we(bb_we), .b_addr(bb_addr), .b_din(bb_din), .b_dout(bb_dout));

    // ================= 字节缓冲 =================
    // 两块都是 A 口写、B 口读：写在输入阶段，读在处理阶段，时间上不重叠，
    // 但分开两个口就不必去想"同址同拍读写"这件事。
    reg         eka_we;
    reg  [10:0] eka_addr;
    reg  [7:0]  eka_din;
    reg  [10:0] ekb_addr;
    wire [7:0]  ekb_dout;

    ram_dp #(.DW(8), .AW(11)) u_ekbuf (
        .clk(clk),
        .a_we(eka_we), .a_addr(eka_addr), .a_din(eka_din), .a_dout(),
        .b_we(1'b0),   .b_addr(ekb_addr), .b_din(8'd0),    .b_dout(ekb_dout));

    reg         cba_we;
    reg  [10:0] cba_addr;
    reg  [7:0]  cba_din;
    reg  [10:0] cbb_addr;
    wire [7:0]  cbb_dout;

    ram_dp #(.DW(8), .AW(11)) u_cbuf (
        .clk(clk),
        .a_we(cba_we), .a_addr(cba_addr), .a_din(cba_din), .a_dout(),
        .b_we(1'b0),   .b_addr(cbb_addr), .b_din(8'd0),    .b_dout(cbb_dout));

    // ================= 状态机 =================
    localparam [4:0]
        S_IDLE      = 5'd0,
        S_HEK_START = 5'd1,          // 起 SHA3-256（算 H(ek)）
        S_DK_IN     = 5'd2,
        S_HEK_FLUSH = 5'd3,
        S_HEK_SQ    = 5'd4,
        S_J_START   = 5'd5,          // 起 SHAKE256（算 J(z‖c)）
        S_J_Z       = 5'd6,
        S_C_IN      = 5'd7,
        S_J_FLUSH   = 5'd8,
        S_J_SQ      = 5'd9,
        S_ACC_CLR   = 5'd10,
        S_U_WAIT    = 5'd11,
        S_U_STEP    = 5'd12,
        S_U_NTT     = 5'd13,
        S_MAC_A0    = 5'd14,
        S_MAC_A1    = 5'd15,
        S_MAC_C     = 5'd16,
        S_MAC_M     = 5'd17,
        S_MAC_T     = 5'd18,
        S_MAC_W0    = 5'd19,
        S_MAC_W1    = 5'd20,
        S_A2N_RD    = 5'd21,
        S_A2N       = 5'd22,
        S_INV       = 5'd23,
        S_V_WAIT    = 5'd24,
        S_V_STEP    = 5'd25,
        S_V_W       = 5'd26,
        S_V_BIT     = 5'd27,
        S_RE_START  = 5'd28,
        S_RE_RUN    = 5'd29,
        S_OUT       = 5'd30,
        S_DONE      = 5'd31;

    reg [4:0] state;

    // ================= 海绵：先算 H(ek)，再算 J(z‖c) =================
    reg  [7:0]  sha_rate, sha_suffix;
    reg         sha_start, sha_in_valid, sha_in_flush, sha_out_ready;
    reg  [7:0]  sha_in_data;
    wire        sha_in_ready, sha_out_valid;
    wire [7:0]  sha_out_data;

    sha3_core u_sha3 (
        .clk(clk), .rst_n(rst_n),
        .rate_bytes(sha_rate), .suffix(sha_suffix),
        .start(sha_start), .zeroize(1'b0),
        .in_valid(sha_in_valid), .in_ready(sha_in_ready), .in_data(sha_in_data),
        .in_flush(sha_in_flush),
        .out_valid(sha_out_valid), .out_ready(sha_out_ready), .out_data(sha_out_data),
        .busy(), .absorbing(), .squeezing(),
        .ext_start(1'b0), .ext_done(), .ext_wr_en(1'b0), .ext_wr_addr(5'd0),
        .ext_wr_data(64'd0), .ext_rd_addr(5'd0), .ext_rd_data());

    // ================= NTT =================
    reg         ntt_start, ntt_inverse, ntt_wr_en;
    reg  [7:0]  ntt_wr_addr, ntt_rd_addr;
    reg  signed [15:0] ntt_wr_data;
    wire        ntt_done;
    wire signed [15:0] ntt_rd_data;

    ntt_core u_ntt (
        .clk(clk), .rst_n(rst_n),
        .start(ntt_start), .inverse(ntt_inverse), .done(ntt_done),
        .wr_en(ntt_wr_en), .wr_addr(ntt_wr_addr), .wr_data(ntt_wr_data),
        .rd_addr(ntt_rd_addr), .rd_data(ntt_rd_data));

    // ================= 位解包：c₁ 用 du、c₂ 用 dv =================
    reg        v_phase;              // 0 = 正在解 c₁（u），1 = 正在解 c₂（v）
    reg        bu_in_valid, bu_out_ready;
    reg  [7:0] bu_in_data;
    wire       bu_in_ready, bu_out_valid;
    wire [11:0] bu_out_data;

    // d 只在两个多项式之间变。256·d 比特总是整字节，所以换 d 的那一刻
    // 位累加器必然是空的 —— 不会把上一段的残余比特带进下一段。
    wire [3:0] bu_d = v_phase ? dv : du;

    mlkem_bitunpack u_bu (
        .clk(clk), .rst_n(rst_n), .d(bu_d),
        .in_valid(bu_in_valid), .in_ready(bu_in_ready), .in_data(bu_in_data),
        .out_valid(bu_out_valid), .out_ready(bu_out_ready), .out_data(bu_out_data));

    // du ∈ {10,11}、dv ∈ {4,5} 各要一份解压：D 在移位量上，推不出来
    wire signed [15:0] dc10, dc11, dc4, dc5;
    mlkem_decompress #(.D(10)) u_dc10 (.val(bu_out_data[9:0]),  .coeff(dc10));
    mlkem_decompress #(.D(11)) u_dc11 (.val(bu_out_data[10:0]), .coeff(dc11));
    mlkem_decompress #(.D(4))  u_dc4  (.val(bu_out_data[3:0]),  .coeff(dc4));
    mlkem_decompress #(.D(5))  u_dc5  (.val(bu_out_data[4:0]),  .coeff(dc5));

    wire signed [15:0] u_coeff = d11_r ? dc11 : dc10;
    wire signed [15:0] v_coeff = d11_r ? dc5  : dc4;

    // ================= ByteDecode12：dk 里的 ŝ =================
    reg  [1:0]  grp;                 // 3 字节一组的组内计数
    reg  [15:0] grp_buf;
    wire [11:0] dec_c0, dec_c1;
    mlkem_decode12 u_dec (.bytes_in({dk_data, grp_buf}), .c0(dec_c0), .c1(dec_c1));

    // ŝ 与 encaps 里的 t̂ 一样，存 ByteDecode12 的原值 [0, 2¹²)
    wire signed [15:0] dec_s0 = {4'd0, dec_c0};
    wire signed [15:0] dec_s1 = {4'd0, dec_c1};

    // ================= 基乘（两级流水，见 basemul.v）=================
    reg  [7:0]  pair;
    reg  signed [15:0] a0_r, a1_r, b0_r, b1_r, acc0_r, acc1_r;
    reg  signed [15:0] t_ab_r, bm0_r, bm1_r;

    // ζ 表与取法都在 basemul.v 的 mlkem_bmzeta 里 —— 三个核共用一份源码
    wire signed [15:0] bm_zeta;
    mlkem_bmzeta u_bmz (.pair(pair), .zeta(bm_zeta));

    wire signed [15:0] t_ab;
    mlkem_basemul_head u_bm_h (.a1(a1_r), .b1(b1_r), .t_a1b1(t_ab));

    wire signed [15:0] bm_r0, bm_r1;
    mlkem_basemul_tail u_bm_t (
        .a0(a0_r), .a1(a1_r), .b0(b0_r), .b1(b1_r), .zeta(bm_zeta),
        .t_a1b1(t_ab_r), .r0(bm_r0), .r1(bm_r1));

    wire signed [15:0] acc0_next, acc1_next;
    barrett_reduce u_br0 (.a(acc0_r + bm0_r), .r(acc0_next));
    barrett_reduce u_br1 (.a(acc1_r + bm1_r), .r(acc1_next));

    // ================= w = v − NTT⁻¹(ŝᵀû)，再 Compress₁ =================
    // 三级流水，不是为了吞吐是为了时序：decompress 一次乘法、barrett 两次、
    // compress 又一次，四级 DSP 串起来在这颗片子上跑不到 100 MHz
    // （encaps 的输出尾巴就是这么栽的，见 docs/fpga-进展.md 的 S4b）。
    reg signed [15:0] v_r, su_r, w_r;
    wire signed [15:0] w_bar;
    barrett_reduce u_w_br (.a(v_r - su_r), .r(w_bar));

    wire mbit;
    mlkem_compress #(.D(1)) u_cmp1 (.coeff(w_r), .val(mbit));

    // ================= 重加密：直接例化封装核 =================
    reg          enc_start;
    reg  [255:0] m_r;
    wire         enc_done, enc_ek_ready, enc_out_valid, enc_out_last;
    wire [7:0]   enc_out_data;

    reg  [7:0]  ekb_r;               // 从 ek 缓冲取出的一个字节
    reg         ekb_v, ekb_wait;
    reg  [12:0] ekpos;

    wire enc_ek_valid = ekb_v && (state == S_RE_RUN);

    mlkem_encaps #(.DEBUG_BANK(0)) u_enc (
        .clk(clk), .rst_n(rst_n),
        .param_set(ps_r), .m_in(m_r),
        .start(enc_start), .done(enc_done),
        .ek_valid(enc_ek_valid), .ek_ready(enc_ek_ready), .ek_data(ekb_r),
        .out_valid(enc_out_valid), .out_ready(1'b1),
        .out_data(enc_out_data), .out_last(enc_out_last),
        .dbg_addr(12'd0), .dbg_data());

    // ================= 状态寄存器 =================
    reg [255:0] h_r, z_r, hek_r, kbar_r, kprime_r;
    reg [12:0]  dkcnt, cpos, cbcnt;
    reg [10:0]  rbcnt;               // 封装核已经吐出的字节数（32 + clen）
    reg [8:0]   ucnt, vcnt, acnt;
    reg [5:0]   ocnt;
    reg [2:0]   idx_i, tp;
    reg [7:0]   tn;
    reg         ntt_kicked;

    // ⚠️ 常量时间的核心：只累积，不影响任何跳转。
    reg cmp_diff;

    // ================= 组合：握手与地址 =================
    wire in_s_rng  = (dkcnt <  sbytes);
    wire in_ek_rng = (dkcnt >= sbytes) && (dkcnt < ekend);
    wire in_h_rng  = (dkcnt >= ekend)  && (dkcnt < hend);

    // ek 那一段要同时进海绵和缓冲，海绵每 136 字节要停 24 拍做置换；
    // 其余三段不进海绵，随到随收。
    assign dk_ready = (state == S_DK_IN) && (!in_ek_rng || sha_in_ready);
    assign c_ready  = (state == S_C_IN)  && sha_in_ready;

    wire dk_fire = dk_valid && dk_ready;
    wire c_fire  = c_valid  && c_ready;

    // 比对：封装核每吐一个字节就比一个（前 32 个是 K′，不参与比对）
    wire cmp_fire = (state == S_RE_RUN) && enc_out_valid && (rbcnt >= 11'd32);

    // 位掩码选，不是 if —— 两条路的时序完全一样
    wire [255:0] kmask = {256{cmp_diff}};
    wire [255:0] kout  = (kprime_r & ~kmask) | (kbar_r & kmask);

    assign out_valid = (state == S_OUT);
    assign out_data  = kout[{ocnt[4:0], 3'd0} +: 8];
    assign out_last  = (state == S_OUT) && (ocnt == 6'd31);

    wire out_fire = out_valid && out_ready;

    always @(*) begin
        sha_rate      = 8'd136;
        sha_suffix    = 8'h1f;                 // 默认 SHAKE
        sha_start     = 1'b0;
        sha_in_valid  = 1'b0;
        sha_in_data   = 8'd0;
        sha_in_flush  = 1'b0;
        sha_out_ready = 1'b0;

        ntt_start   = 1'b0;
        ntt_wr_en   = 1'b0;
        ntt_wr_addr = 8'd0;
        ntt_wr_data = 16'sd0;
        ntt_rd_addr = 8'd0;

        ba_we = 1'b0; ba_addr = 11'd0; ba_din = 16'sd0;
        bb_we = 1'b0; bb_addr = 11'd0; bb_din = 16'sd0;

        eka_we = 1'b0; eka_addr = 11'd0; eka_din = 8'd0;
        cba_we = 1'b0; cba_addr = 11'd0; cba_din = 8'd0;

        // ek 缓冲的读地址一直跟着 ekpos；c 缓冲的读口在解包阶段跟 cpos，
        // 在比对阶段跟 cbcnt（同步读要提前一拍，所以加上 cmp_fire）。
        ekb_addr = ekpos[10:0];
        cbb_addr = (state == S_RE_RUN) ? (cbcnt[10:0] + {10'd0, cmp_fire})
                                       : cpos[10:0];

        bu_in_valid  = 1'b0;
        bu_in_data   = 8'd0;
        bu_out_ready = 1'b0;

        enc_start = 1'b0;

        case (state)
        // ---------- 吃 dk：ŝ 解码、ek 进海绵与缓冲、h/z 进寄存器 ----------
        S_HEK_START: begin
            sha_suffix = 8'h06;                // SHA3-256，rate 136
            sha_start  = 1'b1;
        end
        S_DK_IN: begin
            sha_suffix   = 8'h06;
            sha_in_valid = dk_valid && in_ek_rng;
            sha_in_data  = dk_data;

            if (dk_fire && in_s_rng && (grp == 2'd2)) begin
                ba_we   = 1'b1;
                ba_addr = {SL_S + tp, tn};
                ba_din  = dec_s0;
                bb_we   = 1'b1;
                bb_addr = {SL_S + tp, tn + 8'd1};
                bb_din  = dec_s1;
            end
            if (dk_fire && in_ek_rng) begin
                eka_we   = 1'b1;
                eka_addr = ekpos[10:0];
                eka_din  = dk_data;
            end
        end
        S_HEK_FLUSH: begin sha_suffix = 8'h06; sha_in_flush  = 1'b1; end
        S_HEK_SQ:    begin sha_suffix = 8'h06; sha_out_ready = 1'b1; end

        // ---------- J(z‖c) = SHAKE256(z‖c, 32)，无条件先算 ----------
        S_J_START: sha_start = 1'b1;
        S_J_Z: begin
            sha_in_valid = 1'b1;
            sha_in_data  = z_r[{ocnt[4:0], 3'd0} +: 8];
        end
        S_C_IN: begin
            sha_in_valid = c_valid;
            sha_in_data  = c_data;
            if (c_fire) begin
                cba_we   = 1'b1;
                cba_addr = cpos[10:0];
                cba_din  = c_data;
            end
        end
        S_J_FLUSH: sha_in_flush  = 1'b1;
        S_J_SQ:    sha_out_ready = 1'b1;

        // ---------- 累加器清零 ----------
        S_ACC_CLR: begin
            bb_we   = 1'b1;
            bb_addr = {SL_ACC, acnt[7:0]};
            bb_din  = 16'sd0;
        end

        // ---------- c₁ → u[i] → NTT ----------
        S_U_STEP: begin
            if (bu_out_valid) begin
                bu_out_ready = 1'b1;
                ntt_wr_en    = 1'b1;
                ntt_wr_addr  = ucnt[7:0];
                ntt_wr_data  = u_coeff;
            end else begin
                // 位解包器的 in_ready 与 out_valid 互补，所以这一支必然收得下
                bu_in_valid = 1'b1;
                bu_in_data  = cbb_dout;
            end
        end
        S_U_NTT: ntt_start = !ntt_kicked;

        // ---------- ŝ[i] ∘ û[i] 累加：a 取 bank、b 取 NTT 存储、acc 在 bank ----------
        S_MAC_A0: begin
            ba_addr     = {SL_S + idx_i, pair[6:0], 1'b0};
            ntt_rd_addr = {pair[6:0], 1'b0};
            bb_addr     = {SL_ACC, pair[6:0], 1'b0};
        end
        S_MAC_A1: begin
            ba_addr     = {SL_S + idx_i, pair[6:0], 1'b1};
            ntt_rd_addr = {pair[6:0], 1'b1};
            bb_addr     = {SL_ACC, pair[6:0], 1'b1};
        end
        S_MAC_W0: begin
            bb_we   = 1'b1;
            bb_addr = {SL_ACC, pair[6:0], 1'b0};
            bb_din  = acc0_next;
        end
        S_MAC_W1: begin
            bb_we   = 1'b1;
            bb_addr = {SL_ACC, pair[6:0], 1'b1};
            bb_din  = acc1_next;
        end

        // ---------- 累加结果搬进 NTT 核，做逆变换 ----------
        S_A2N_RD: bb_addr = {SL_ACC, 8'd0};
        S_A2N: begin
            bb_addr     = {SL_ACC, acnt[7:0] + 8'd1};   // 同步读，地址提前一拍
            ntt_wr_en   = 1'b1;
            ntt_wr_addr = acnt[7:0];
            ntt_wr_data = bb_dout;
        end
        S_INV: ntt_start = !ntt_kicked;

        // ---------- c₂ → v，w = v − su，m′ = Compress₁(w) ----------
        S_V_WAIT: ntt_rd_addr = vcnt[7:0];
        S_V_STEP: begin
            ntt_rd_addr = vcnt[7:0];
            if (bu_out_valid) bu_out_ready = 1'b1;
            else begin
                bu_in_valid = 1'b1;
                bu_in_data  = cbb_dout;
            end
        end
        S_V_W:   ntt_rd_addr = vcnt[7:0];
        S_V_BIT: ntt_rd_addr = vcnt[7:0];

        // ---------- 重加密 + 常量时间比对 ----------
        S_RE_START: enc_start = 1'b1;

        default: ;
        endcase
    end

    // ================= 时序 =================
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= S_IDLE; done <= 1'b0; dk_hash_ok <= 1'b0;
            ps_r <= 2'd1; k_r <= 3'd3; d11_r <= 1'b0;
            h_r <= 256'd0; z_r <= 256'd0; hek_r <= 256'd0;
            kbar_r <= 256'd0; kprime_r <= 256'd0; m_r <= 256'd0;
            dkcnt <= 13'd0; cpos <= 13'd0; cbcnt <= 13'd0; ekpos <= 13'd0;
            rbcnt <= 11'd0; ucnt <= 9'd0; vcnt <= 9'd0; acnt <= 9'd0;
            ocnt <= 6'd0; idx_i <= 3'd0; tp <= 3'd0; tn <= 8'd0;
            grp <= 2'd0; grp_buf <= 16'd0; pair <= 8'd0;
            ntt_kicked <= 1'b0; ntt_inverse <= 1'b0;
            v_phase <= 1'b0; cmp_diff <= 1'b0;
            ekb_r <= 8'd0; ekb_v <= 1'b0; ekb_wait <= 1'b0;
            a0_r <= 16'sd0; a1_r <= 16'sd0; b0_r <= 16'sd0; b1_r <= 16'sd0;
            acc0_r <= 16'sd0; acc1_r <= 16'sd0;
            t_ab_r <= 16'sd0; bm0_r <= 16'sd0; bm1_r <= 16'sd0;
            v_r <= 16'sd0; su_r <= 16'sd0; w_r <= 16'sd0;
        end else if (start) begin
            state <= S_HEK_START; done <= 1'b0; dk_hash_ok <= 1'b0;
            ps_r <= param_set; k_r <= k_now; d11_r <= d11_now;
            dkcnt <= 13'd0; cpos <= 13'd0; cbcnt <= 13'd0; ekpos <= 13'd0;
            rbcnt <= 11'd0; ucnt <= 9'd0; vcnt <= 9'd0; acnt <= 9'd0;
            ocnt <= 6'd0; idx_i <= 3'd0; tp <= 3'd0; tn <= 8'd0;
            grp <= 2'd0; grp_buf <= 16'd0; pair <= 8'd0;
            ntt_kicked <= 1'b0; ntt_inverse <= 1'b0;
            v_phase <= 1'b0; cmp_diff <= 1'b0;
            ekb_v <= 1'b0; ekb_wait <= 1'b0;
        end else begin
            case (state)
            S_IDLE: ;

            S_HEK_START: state <= S_DK_IN;

            S_DK_IN: if (dk_fire) begin
                dkcnt <= dkcnt + 13'd1;
                if (in_s_rng) begin
                    if (grp == 2'd2) begin
                        grp <= 2'd0;
                        if (tn == 8'd254) begin tn <= 8'd0; tp <= tp + 3'd1; end
                        else                   tn <= tn + 8'd2;
                    end else begin
                        grp_buf <= {dk_data, grp_buf[15:8]};
                        grp     <= grp + 2'd1;
                    end
                end else if (in_ek_rng) begin
                    ekpos <= ekpos + 13'd1;
                end else if (in_h_rng) begin
                    h_r <= {dk_data, h_r[255:8]};
                end else begin
                    z_r <= {dk_data, z_r[255:8]};
                end
                if (dkcnt + 13'd1 == dklen) begin
                    ocnt <= 6'd0;
                    state <= S_HEK_FLUSH;
                end
            end

            S_HEK_FLUSH: state <= S_HEK_SQ;
            S_HEK_SQ: if (sha_out_valid) begin
                hek_r <= {sha_out_data, hek_r[255:8]};
                ocnt  <= ocnt + 6'd1;
                if (ocnt == 6'd31) begin ocnt <= 6'd0; state <= S_J_START; end
            end

            // ---------- J(z‖c) ----------
            S_J_START: begin
                // dk 自洽性检查（FIPS 203 §7.3）。只是报出来，不改数据通路 ——
                // 它检查的是本方私钥，与密文无关，因此不涉及常量时间。
                dk_hash_ok <= (hek_r == h_r);
                ocnt  <= 6'd0;
                state <= S_J_Z;
            end
            S_J_Z: if (sha_in_ready) begin
                ocnt <= ocnt + 6'd1;
                if (ocnt == 6'd31) begin cpos <= 13'd0; state <= S_C_IN; end
            end
            S_C_IN: if (c_fire) begin
                cpos <= cpos + 13'd1;
                if (cpos + 13'd1 == clen) state <= S_J_FLUSH;
            end
            S_J_FLUSH: begin ocnt <= 6'd0; state <= S_J_SQ; end
            S_J_SQ: if (sha_out_valid) begin
                kbar_r <= {sha_out_data, kbar_r[255:8]};
                ocnt   <= ocnt + 6'd1;
                if (ocnt == 6'd31) begin
                    acnt  <= 9'd0;
                    state <= S_ACC_CLR;
                end
            end

            // ---------- 解密主循环 ----------
            S_ACC_CLR: begin
                acnt <= acnt + 9'd1;
                if (acnt == 9'd255) begin
                    acnt <= 9'd0; cpos <= 13'd0; ucnt <= 9'd0; idx_i <= 3'd0;
                    v_phase <= 1'b0;
                    state <= S_U_WAIT;
                end
            end

            S_U_WAIT: state <= S_U_STEP;
            S_U_STEP: begin
                if (bu_out_valid) begin
                    ucnt <= ucnt + 9'd1;
                    if (ucnt == 9'd255) begin
                        ucnt <= 9'd0;
                        ntt_kicked  <= 1'b0;
                        ntt_inverse <= 1'b0;
                        state <= S_U_NTT;
                    end
                end else begin
                    cpos  <= cpos + 13'd1;
                    state <= S_U_WAIT;
                end
            end
            S_U_NTT: begin
                if (ntt_start) ntt_kicked <= 1'b1;
                if (ntt_done && ntt_kicked) begin
                    pair  <= 8'd0;
                    state <= S_MAC_A0;
                end
            end

            S_MAC_A0: state <= S_MAC_A1;
            S_MAC_A1: begin
                a0_r   <= ba_dout;        // ŝ[i]
                b0_r   <= ntt_rd_data;    // û[i]
                acc0_r <= bb_dout;
                state  <= S_MAC_C;
            end
            S_MAC_C: begin
                a1_r   <= ba_dout;
                b1_r   <= ntt_rd_data;
                acc1_r <= bb_dout;
                state  <= S_MAC_M;
            end
            S_MAC_M: begin t_ab_r <= t_ab;  state <= S_MAC_T; end
            S_MAC_T: begin bm0_r <= bm_r0; bm1_r <= bm_r1; state <= S_MAC_W0; end
            S_MAC_W0: state <= S_MAC_W1;
            S_MAC_W1: begin
                if (pair == 8'd127) begin
                    if (idx_i + 3'd1 == k_r) begin
                        acnt  <= 9'd0;
                        state <= S_A2N_RD;
                    end else begin
                        idx_i <= idx_i + 3'd1;
                        ucnt  <= 9'd0;
                        state <= S_U_WAIT;
                    end
                end else begin
                    pair  <= pair + 8'd1;
                    state <= S_MAC_A0;
                end
            end

            S_A2N_RD: begin acnt <= 9'd0; state <= S_A2N; end
            S_A2N: begin
                acnt <= acnt + 9'd1;
                if (acnt == 9'd255) begin
                    acnt <= 9'd0;
                    ntt_kicked  <= 1'b0;
                    ntt_inverse <= 1'b1;
                    state <= S_INV;
                end
            end
            S_INV: begin
                if (ntt_start) ntt_kicked <= 1'b1;
                if (ntt_done && ntt_kicked) begin
                    vcnt    <= 9'd0;
                    v_phase <= 1'b1;
                    state   <= S_V_WAIT;
                end
            end

            S_V_WAIT: state <= S_V_STEP;
            S_V_STEP: begin
                if (bu_out_valid) begin
                    v_r   <= v_coeff;
                    su_r  <= ntt_rd_data;
                    state <= S_V_W;
                end else begin
                    cpos  <= cpos + 13'd1;
                    state <= S_V_WAIT;
                end
            end
            S_V_W: begin w_r <= w_bar; state <= S_V_BIT; end
            S_V_BIT: begin
                m_r[vcnt[7:0]] <= mbit;
                vcnt <= vcnt + 9'd1;
                if (vcnt == 9'd255) state <= S_RE_START;
                else                state <= S_V_WAIT;
            end

            // ---------- 重加密 + 常量时间比对 ----------
            S_RE_START: begin
                ekpos <= 13'd0; cbcnt <= 13'd0; rbcnt <= 11'd0;
                cmp_diff <= 1'b0; ekb_v <= 1'b0;
                // ekb_wait 起手就是 1：ekpos 这一拍才被清零，ek 缓冲的同步读
                // 要到下下拍才吐出 mem[0]。少了这一拍就会把上一次留在读口上的
                // 字节（地址还停在 eklen）当成 ek 的第一个字节。
                ekb_wait <= 1'b1;
                state <= S_RE_RUN;
            end
            S_RE_RUN: begin
                // ---- 把 ek 一个字节一个字节回放给封装核 ----
                // 同步读要等一拍，所以 fire 之后先过一个 ekb_wait 再取新值。
                if (ekb_v && enc_ek_ready) begin
                    ekb_v    <= 1'b0;
                    ekpos    <= ekpos + 13'd1;
                    ekb_wait <= 1'b1;
                end else if (ekb_wait) begin
                    ekb_wait <= 1'b0;
                end else if (!ekb_v && (ekpos < eklen)) begin
                    ekb_r <= ekb_dout;
                    ekb_v <= 1'b1;
                end

                // ---- 收封装核吐出来的 K′‖c′ ----
                // ⚠️ 这里没有、也不能有任何 early-out：无论比到第几个字节
                //    发现不同，都要老老实实比完 clen 个。
                if (enc_out_valid) begin
                    rbcnt <= rbcnt + 11'd1;
                    if (rbcnt < 11'd32) begin
                        kprime_r <= {enc_out_data, kprime_r[255:8]};
                    end else begin
                        cmp_diff <= cmp_diff | (|(enc_out_data ^ cbb_dout));
                        cbcnt    <= cbcnt + 13'd1;
                    end
                    if ({2'd0, rbcnt} + 13'd1 == clen + 13'd32) begin
                        ocnt  <= 6'd0;
                        state <= S_OUT;
                    end
                end
            end

            S_OUT: if (out_fire) begin
                ocnt <= ocnt + 6'd1;
                if (ocnt == 6'd31) state <= S_DONE;
            end

            S_DONE: begin done <= 1'b1; state <= S_IDLE; end
            default: state <= S_IDLE;
            endcase
        end
    end

    // ================= 仿真观察口 =================
    generate
        if (DEBUG_BANK != 0) begin : g_dbg
            assign dbg_data = u_bank.mem[dbg_addr];
        end else begin : g_nodbg
            wire _unused = &{1'b0, dbg_addr};
            assign dbg_data = 16'sd0;
        end
    endgenerate

endmodule

`default_nettype wire
