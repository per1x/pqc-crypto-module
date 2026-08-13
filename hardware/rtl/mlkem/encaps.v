// mlkem_encaps —— 完整的 ML-KEM 封装核（FIPS 203 Alg 17 + Alg 14）
//
//     ek 字节流 ┐
//               ├─ H(ek) ─┐
//               └─ ByteDecode12 → t̂[0..k-1]、ρ
//                              │
//     m (32 B) ────────────────┴→ G(m‖H(ek)) → K ‖ r
//                                              │   │
//                                    K 直接输出 ┘   ├→ PRF → CBD_η1 → NTT → r̂[i]
//                                                  ├→ PRF → CBD_η2 → e₁[i]
//                                                  └→ PRF → CBD_η2 → e₂
//
//     u[i] = NTT⁻¹(Σⱼ Âᵀ[i][j] ∘ r̂[j]) + e₁[i]  ──→ Compress_du ──→ c₁
//     v    = NTT⁻¹(Σᵢ t̂[i]    ∘ r̂[i]) + e₂ + μ  ──→ Compress_dv ──→ c₂
//
// 输出顺序是 **K‖c**：K 在 G 之后就定了，先发出去，下游不必等整条密文。
//
// 【和 KeyGen 的三个关键差别，每一个都能让核"跑得通但不是 ML-KEM"】
//
// ① **矩阵是转置的**。Encrypt 用 Âᵀ，落到采样端就是 XOF 头写成 ρ‖i‖j，
//    而 KeyGen 是 ρ‖j‖i。两种写法都能算出合法密文，只有一个对。
//    黄金模型那边有一条专门的反证（"Encrypt 的矩阵忘了转置"）盯着这件事。
//
// ② **累加器直接放在 ntt_core 的系数存储里**，不再单独占一个存储槽。
//    基乘要同时读 Â（拒绝采样缓冲）、r̂（多项式存储）、累加值三个来源，
//    而多项式存储只有两个口。把累加值放进 ntt_core 正好凑齐三个独立口，
//    还省掉了"算完再拷进 NTT"的一趟搬运。
//
// ③ **算 v 的时候 Â 换成 t̂**，也就是"a 操作数"从拒绝采样缓冲改成多项式
//    存储的 A 口。除此之外 u 和 v 走的是同一段基乘累加逻辑（mac_v 选源），
//    这样两条路的 Montgomery 因子不会各写一遍、各错一遍。
//
// 基乘沿用 basemul.v 的 _head/_tail 两半并在中间插一级寄存器 ——
// 理由与代价见 mlkem_keygen 的注释和 docs/fpga-进展.md 的 S4 一节。
`default_nettype none

module mlkem_encaps #(
    // ⚠️ 仿真专用：接出多项式存储的读口。综合时必须是 0。
    parameter integer DEBUG_BANK = 0
) (
    input  wire        clk,
    input  wire        rst_n,

    input  wire [1:0]  param_set,    // 0=512 1=768 2=1024
    input  wire [255:0] m_in,        // Encaps_internal 的 m

    input  wire        start,        // 脉冲
    output reg         done,         // 电平，保持到下一次 start

    // ---- ek 输入字节流（384k + 32 字节）----
    input  wire        ek_valid,
    output wire        ek_ready,
    input  wire [7:0]  ek_data,

    // ---- 输出字节流：K（32 字节）紧接着 c ----
    output wire        out_valid,
    input  wire        out_ready,
    output wire [7:0]  out_data,
    output wire        out_last,     // c 的最后一个字节

    input  wire [11:0] dbg_addr,
    output wire signed [15:0] dbg_data
);
    // ================= 参数集 =================
    wire [2:0] k_now      = (param_set == 2'd0) ? 3'd2 :
                            (param_set == 2'd1) ? 3'd3 : 3'd4;
    wire       eta1_3_now = (param_set == 2'd0);          // 512 的 η1 = 3
    wire       d11_now    = (param_set == 2'd2);          // 1024 用 du=11 dv=5

    reg [2:0] k_r;
    reg       eta1_3_r, d11_r;

    wire [3:0] du = d11_r ? 4'd11 : 4'd10;
    wire [3:0] dv = d11_r ? 4'd5  : 4'd4;

    // ek 里 t̂ 那一段的字节数 = 384k。384 = 256 + 128，所以是两项移位和 ——
    // 不是四项（128+64+32+16 = 240，那是另一个数）。
    wire [11:0] tbytes = {1'b0, k_r, 8'd0} + {2'b0, k_r, 7'd0};

    // ================= 多项式存储 =================
    //   槽 0..3 : r̂[0..k-1]
    //   槽 4..7 : e₁[0..k-1]（时域，不做 NTT）
    //   槽 8..11: t̂[0..k-1]（从 ek 解出来的）
    //   槽 12   : e₂
    localparam [3:0] SL_RHAT = 4'd0;
    localparam [3:0] SL_E1   = 4'd4;
    localparam [3:0] SL_THAT = 4'd8;
    localparam [3:0] SL_E2   = 4'd12;

    reg         ba_we, bb_we;
    reg  [11:0] ba_addr, bb_addr;
    reg  signed [15:0] ba_din, bb_din;
    wire signed [15:0] ba_dout, bb_dout;

    ram_dp #(.DW(16), .AW(12)) u_bank (
        .clk(clk),
        .a_we(ba_we), .a_addr(ba_addr), .a_din(ba_din), .a_dout(ba_dout),
        .b_we(bb_we), .b_addr(bb_addr), .b_din(bb_din), .b_dout(bb_dout));

    // ================= 状态机 =================
    localparam [5:0]
        S_IDLE      = 6'd0,
        S_H_START   = 6'd1,
        S_EK_IN     = 6'd2,
        S_EK_FLUSH  = 6'd3,
        S_EK_SQ     = 6'd4,
        S_G_START   = 6'd5,
        S_G_ABS     = 6'd6,
        S_G_FLUSH   = 6'd7,
        S_G_SQ      = 6'd8,
        S_K_OUT     = 6'd9,
        S_R_START   = 6'd10,
        S_R_ABS     = 6'd11,
        S_R_FLUSH   = 6'd12,
        S_R_RUN     = 6'd13,
        S_R_NTT_RUN = 6'd14,
        S_R_NTT_ST  = 6'd15,
        S_ACC_CLR   = 6'd16,
        S_A_START   = 6'd17,
        S_A_ABS     = 6'd18,
        S_A_FLUSH   = 6'd19,
        S_A_RUN     = 6'd20,
        S_MAC_A0    = 6'd21,
        S_MAC_A1    = 6'd22,
        S_MAC_C     = 6'd23,
        S_MAC_M     = 6'd24,
        S_MAC_T     = 6'd25,
        S_MAC_W0    = 6'd26,
        S_MAC_W1    = 6'd27,
        S_INV_RUN   = 6'd28,
        S_OUT_RD    = 6'd29,
        S_OUT_CMP   = 6'd30,
        S_OUT_FEED  = 6'd31,
        S_OUT_DRAIN = 6'd32,
        S_DONE      = 6'd33;

    reg [5:0] state;

    // ================= 子模块 =================
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

    reg         rej_start, rej_in_valid;
    reg  [23:0] rej_in_bytes;
    reg  [7:0]  rej_rd_addr;
    wire        rej_done, rej_in_ready;
    wire [15:0] rej_rd_data;

    mlkem_rej_uniform u_rej (
        .clk(clk), .rst_n(rst_n),
        .start(rej_start), .done(rej_done),
        .in_valid(rej_in_valid), .in_bytes(rej_in_bytes), .in_ready(rej_in_ready),
        .count(), .rd_addr(rej_rd_addr), .rd_data(rej_rd_data));

    reg         cbd_start, cbd_out_ready;
    wire        cbd_in_ready, cbd_out_valid;
    wire signed [15:0] cbd_out_coeff;

    // r 用 η1（512 是 3），e₁/e₂ 一律 η2 = 2
    // ⚠️ 四位不是三位：k=4 时 se_n 要数到 2k = 8，三位装不下会绕回 0。
    reg  [3:0]  se_n;                  // 0..k-1 是 r，k..2k-1 是 e₁，2k 是 e₂
    wire        se_is_r  = (se_n < {1'b0, k_r});
    wire        cbd_eta3 = se_is_r && eta1_3_r;

    wire cbd_fb_valid = (state == S_R_RUN) && sha_out_valid;

    mlkem_cbd_stream u_cbd (
        .clk(clk), .rst_n(rst_n),
        // .done() 与 .count() 都有意留空：采样推进看的是 out_valid 的拍数
        .eta3(cbd_eta3), .start(cbd_start), .done(),
        .in_valid(cbd_fb_valid), .in_ready(cbd_in_ready), .in_data(sha_out_data),
        .out_valid(cbd_out_valid), .out_ready(cbd_out_ready),
        .out_coeff(cbd_out_coeff), .count());

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

    // ---- 密文打包：d 运行时可变 ----
    reg  [3:0] bp_d;
    reg        bp_in_valid;
    reg  [11:0] bp_in_data;
    wire       bp_in_ready, bp_out_valid;
    wire [7:0] bp_out_data;

    // 打包器只在输出状态里排空 —— 否则别的状态下 out_ready 恰好为高时
    // 会把字节吐掉而没人接（out_valid 那时是低的）。
    wire out_phase = (state == S_OUT_RD) || (state == S_OUT_CMP)
                     || (state == S_OUT_FEED) || (state == S_OUT_DRAIN);

    mlkem_bitpack u_bp (
        .clk(clk), .rst_n(rst_n), .d(bp_d),
        .in_valid(bp_in_valid), .in_ready(bp_in_ready), .in_data(bp_in_data),
        .out_valid(bp_out_valid), .out_ready(out_ready && out_phase),
        .out_data(bp_out_data));

    // ================= 状态寄存器 =================
    reg [255:0] rho_r, h_r, rand_r, shared_r, m_r;
    reg [2:0]   idx_i, idx_j;
    reg [8:0]   cnt;
    reg [11:0]  ekcnt;             // ek 的字节计数（最多 1568）
    reg [1:0]   grp;               // 3 字节一组的组内计数
    reg [15:0]  grp_buf;           // 组里已经收到的前两个字节
    reg [2:0]   tp;                // 正在解的是 t̂ 的第几个多项式
    reg [7:0]   tn;                // t̂ 的系数下标（成对推进）
    reg [5:0]   hdr_cnt;
    reg [7:0]   pair;
    reg         ntt_kicked;
    reg         mac_v;             // 0 = 算 u（a 取自拒绝采样），1 = 算 v（a 取自 t̂）
    reg [2:0]   out_poly;          // 正在输出 u 的第几个多项式
    reg         out_is_v;
    reg [7:0]   obcnt;             // 当前这一段密文已经发出去的字节数

    reg signed [15:0] a0_r, a1_r, b0_r, b1_r, acc0_r, acc1_r;
    reg signed [15:0] t_ab_r, bm0_r, bm1_r;

    // ---- 槽地址 ----
    wire [3:0] slot_se   = se_is_r ? (SL_RHAT + se_n)
                         : (se_n == {k_r, 1'b0}) ? SL_E2
                                                 : (SL_E1 + (se_n - {1'b0, k_r}));
    wire [2:0] mac_idx   = mac_v ? idx_i : idx_j;   // 基乘循环的内层变量
    wire [3:0] slot_rhat = SL_RHAT + {1'b0, mac_idx};
    wire [3:0] slot_that = SL_THAT + {1'b0, idx_i};
    wire [3:0] slot_err  = out_is_v ? SL_E2 : (SL_E1 + {1'b0, out_poly});

    // ================= 基乘（两级流水，见 basemul.v）=================
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

    // ================= 输出那一步：加误差项、（v 还要加 μ）、压缩 =================
    // μ = Decompress_1(m 的第 cnt 位)。用 mlkem_decompress 而不是写死 1665，
    // 免得"解压"这段数学在仓库里出现第二份实现。
    wire signed [15:0] mu;
    mlkem_decompress #(.D(1)) u_mu (.val(m_r[cnt[7:0]]), .coeff(mu));

    wire signed [15:0] err_term = ba_dout + (out_is_v ? mu : 16'sd0);
    wire signed [15:0] outc;
    barrett_reduce u_out_br (.a(ntt_rd_data + err_term), .r(outc));

    // ⚠️ 这一级寄存器是**时序需要**，不是随手加的。
    // 没有它的时候整条输出尾巴挤在一拍里：
    //   BRAM 读出 → barrett（两级 DSP）→ compress（一级 DSP）→ 打包器累加器，
    // 布线后 WNS = −1.987 ns（83.4 MHz）。切在 barrett 与 compress 之间，
    // 前半是"读存储 + barrett"、后半是"compress + 打包"，两边各剩一半。
    // 代价是每个系数从 2 拍变 3 拍（S_OUT_RD → S_OUT_CMP → S_OUT_FEED）。
    reg signed [15:0] outc_r;

    // du ∈ {10, 11}、dv ∈ {4, 5} 各要一份：压缩的整数式里 d 在移位量上，
    // 不能由别的 d 推出来。四个都很小（一次 24×22 乘法）。
    wire [10:0] cmp11; wire [9:0] cmp10; wire [4:0] cmp5; wire [3:0] cmp4;
    mlkem_compress #(.D(11)) u_c11 (.coeff(outc_r), .val(cmp11));
    mlkem_compress #(.D(10)) u_c10 (.coeff(outc_r), .val(cmp10));
    mlkem_compress #(.D(5))  u_c5  (.coeff(outc_r), .val(cmp5));
    mlkem_compress #(.D(4))  u_c4  (.coeff(outc_r), .val(cmp4));

    wire [11:0] cmp_out = out_is_v ? (d11_r ? {7'd0, cmp5} : {8'd0, cmp4})
                                   : (d11_r ? {1'd0, cmp11} : {2'd0, cmp10});

    // ================= ek 里的 12 位解码 =================
    wire [11:0] dec_c0, dec_c1;
    mlkem_decode12 u_dec (.bytes_in({ek_data, grp_buf}), .c0(dec_c0), .c1(dec_c1));

    // t̂ 存回来时要用有符号代表元（数据通路里系数是 (−q, q)）——
    // ByteDecode12 给的是 [0, 2^12)，超过 q 的按 FIPS 203 属于非法公钥，
    // 这里不做检查（合法性检查是上层的事），照原值存。
    wire signed [15:0] dec_s0 = {4'd0, dec_c0};
    wire signed [15:0] dec_s1 = {4'd0, dec_c1};

    // ================= 输出总线 =================
    wire k_out_phase = (state == S_K_OUT);
    assign out_valid = k_out_phase || (out_phase && bp_out_valid);
    assign out_data  = k_out_phase ? shared_r[{cnt[4:0], 3'd0} +: 8] : bp_out_data;
    // c₂ 是 32·dv 字节（dv ∈ {4,5} → 128 或 160），最后一个就是整条密文的末字节
    assign out_last  = out_is_v && out_phase && bp_out_valid
                       && (obcnt + 8'd1 == {dv, 5'd0});

    assign ek_ready = (state == S_EK_IN) && sha_in_ready;

    // ================= 组合：控制线 =================
    always @(*) begin
        sha_rate      = 8'd136;
        sha_suffix    = 8'h1F;
        sha_start     = 1'b0;
        sha_in_valid  = 1'b0;
        sha_in_data   = 8'd0;
        sha_in_flush  = 1'b0;
        sha_out_ready = 1'b0;

        rej_start     = 1'b0;
        rej_in_valid  = 1'b0;
        rej_in_bytes  = 24'd0;
        rej_rd_addr   = 8'd0;

        cbd_start     = 1'b0;
        cbd_out_ready = 1'b0;

        ntt_start     = 1'b0;
        ntt_wr_en     = 1'b0;
        ntt_wr_addr   = 8'd0;
        ntt_wr_data   = 16'sd0;
        ntt_rd_addr   = 8'd0;

        ba_we = 1'b0; ba_addr = 12'd0; ba_din = 16'sd0;
        bb_we = 1'b0; bb_addr = 12'd0; bb_din = 16'sd0;

        bp_d        = 4'd10;
        bp_in_valid = 1'b0;
        bp_in_data  = 12'd0;

        case (state)
        // ---------- 吃 ek：一路进 H，一路解成 t̂ 和 ρ ----------
        S_H_START: begin
            sha_suffix = 8'h06;               // SHA3-256，rate 136
            sha_start  = 1'b1;
        end
        S_EK_IN: begin
            sha_in_valid = ek_valid;
            sha_in_data  = ek_data;
            // 每收满 3 字节就落两个系数
            if (ek_valid && sha_in_ready && (ekcnt < tbytes) && (grp == 2'd2)) begin
                ba_we   = 1'b1;
                ba_addr = {SL_THAT + {1'b0, tp}, tn};
                ba_din  = dec_s0;
                bb_we   = 1'b1;
                bb_addr = {SL_THAT + {1'b0, tp}, tn + 8'd1};
                bb_din  = dec_s1;
            end
        end
        S_EK_FLUSH: sha_in_flush = 1'b1;
        S_EK_SQ:    sha_out_ready = 1'b1;

        // ---------- G(m ‖ H(ek)) ----------
        S_G_START: begin
            sha_rate   = 8'd72;               // SHA3-512
            sha_suffix = 8'h06;
            sha_start  = 1'b1;
        end
        S_G_ABS: begin
            sha_rate     = 8'd72;
            sha_suffix   = 8'h06;
            sha_in_valid = 1'b1;
            sha_in_data  = (cnt < 9'd32) ? m_r[{cnt[4:0], 3'd0} +: 8]
                                         : h_r[{cnt[4:0], 3'd0} +: 8];
        end
        S_G_FLUSH: begin
            sha_rate   = 8'd72;
            sha_suffix = 8'h06;
            sha_in_flush = 1'b1;
        end
        S_G_SQ: begin
            sha_rate   = 8'd72;
            sha_suffix = 8'h06;
            sha_out_ready = 1'b1;
        end

        // ---------- PRF(r, N)：SHAKE256 ----------
        S_R_START: begin
            sha_rate  = 8'd136;
            sha_start = 1'b1;
        end
        S_R_ABS: begin
            sha_in_valid = 1'b1;
            sha_in_data  = (hdr_cnt < 6'd32) ? rand_r[{hdr_cnt[4:0], 3'd0} +: 8]
                                             : {4'd0, se_n};
        end
        S_R_FLUSH: begin
            sha_in_flush = 1'b1;
            cbd_start    = 1'b1;
        end
        S_R_RUN: begin
            sha_out_ready = cbd_in_ready;
            cbd_out_ready = 1'b1;
            if (cbd_out_valid) begin
                if (se_is_r) begin
                    ntt_wr_en   = 1'b1;       // r 直接落进 NTT 核，省一趟搬运
                    ntt_wr_addr = cnt[7:0];
                    ntt_wr_data = cbd_out_coeff;
                end else begin
                    ba_we   = 1'b1;
                    ba_addr = {slot_se, cnt[7:0]};
                    ba_din  = cbd_out_coeff;
                end
            end
        end
        S_R_NTT_RUN: ntt_start = !ntt_kicked;
        S_R_NTT_ST: begin
            ntt_rd_addr = cnt[7:0] + 8'd1;    // 同步读，地址要提前一拍
            ba_we   = 1'b1;
            ba_addr = {slot_se, cnt[7:0]};
            ba_din  = ntt_rd_data;
        end

        // ---------- 累加器清零（累加器就是 NTT 核的系数存储）----------
        S_ACC_CLR: begin
            ntt_wr_en   = 1'b1;
            ntt_wr_addr = cnt[7:0];
            ntt_wr_data = 16'sd0;
        end

        // ---------- Âᵀ[i][j] 的 XOF：头是 ρ‖i‖j ----------
        S_A_START: begin
            sha_rate  = 8'd168;               // SHAKE128
            sha_start = 1'b1;
        end
        S_A_ABS: begin
            sha_rate     = 8'd168;
            sha_in_valid = 1'b1;
            sha_in_data  = (hdr_cnt < 6'd32) ? rho_r[{hdr_cnt[4:0], 3'd0} +: 8]
                         : (hdr_cnt == 6'd32) ? {5'd0, idx_i}   // ⚠️ i 在前
                                              : {5'd0, idx_j};
        end
        S_A_FLUSH: begin
            sha_rate     = 8'd168;
            sha_in_flush = 1'b1;
            rej_start    = 1'b1;
        end
        S_A_RUN: begin
            sha_rate      = 8'd168;
            sha_out_ready = !rej_done && (grp != 2'd2 || rej_in_ready);
            rej_in_valid  = (grp == 2'd2) && sha_out_valid && !rej_done;
            rej_in_bytes  = {sha_out_data, grp_buf};
        end

        // ---------- 基乘累加：u 的 a 取自拒绝采样，v 的 a 取自 t̂ ----------
        S_MAC_A0: begin
            rej_rd_addr = {pair[6:0], 1'b0};
            ba_addr     = {slot_that, pair[6:0], 1'b0};
            bb_addr     = {slot_rhat, pair[6:0], 1'b0};
            ntt_rd_addr = {pair[6:0], 1'b0};
        end
        S_MAC_A1: begin
            rej_rd_addr = {pair[6:0], 1'b1};
            ba_addr     = {slot_that, pair[6:0], 1'b1};
            bb_addr     = {slot_rhat, pair[6:0], 1'b1};
            ntt_rd_addr = {pair[6:0], 1'b1};
        end
        S_MAC_W0: begin
            ntt_wr_en   = 1'b1;
            ntt_wr_addr = {pair[6:0], 1'b0};
            ntt_wr_data = acc0_next;
        end
        S_MAC_W1: begin
            ntt_wr_en   = 1'b1;
            ntt_wr_addr = {pair[6:0], 1'b1};
            ntt_wr_data = acc1_next;
        end

        S_INV_RUN: ntt_start = !ntt_kicked;

        // ---------- 输出：加误差项、压缩、打包 ----------
        S_OUT_RD: begin
            bp_d        = out_is_v ? dv : du;
            ntt_rd_addr = cnt[7:0];
            ba_addr     = {slot_err, cnt[7:0]};
        end
        // 存储的数据这一拍到齐，走完 barrett 存进 outc_r。地址要继续驱动，
        // 否则同步存储的输出在这一拍就不是这个系数了。
        S_OUT_CMP: begin
            bp_d        = out_is_v ? dv : du;
            ntt_rd_addr = cnt[7:0];
            ba_addr     = {slot_err, cnt[7:0]};
        end
        S_OUT_FEED: begin
            bp_d        = out_is_v ? dv : du;
            bp_in_valid = bp_in_ready;
            bp_in_data  = cmp_out;
        end
        S_OUT_DRAIN: bp_d = out_is_v ? dv : du;

        default: ;
        endcase
    end

    // ================= 时序 =================
    wire ek_fire  = ek_valid && ek_ready;
    wire out_fire = out_valid && out_ready;
    wire bp_fire  = bp_out_valid && out_ready && out_phase;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= S_IDLE; done <= 1'b0;
            k_r <= 3'd3; eta1_3_r <= 1'b0; d11_r <= 1'b0;
            rho_r <= 256'd0; h_r <= 256'd0; rand_r <= 256'd0;
            shared_r <= 256'd0; m_r <= 256'd0;
            idx_i <= 3'd0; idx_j <= 3'd0; se_n <= 4'd0;
            cnt <= 9'd0; ekcnt <= 12'd0; grp <= 2'd0; grp_buf <= 16'd0;
            tp <= 3'd0; tn <= 8'd0; hdr_cnt <= 6'd0; pair <= 8'd0;
            ntt_kicked <= 1'b0; ntt_inverse <= 1'b0;
            mac_v <= 1'b0; out_poly <= 3'd0; out_is_v <= 1'b0; obcnt <= 8'd0;
            outc_r <= 16'sd0;
            a0_r <= 16'sd0; a1_r <= 16'sd0; b0_r <= 16'sd0; b1_r <= 16'sd0;
            acc0_r <= 16'sd0; acc1_r <= 16'sd0;
            t_ab_r <= 16'sd0; bm0_r <= 16'sd0; bm1_r <= 16'sd0;
        end else if (start) begin
            state <= S_H_START; done <= 1'b0;
            k_r <= k_now; eta1_3_r <= eta1_3_now; d11_r <= d11_now;
            m_r <= m_in;
            cnt <= 9'd0; ekcnt <= 12'd0; grp <= 2'd0; grp_buf <= 16'd0;
            tp <= 3'd0; tn <= 8'd0; hdr_cnt <= 6'd0;
            idx_i <= 3'd0; idx_j <= 3'd0; se_n <= 4'd0;
            ntt_inverse <= 1'b0; mac_v <= 1'b0; out_is_v <= 1'b0; out_poly <= 3'd0;
            obcnt <= 8'd0;
        end else begin
            case (state)
            S_IDLE: ;

            S_H_START: state <= S_EK_IN;

            S_EK_IN: begin
                if (ek_fire) begin
                    ekcnt <= ekcnt + 12'd1;
                    if (ekcnt + 12'd1 == tbytes + 12'd32) begin
                        rho_r <= {ek_data, rho_r[255:8]};
                        cnt   <= 9'd0;
                        state <= S_EK_FLUSH;
                    end else if (ekcnt >= tbytes) begin
                        rho_r <= {ek_data, rho_r[255:8]};
                    end else begin
                        if (grp == 2'd2) begin
                            grp <= 2'd0;
                            if (tn == 8'd254) begin
                                tn <= 8'd0;
                                tp <= tp + 3'd1;
                            end else begin
                                tn <= tn + 8'd2;
                            end
                        end else begin
                            grp_buf <= {ek_data, grp_buf[15:8]};
                            grp <= grp + 2'd1;
                        end
                    end
                end
            end

            S_EK_FLUSH: state <= S_EK_SQ;
            S_EK_SQ: if (sha_out_valid) begin
                h_r <= {sha_out_data, h_r[255:8]};
                cnt <= cnt + 9'd1;
                if (cnt == 9'd31) begin cnt <= 9'd0; state <= S_G_START; end
            end

            S_G_START: begin cnt <= 9'd0; state <= S_G_ABS; end
            S_G_ABS: if (sha_in_ready) begin
                cnt <= cnt + 9'd1;
                if (cnt == 9'd63) begin cnt <= 9'd0; state <= S_G_FLUSH; end
            end
            S_G_FLUSH: state <= S_G_SQ;
            S_G_SQ: if (sha_out_valid) begin
                if (cnt < 9'd32) shared_r <= {sha_out_data, shared_r[255:8]};
                else             rand_r   <= {sha_out_data, rand_r[255:8]};
                cnt <= cnt + 9'd1;
                if (cnt == 9'd63) begin cnt <= 9'd0; state <= S_K_OUT; end
            end

            // K 先发出去：它在 G 之后就定了，没必要等整条密文
            S_K_OUT: if (out_fire) begin
                cnt <= cnt + 9'd1;
                if (cnt == 9'd31) begin
                    cnt <= 9'd0; se_n <= 4'd0; hdr_cnt <= 6'd0;
                    state <= S_R_START;
                end
            end

            // ---------- r / e₁ / e₂ 的采样 ----------
            S_R_START: begin hdr_cnt <= 6'd0; state <= S_R_ABS; end
            S_R_ABS: if (sha_in_ready) begin
                hdr_cnt <= hdr_cnt + 6'd1;
                if (hdr_cnt == 6'd32) begin cnt <= 9'd0; state <= S_R_FLUSH; end
            end
            S_R_FLUSH: state <= S_R_RUN;
            S_R_RUN: if (cbd_out_valid) begin
                cnt <= cnt + 9'd1;
                if (cnt == 9'd255) begin
                    cnt <= 9'd0;
                    ntt_kicked <= 1'b0;
                    ntt_inverse <= 1'b0;
                    state <= se_is_r ? S_R_NTT_RUN : S_R_START;
                    if (!se_is_r) begin
                        if (se_n == {k_r, 1'b0}) begin
                            // e₂ 采完了 → 进入算 u 的主循环
                            idx_i <= 3'd0; idx_j <= 3'd0;
                            mac_v <= 1'b0; ntt_kicked <= 1'b0;
                            state <= S_ACC_CLR;
                        end else begin
                            se_n <= se_n + 4'd1;
                        end
                    end
                end
            end
            S_R_NTT_RUN: begin
                if (ntt_start) ntt_kicked <= 1'b1;
                if (ntt_done && ntt_kicked) begin cnt <= 9'd0; state <= S_R_NTT_ST; end
            end
            S_R_NTT_ST: begin
                cnt <= cnt + 9'd1;
                if (cnt == 9'd255) begin
                    cnt <= 9'd0; se_n <= se_n + 4'd1; state <= S_R_START;
                end
            end

            // ---------- 主循环 ----------
            S_ACC_CLR: begin
                cnt <= cnt + 9'd1;
                if (cnt == 9'd255) begin
                    cnt <= 9'd0; hdr_cnt <= 6'd0; grp <= 2'd0; pair <= 8'd0;
                    state <= mac_v ? S_MAC_A0 : S_A_START;
                end
            end

            S_A_START: begin hdr_cnt <= 6'd0; state <= S_A_ABS; end
            S_A_ABS: if (sha_in_ready) begin
                hdr_cnt <= hdr_cnt + 6'd1;
                if (hdr_cnt == 6'd33) begin grp <= 2'd0; state <= S_A_FLUSH; end
            end
            S_A_FLUSH: state <= S_A_RUN;
            S_A_RUN: begin
                if (rej_done) begin
                    pair <= 8'd0; state <= S_MAC_A0;
                end else if (sha_out_valid && sha_out_ready) begin
                    if (grp == 2'd2) grp <= 2'd0;
                    else begin
                        grp_buf <= {sha_out_data, grp_buf[15:8]};
                        grp <= grp + 2'd1;
                    end
                end
            end

            S_MAC_A0: state <= S_MAC_A1;
            S_MAC_A1: begin
                a0_r   <= mac_v ? ba_dout : $signed(rej_rd_data);
                b0_r   <= bb_dout;
                acc0_r <= ntt_rd_data;
                state  <= S_MAC_C;
            end
            S_MAC_C: begin
                a1_r   <= mac_v ? ba_dout : $signed(rej_rd_data);
                b1_r   <= bb_dout;
                acc1_r <= ntt_rd_data;
                state  <= S_MAC_M;
            end
            S_MAC_M: begin t_ab_r <= t_ab;  state <= S_MAC_T; end
            S_MAC_T: begin bm0_r <= bm_r0; bm1_r <= bm_r1; state <= S_MAC_W0; end
            S_MAC_W0: state <= S_MAC_W1;
            S_MAC_W1: begin
                if (pair == 8'd127) begin
                    // u 的内层是 j（Âᵀ[i][j] ∘ r̂[j]），v 的内层是 i（t̂[i] ∘ r̂[i]）——
                    // 两条路共用这段逻辑，只有循环变量不同
                    if (mac_idx + 3'd1 == k_r) begin
                        cnt <= 9'd0; ntt_kicked <= 1'b0; ntt_inverse <= 1'b1;
                        state <= S_INV_RUN;
                    end else begin
                        if (mac_v) idx_i <= idx_i + 3'd1;
                        else       idx_j <= idx_j + 3'd1;
                        pair  <= 8'd0;
                        hdr_cnt <= 6'd0; grp <= 2'd0;
                        state <= mac_v ? S_MAC_A0 : S_A_START;
                    end
                end else begin
                    pair  <= pair + 8'd1;
                    state <= S_MAC_A0;
                end
            end

            S_INV_RUN: begin
                if (ntt_start) ntt_kicked <= 1'b1;
                if (ntt_done && ntt_kicked) begin
                    cnt <= 9'd0;
                    obcnt <= 8'd0;
                    out_is_v <= mac_v;
                    out_poly <= mac_v ? 3'd0 : idx_i;
                    state <= S_OUT_RD;
                end
            end

            // 每个系数三拍：RD 发地址、CMP 收数据走 barrett、FEED 等打包器要。
            // 打包器的 in_ready 与 out_valid 互斥，所以等的时候正好在吐字节。
            S_OUT_RD: state <= S_OUT_CMP;
            S_OUT_CMP: begin
                outc_r <= outc;
                state  <= S_OUT_FEED;
            end
            S_OUT_FEED: if (bp_in_ready) begin
                cnt <= cnt + 9'd1;
                if (cnt == 9'd255) state <= S_OUT_DRAIN;
                else               state <= S_OUT_RD;
            end
            S_OUT_DRAIN: if (!bp_out_valid) begin
                // 32·d 比特整字节，打包器一定排空
                if (out_is_v) begin
                    state <= S_DONE;
                end else if (idx_i + 3'd1 == k_r) begin
                    // u 全部输出完 → 转去算 v
                    mac_v <= 1'b1; idx_i <= 3'd0; idx_j <= 3'd0;
                    cnt <= 9'd0; ntt_kicked <= 1'b0; ntt_inverse <= 1'b0;
                    state <= S_ACC_CLR;
                end else begin
                    idx_i <= idx_i + 3'd1; idx_j <= 3'd0;
                    cnt <= 9'd0; ntt_kicked <= 1'b0; ntt_inverse <= 1'b0;
                    state <= S_ACC_CLR;
                end
            end

            S_DONE: begin done <= 1'b1; state <= S_IDLE; end
            default: state <= S_IDLE;
            endcase

            // 这一段密文已经发出去的字节数（out_last 用它判末字节）
            if (bp_fire) obcnt <= obcnt + 8'd1;
        end
    end

    // ================= 仿真观察口 =================
    generate
        if (DEBUG_BANK != 0) begin : g_dbg
            assign dbg_data = u_bank.mem[dbg_addr];
        end else begin : g_nodbg
            assign dbg_data = 16'sd0;
        end
    endgenerate

endmodule

`default_nettype wire
