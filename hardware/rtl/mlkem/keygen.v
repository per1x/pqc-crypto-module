// mlkem_keygen —— ML-KEM.KeyGen_internal 的纯 RTL 实现（FIPS 203 Alg 16 + Alg 13）
//
// 【这一步是什么】
// 之前 PL 里放的都是**算子**：NTT、基乘、压缩、采样、SHA-3。密钥生成的编排
// （谁先谁后、ρ/σ 怎么展开、矩阵怎么累加、字节怎么打包）还在普通世界的 C 里。
// 本模块把那层编排也搬进 PL：普通世界只给 d、z 两个 32 字节种子，拿回 ek、dk
// 两串字节，**中间的 ŝ、ê、Â、σ 一个都不出芯片**。
//
// 这才是密码机里"密钥不出边界"那句话的硬件形态 —— 私钥的每一个中间量都只在
// PL 的 BRAM 里存在过，普通世界的地址空间里从来没有它们。
//
// 【参数集在运行时选】
//   param_set = 0 → ML-KEM-512 （k=2, η1=3）
//               1 → ML-KEM-768 （k=3, η1=2）
//               2 → ML-KEM-1024（k=4, η1=2）
// 三套共用同一份数据通路，只是循环次数和 η 不同。为三个参数集各综合一份
// 在真密码机里是说不通的：同一块板要同时支持多套参数。
//
// 【与 hardware/model/mlkem_oracle.py 的 mlkem_keygen 逐字节对齐】
// 那个模型是拿 NIST ACVP 向量验过的，所以本模块的对拍不是"自己和自己比"。
// 特别注意两处容易漂的地方：
//   ① Â[i][j] 的 XOF 头是 ρ‖j‖i（j 在前），不是 ρ‖i‖j；
//   ② t̂ 的累加要先 barrett，再乘 f=2³²mod q 走一次 Montgomery（"搬进蒙域"），
//      最后加 ê 再 barrett。少一步或换个顺序，结果照样是合法多项式，但不是
//      ML-KEM 的那个 t̂。
//
// 【累加为什么可以每一步都 barrett】
// 参考实现是把 k 个基乘全加完再 barrett 一次；这里是每加一个就 barrett。
// 两者结果**逐系数完全相同**：barrett_reduce 把输入映到 (−q/2, q/2] 里唯一的
// 代表元，同余类相同 ⇒ 输出相同。好处是累加器恒定在 ±1664，加上一个基乘输出
// （范围 (−q, q)）也就 ±4993，稳稳落在 16 位有符号里 —— 不必为 k=4 的
// 最坏情况把累加器加宽。
//
// 【一个 sha3_core 走完全程】
// G（SHA3-512）、PRF（SHAKE256）、XOF（SHAKE128）、H（SHA3-256）四种用法在时间上
// 互不重叠，共用同一个海绵核。四种只差 rate 和 suffix 两个字节，start 那一拍
// 锁存即可。多例化一个置换核要 ~7000 LUT，这颗片子上不划算。
//
// 【时序不是重点】
// 一次 KeyGen 大约：ML-KEM-768 ≈ 12 万周期 @100 MHz ≈ 1.2 ms。
// 软件实现是几十微秒。慢在两处：NTT 一个蝶形两拍（S3 换 BRAM 的代价），
// 基乘一对系数五拍（单口读、不流水）。本项目要的是"能放进去、算得对"，
// 真要提速，把基乘做成四路并行即可 —— 面积还有八成空着。
`default_nettype none

module mlkem_keygen #(
    // ⚠️ 仿真专用。为 1 时把多项式存储的读口引出来，让 cocotb 能逐步核对
    // ŝ / ê / t̂。**综合时必须是 0** —— 那个口直连私钥系数，引出来等于在
    // 密码边界上开一个洞。综合脚本会检查这一点。
    parameter integer DEBUG_BANK = 0
) (
    input  wire        clk,
    input  wire        rst_n,

    // ---- 参数与种子 ----
    input  wire [1:0]  param_set,   // 0=512 1=768 2=1024
    input  wire [255:0] d_in,       // KeyGen_internal 的 d
    input  wire [255:0] z_in,       // KeyGen_internal 的 z（隐式拒绝用）

    input  wire        start,       // 脉冲
    output reg         done,        // 电平，保持到下一次 start

    // ---- 输出字节流：ek 紧接着 dk ----
    output wire        out_valid,
    input  wire        out_ready,
    output wire [7:0]  out_data,
    output wire        out_last,    // dk 的最后一个字节

    // ---- 仿真观察口（DEBUG_BANK=1 时才接出）----
    input  wire [11:0] dbg_addr,
    output wire signed [15:0] dbg_data
);
    // ================= 参数集 =================
    // k：向量维数；eta1_3：s/e 用 η=3 还是 η=2
    wire [2:0] k_now    = (param_set == 2'd0) ? 3'd2 :
                          (param_set == 2'd1) ? 3'd3 : 3'd4;
    wire       eta1_3_now = (param_set == 2'd0);

    reg [2:0] k_r;
    reg       eta1_3_r;

    // ================= 多项式存储 =================
    // 16 个槽 × 256 个系数 × 16 bit = 64 Kbit ≈ 2 块 RAMB36。
    //   槽 0..3 : ŝ[0..k-1]
    //   槽 4..7 : ê[0..k-1]
    //   槽 8..11: t̂[0..k-1]（也当 Â∘ŝ 的累加器用 —— 它们本来就是同一个量）
    localparam [3:0] SL_SHAT = 4'd0;
    localparam [3:0] SL_EHAT = 4'd4;
    localparam [3:0] SL_THAT = 4'd8;

    reg         ba_we, bb_we;
    reg  [11:0] ba_addr, bb_addr;
    reg  signed [15:0] ba_din, bb_din;
    wire signed [15:0] ba_dout, bb_dout;

    ram_dp #(.DW(16), .AW(12)) u_bank (
        .clk(clk),
        .a_we(ba_we), .a_addr(ba_addr), .a_din(ba_din), .a_dout(ba_dout),
        .b_we(bb_we), .b_addr(bb_addr), .b_din(bb_din), .b_dout(bb_dout));

    // ================= 基乘用的 ζ 表（ZETAS[64..127]）=================
    (* rom_style = "distributed" *)
    reg signed [15:0] bz [0:63];
    initial begin
        bz[ 0]=-16'sd1103; bz[ 1]= 16'sd430;  bz[ 2]= 16'sd555;  bz[ 3]= 16'sd843;
        bz[ 4]=-16'sd1251; bz[ 5]= 16'sd871;  bz[ 6]= 16'sd1550; bz[ 7]= 16'sd105;
        bz[ 8]= 16'sd422;  bz[ 9]= 16'sd587;  bz[10]= 16'sd177;  bz[11]=-16'sd235;
        bz[12]=-16'sd291;  bz[13]=-16'sd460;  bz[14]= 16'sd1574; bz[15]= 16'sd1653;
        bz[16]=-16'sd246;  bz[17]= 16'sd778;  bz[18]= 16'sd1159; bz[19]=-16'sd147;
        bz[20]=-16'sd777;  bz[21]= 16'sd1483; bz[22]=-16'sd602;  bz[23]= 16'sd1119;
        bz[24]=-16'sd1590; bz[25]= 16'sd644;  bz[26]=-16'sd872;  bz[27]= 16'sd349;
        bz[28]= 16'sd418;  bz[29]= 16'sd329;  bz[30]=-16'sd156;  bz[31]=-16'sd75;
        bz[32]= 16'sd817;  bz[33]= 16'sd1097; bz[34]= 16'sd603;  bz[35]= 16'sd610;
        bz[36]= 16'sd1322; bz[37]=-16'sd1285; bz[38]=-16'sd1465; bz[39]= 16'sd384;
        bz[40]=-16'sd1215; bz[41]=-16'sd136;  bz[42]= 16'sd1218; bz[43]=-16'sd1335;
        bz[44]=-16'sd874;  bz[45]= 16'sd220;  bz[46]=-16'sd1187; bz[47]=-16'sd1659;
        bz[48]=-16'sd1185; bz[49]=-16'sd1530; bz[50]=-16'sd1278; bz[51]= 16'sd794;
        bz[52]=-16'sd1510; bz[53]=-16'sd854;  bz[54]=-16'sd870;  bz[55]= 16'sd478;
        bz[56]=-16'sd108;  bz[57]=-16'sd308;  bz[58]= 16'sd996;  bz[59]= 16'sd991;
        bz[60]= 16'sd958;  bz[61]=-16'sd1460; bz[62]= 16'sd1522; bz[63]= 16'sd1628;
    end

    // ================= 状态机 =================
    localparam [5:0]
        S_IDLE     = 6'd0,
        S_G_START  = 6'd1,
        S_G_ABS    = 6'd2,
        S_G_FLUSH  = 6'd3,
        S_G_SQ     = 6'd4,
        S_SE_START = 6'd5,
        S_SE_ABS   = 6'd6,
        S_SE_FLUSH = 6'd7,
        S_SE_RUN   = 6'd8,
        S_NTT_RUN  = 6'd9,
        S_NTT_PRE  = 6'd10,
        S_NTT_ST   = 6'd11,
        S_ACC_CLR  = 6'd12,
        S_A_START  = 6'd13,
        S_A_ABS    = 6'd14,
        S_A_FLUSH  = 6'd15,
        S_A_RUN    = 6'd16,
        S_MAC_A0   = 6'd17,
        S_MAC_A1   = 6'd18,
        S_MAC_C    = 6'd19,
        S_MAC_W0   = 6'd20,
        S_MAC_W1   = 6'd21,
        S_PO_RD    = 6'd22,
        S_PO_C     = 6'd23,
        S_PO_W     = 6'd24,
        S_H_START  = 6'd25,
        S_OUT_RD0  = 6'd26,
        S_OUT_RD1  = 6'd27,
        S_OUT_RD2  = 6'd28,
        S_OUT_B    = 6'd29,
        S_OUT_REG  = 6'd30,
        S_H_FLUSH  = 6'd31,
        S_H_SQ     = 6'd32,
        S_DONE     = 6'd33;

    reg [5:0] state;

    // ================= 子模块的控制线（由状态机驱动）=================
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
    wire [8:0]  rej_count;
    wire [15:0] rej_rd_data;

    mlkem_rej_uniform u_rej (
        .clk(clk), .rst_n(rst_n),
        .start(rej_start), .done(rej_done),
        .in_valid(rej_in_valid), .in_bytes(rej_in_bytes), .in_ready(rej_in_ready),
        .count(rej_count), .rd_addr(rej_rd_addr), .rd_data(rej_rd_data));

    reg         cbd_start, cbd_out_ready;
    wire        cbd_done, cbd_in_ready, cbd_out_valid;
    wire signed [15:0] cbd_out_coeff;
    wire [8:0]  cbd_count;

    // s/e 的采样字节要在 sha3 出口和 cbd 入口之间握手：
    // 只有状态机在 S_SE_RUN 且 cbd 要字节时，才算真的取走一个字节。
    wire cbd_fb_valid = (state == S_SE_RUN) && sha_out_valid;

    mlkem_cbd_stream u_cbd (
        .clk(clk), .rst_n(rst_n),
        .eta3(eta1_3_r), .start(cbd_start), .done(cbd_done),
        .in_valid(cbd_fb_valid), .in_ready(cbd_in_ready), .in_data(sha_out_data),
        .out_valid(cbd_out_valid), .out_ready(cbd_out_ready),
        .out_coeff(cbd_out_coeff), .count(cbd_count));

    reg         ntt_start, ntt_wr_en;
    reg  [7:0]  ntt_wr_addr, ntt_rd_addr;
    reg  signed [15:0] ntt_wr_data;
    wire        ntt_done;
    wire signed [15:0] ntt_rd_data;

    ntt_core u_ntt (
        .clk(clk), .rst_n(rst_n),
        .start(ntt_start), .inverse(1'b0), .done(ntt_done),
        .wr_en(ntt_wr_en), .wr_addr(ntt_wr_addr), .wr_data(ntt_wr_data),
        .rd_addr(ntt_rd_addr), .rd_data(ntt_rd_data));


    reg [255:0] rho_r, sigma_r, h_r, d_r, z_r;
    reg [2:0]   idx_i, idx_j;      // 矩阵行列
    reg [2:0]   se_n;              // s/e 的第几个多项式（0..2k-1）
    reg [8:0]   cnt;               // 通用系数/字节计数
    reg [5:0]   hdr_cnt;           // 吸收头部的字节计数
    reg [7:0]   pair;              // 基乘的第几对（0..127）
    reg         ntt_kicked;        // NTT 的 start 已经发过了（不能靠计数器判，
                                   // 一次变换 2305 拍，9 位计数器会绕回去再踢一次）
    reg signed [15:0] a0_r, a1_r, b0_r, b1_r, acc0_r, acc1_r;
    reg [2:0]   sec;               // 输出段号
    reg [1:0]   obyte;             // 一对系数的第几个字节
    reg signed [15:0] enc_c0, enc_c1;
    reg [2:0]   out_poly;          // 输出时的第几个多项式
    reg         feed_h;            // 这一段是否同时喂给 SHA3-256

    // ---- 组合：基乘 ----
    wire signed [15:0] bm_zeta_raw = bz[pair[7:1]];
    wire signed [15:0] bm_zeta = pair[0] ? -bm_zeta_raw : bm_zeta_raw;
    wire signed [15:0] bm_r0, bm_r1;
    mlkem_basemul u_bm (
        .a0(a0_r), .a1(a1_r), .b0(b0_r), .b1(b1_r), .zeta(bm_zeta),
        .r0(bm_r0), .r1(bm_r1));

    wire signed [15:0] acc0_next, acc1_next;
    barrett_reduce u_br0 (.a(acc0_r + bm_r0), .r(acc0_next));
    barrett_reduce u_br1 (.a(acc1_r + bm_r1), .r(acc1_next));

    // ---- 组合：搬进蒙域 + 加 ê + barrett ----
    localparam signed [15:0] F_TOMONT = 16'sd1353;   // 2³² mod q
    wire signed [31:0] po_prod = $signed({{16{acc0_r[15]}}, acc0_r}) * F_TOMONT;
    wire signed [15:0] po_mont;
    mont_reduce u_po_mont (.a(po_prod), .t_out(po_mont));
    wire signed [15:0] po_out;
    barrett_reduce u_po_br (.a(po_mont + acc1_r), .r(po_out));   // acc1_r 这里存 ê

    // ---- 组合：12 位编码 ----
    wire [23:0] enc_bytes;
    mlkem_encode12 u_enc (.c0(enc_c0), .c1(enc_c1), .bytes_out(enc_bytes));

    // ---- 输出字节的来源 ----
    // sec 0/3 走多项式编码，其余几段是寄存器里的 32 字节
    reg [255:0] reg_src;
    wire [7:0] reg_byte = reg_src[{cnt[4:0], 3'd0} +: 8];
    wire [7:0] enc_byte = (obyte == 2'd0) ? enc_bytes[7:0] :
                          (obyte == 2'd1) ? enc_bytes[15:8] : enc_bytes[23:16];

    wire out_is_reg = (state == S_OUT_REG);
    assign out_data  = out_is_reg ? reg_byte : enc_byte;

    // ⚠️ ek 那两段的字节要**同时**交给下游和 H 的海绵，两边都收得下才算走掉。
    // 海绵每吸满 136 字节要停下来做 24 拍置换，那段时间 sha_in_ready 为低。
    // 这个条件必须体现在 **out_valid** 上，不能只体现在内部的推进条件上：
    // AXI4-Stream 的规矩是 valid 与 ready 同时为高就必须完成一次传输。
    // 若 valid 一直挂着而字节其实没走，下游会把同一个字节收好几遍 ——
    // 实测正是这样多出 233 字节（≈9 次置换 × 26 拍）。
    assign out_valid = ((state == S_OUT_B) || (state == S_OUT_REG))
                       && (!feed_h || sha_in_ready);
    // dk 的最后一个字节 = 最后一段（z）的第 32 个字节
    assign out_last  = out_is_reg && (sec == 3'd6) && (cnt == 9'd31);

    wire byte_fire = out_valid && out_ready;

    // ---- 吸收头部的字节：前 32 个来自种子寄存器，之后是索引 ----
    reg [255:0] hdr_seed;
    reg [7:0]   hdr_tail0, hdr_tail1;
    reg [5:0]   hdr_len;
    wire [7:0]  hdr_byte = (hdr_cnt < 6'd32) ? hdr_seed[{hdr_cnt[4:0], 3'd0} +: 8]
                         : (hdr_cnt == 6'd32) ? hdr_tail0 : hdr_tail1;

    // ---- 存储的槽号：地址 = {槽号 4 位, 系数下标 8 位} ----
    // 拼在 case 里算会让每一处都写一遍加法，也容易把位宽算错，统一提出来。
    wire [3:0] slot_se     = (se_n < k_r) ? (SL_SHAT + {1'b0, se_n})
                                          : (SL_EHAT + {1'b0, (se_n - k_r)});
    wire [3:0] slot_shat_j = SL_SHAT + {1'b0, idx_j};
    wire [3:0] slot_ehat_i = SL_EHAT + {1'b0, idx_i};
    wire [3:0] slot_that_i = SL_THAT + {1'b0, idx_i};
    wire [3:0] slot_out    = (sec == 3'd2) ? (SL_SHAT + {1'b0, out_poly})
                                           : (SL_THAT + {1'b0, out_poly});

    // ---- 拒绝采样的 3 字节打包 ----
    reg [15:0] pk_buf;
    reg [1:0]  pk_cnt;

    // ================= 组合：子模块控制 + 存储端口 =================
    always @(*) begin
        // 默认值
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

        reg_src = rho_r;

        case (state)
        // ---------- G(d‖k) = SHA3-512 ----------
        // sha3_core 只在 start 那一拍锁存 rate/suffix，所以每换一种用法
        // 都要单独走一个 start 状态。四种用法：
        //   G   SHA3-512  rate 72  suffix 06
        //   PRF SHAKE256  rate 136 suffix 1F
        //   XOF SHAKE128  rate 168 suffix 1F
        //   H   SHA3-256  rate 136 suffix 06
        S_G_START: begin
            sha_rate = 8'd72; sha_suffix = 8'h06;
            sha_start = 1'b1;
        end
        S_G_ABS: begin
            sha_rate = 8'd72; sha_suffix = 8'h06;
            sha_in_valid = 1'b1;
            sha_in_data  = hdr_byte;
        end
        S_G_FLUSH: begin
            sha_rate = 8'd72; sha_suffix = 8'h06;
            sha_in_flush = 1'b1;
        end
        S_G_SQ: begin
            sha_rate = 8'd72; sha_suffix = 8'h06;
            sha_out_ready = 1'b1;
        end

        // ---------- PRF(σ‖N) = SHAKE256 → CBD → NTT ----------
        S_SE_START: sha_start = 1'b1;      // rate/suffix 用默认值（136 / 1F）
        S_SE_ABS: begin
            sha_in_valid = 1'b1;
            sha_in_data  = hdr_byte;
        end
        S_SE_FLUSH: begin
            // 这两个状态各只停留一拍，所以在这里给的就是单周期脉冲
            sha_in_flush = 1'b1;
            cbd_start    = 1'b1;
        end
        S_SE_RUN: begin
            // 海绵的字节直接进 cbd；cbd 吐的系数直接进 ntt_core 的写口
            sha_out_ready = cbd_in_ready;
            cbd_out_ready = 1'b1;
            ntt_wr_en     = cbd_out_valid;
            ntt_wr_addr   = cbd_count[7:0];
            ntt_wr_data   = cbd_out_coeff;
        end
        S_NTT_RUN: ntt_start = !ntt_kicked;
        S_NTT_PRE: ntt_rd_addr = 8'd0;
        S_NTT_ST: begin
            // ntt_core 的读口是同步读，地址要提前一拍
            ntt_rd_addr = cnt[7:0] + 8'd1;
            ba_we   = 1'b1;
            ba_addr = {slot_se, cnt[7:0]};
            ba_din  = ntt_rd_data;
        end

        // ---------- 累加器清零 ----------
        S_ACC_CLR: begin
            bb_we   = 1'b1;
            bb_addr = {slot_that_i, cnt[7:0]};
            bb_din  = 16'sd0;
        end

        // ---------- XOF(ρ‖j‖i) = SHAKE128 → 拒绝采样 ----------
        S_A_START: begin
            sha_rate = 8'd168;
            sha_start = 1'b1;
        end
        S_A_ABS: begin
            sha_rate = 8'd168;
            sha_in_valid = 1'b1;
            sha_in_data  = hdr_byte;
        end
        S_A_FLUSH: begin
            sha_rate = 8'd168;
            sha_in_flush = 1'b1;
            rej_start    = 1'b1;
        end
        S_A_RUN: begin
            sha_rate = 8'd168;
            // 攒够 3 个字节才推给收集器；收集器满了就不再抽海绵
            sha_out_ready = !rej_done && (pk_cnt != 2'd2 || rej_in_ready);
            rej_in_valid  = (pk_cnt == 2'd2) && sha_out_valid && !rej_done;
            rej_in_bytes  = {sha_out_data, pk_buf};
        end

        // ---------- Â[i][j] ∘ ŝ[j] 累加 ----------
        S_MAC_A0: begin
            rej_rd_addr = {pair[6:0], 1'b0};
            ba_addr     = {slot_shat_j, pair[6:0], 1'b0};
            bb_addr     = {slot_that_i, pair[6:0], 1'b0};
        end
        S_MAC_A1: begin
            rej_rd_addr = {pair[6:0], 1'b1};
            ba_addr     = {slot_shat_j, pair[6:0], 1'b1};
            bb_addr     = {slot_that_i, pair[6:0], 1'b1};
        end
        S_MAC_W0: begin
            bb_we   = 1'b1;
            bb_addr = {slot_that_i, pair[6:0], 1'b0};
            bb_din  = acc0_next;
        end
        S_MAC_W1: begin
            bb_we   = 1'b1;
            bb_addr = {slot_that_i, pair[6:0], 1'b1};
            bb_din  = acc1_next;
        end

        // ---------- 搬进蒙域 + 加 ê ----------
        S_PO_RD: begin
            ba_addr = {slot_ehat_i, cnt[7:0]};
            bb_addr = {slot_that_i, cnt[7:0]};
        end
        S_PO_W: begin
            bb_we   = 1'b1;
            bb_addr = {slot_that_i, cnt[7:0]};
            bb_din  = po_out;
        end

        // ---------- 输出 ----------
        S_H_START: begin
            sha_suffix = 8'h06;            // rate 用默认 136 = SHA3-256
            sha_start  = 1'b1;
        end
        S_OUT_RD0: bb_addr = {slot_out, cnt[6:0], 1'b0};
        S_OUT_RD1: bb_addr = {slot_out, cnt[6:0], 1'b1};
        S_OUT_RD2: ;   // 等 bb_dout 把第二个系数送出来，见下面的时序块
        S_OUT_B: begin
            sha_suffix   = 8'h06;
            sha_in_valid = feed_h && byte_fire;
            sha_in_data  = enc_byte;
        end
        S_OUT_REG: begin
            sha_suffix   = 8'h06;
            reg_src      = (sec == 3'd5) ? h_r : (sec == 3'd6) ? z_r : rho_r;
            sha_in_valid = feed_h && byte_fire;
            sha_in_data  = reg_byte;
        end
        S_H_FLUSH: begin
            sha_suffix = 8'h06;
            sha_in_flush = 1'b1;
        end
        S_H_SQ: begin
            sha_suffix = 8'h06;
            sha_out_ready = 1'b1;
        end
        default: ;
        endcase
    end

    // ================= 时序 =================
    wire [8:0] se_bytes = eta1_3_r ? 9'd192 : 9'd128;   // 64·η1

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= S_IDLE;
            done  <= 1'b0;
            k_r <= 3'd3; eta1_3_r <= 1'b0;
            rho_r <= 256'd0; sigma_r <= 256'd0; h_r <= 256'd0;
            d_r <= 256'd0; z_r <= 256'd0;
            idx_i <= 3'd0; idx_j <= 3'd0; se_n <= 3'd0;
            cnt <= 9'd0; hdr_cnt <= 6'd0; hdr_len <= 6'd33;
            hdr_seed <= 256'd0; hdr_tail0 <= 8'd0; hdr_tail1 <= 8'd0;
            pair <= 8'd0; pk_buf <= 16'd0; pk_cnt <= 2'd0; ntt_kicked <= 1'b0;
            a0_r <= 16'sd0; a1_r <= 16'sd0; b0_r <= 16'sd0; b1_r <= 16'sd0;
            acc0_r <= 16'sd0; acc1_r <= 16'sd0;
            sec <= 3'd0; obyte <= 2'd0; out_poly <= 3'd0; feed_h <= 1'b0;
            enc_c0 <= 16'sd0; enc_c1 <= 16'sd0;
        end else if (start) begin
            done     <= 1'b0;
            k_r      <= k_now;
            eta1_3_r <= eta1_3_now;
            d_r      <= d_in;
            z_r      <= z_in;
            hdr_seed <= d_in;
            hdr_tail0 <= {5'd0, k_now};     // G 吸收的是 d‖k
            hdr_len  <= 6'd33;
            hdr_cnt  <= 6'd0;
            cnt      <= 9'd0;
            idx_i    <= 3'd0; idx_j <= 3'd0; se_n <= 3'd0;
            sec      <= 3'd0; out_poly <= 3'd0; obyte <= 2'd0;
            feed_h   <= 1'b0;
            ntt_kicked <= 1'b0;
            state    <= S_G_START;
        end else begin
            case (state)
            S_IDLE: ;
            S_G_START: state <= S_G_ABS;

            // ---------- G ----------
            S_G_ABS: if (sha_in_ready) begin
                if (hdr_cnt + 6'd1 == hdr_len) begin
                    hdr_cnt <= 6'd0;
                    state   <= S_G_FLUSH;
                end else begin
                    hdr_cnt <= hdr_cnt + 6'd1;
                end
            end
            S_G_FLUSH: begin
                cnt   <= 9'd0;
                state <= S_G_SQ;
            end
            S_G_SQ: if (sha_out_valid) begin
                if (cnt < 9'd32) begin
                    rho_r[{cnt[4:0], 3'd0} +: 8] <= sha_out_data;
                end else begin
                    sigma_r[{cnt[4:0], 3'd0} +: 8] <= sha_out_data;
                end
                if (cnt == 9'd63) begin
                    // 第一个 s：σ‖0。σ 的最后一个字节就是这一拍写进去的，
                    // 所以 hdr_seed 要用「带上这个字节」的值，不能直接抄 sigma_r。
                    se_n     <= 3'd0;
                    hdr_cnt  <= 6'd0;
                    hdr_len  <= 6'd33;
                    hdr_tail0 <= 8'd0;
                    cnt      <= 9'd0;
                    state    <= S_SE_START;
                end else begin
                    cnt <= cnt + 9'd1;
                end
            end

            // ---------- s/e 采样 ----------
            S_SE_START: begin
                hdr_seed <= sigma_r;   // 此刻 σ 已经完整了
                state    <= S_SE_ABS;
            end
            S_SE_ABS: begin
                if (sha_in_ready) begin
                    if (hdr_cnt + 6'd1 == hdr_len) begin
                        hdr_cnt <= 6'd0;
                        state   <= S_SE_FLUSH;
                    end else begin
                        hdr_cnt <= hdr_cnt + 6'd1;
                    end
                end
            end
            S_SE_FLUSH: begin
                cnt   <= 9'd0;
                state <= S_SE_RUN;
            end
            S_SE_RUN: if (cbd_done) begin
                cnt   <= 9'd0;
                state <= S_NTT_RUN;
            end
            S_NTT_RUN: begin
                if (!ntt_kicked) begin
                    ntt_kicked <= 1'b1;         // 这一拍组合口发出了 start
                end else if (ntt_done) begin
                    ntt_kicked <= 1'b0;
                    cnt        <= 9'd0;
                    state      <= S_NTT_PRE;
                end
            end
            S_NTT_PRE: begin
                cnt   <= 9'd0;
                state <= S_NTT_ST;
            end
            S_NTT_ST: begin
                if (cnt == 9'd255) begin
                    cnt <= 9'd0;
                    if (({1'b0, se_n} + 4'd1) == {k_r, 1'b0}) begin
                        // 2k 个都采完了 → 开始矩阵与累加
                        idx_i   <= 3'd0;
                        idx_j   <= 3'd0;
                        state   <= S_ACC_CLR;
                    end else begin
                        se_n     <= se_n + 3'd1;
                        hdr_cnt  <= 6'd0;
                        hdr_len  <= 6'd33;
                        hdr_tail0 <= {5'd0, se_n + 3'd1};
                        state    <= S_SE_START;
                    end
                end else begin
                    cnt <= cnt + 9'd1;
                end
            end

            // ---------- 累加器清零 ----------
            S_ACC_CLR: begin
                if (cnt == 9'd255) begin
                    cnt      <= 9'd0;
                    hdr_cnt  <= 6'd0;
                    hdr_len  <= 6'd34;
                    hdr_seed <= rho_r;
                    hdr_tail0 <= {5'd0, idx_j};    // ρ‖j‖i —— j 在前
                    hdr_tail1 <= {5'd0, idx_i};
                    state    <= S_A_START;
                end else begin
                    cnt <= cnt + 9'd1;
                end
            end

            // ---------- Â[i][j] ----------
            S_A_START: state <= S_A_ABS;
            S_A_ABS: if (sha_in_ready) begin
                if (hdr_cnt + 6'd1 == hdr_len) begin
                    hdr_cnt <= 6'd0;
                    state   <= S_A_FLUSH;
                end else begin
                    hdr_cnt <= hdr_cnt + 6'd1;
                end
            end
            S_A_FLUSH: begin
                pk_cnt <= 2'd0;
                state  <= S_A_RUN;
            end
            S_A_RUN: begin
                if (rej_done) begin
                    pair  <= 8'd0;
                    state <= S_MAC_A0;
                end else if (sha_out_valid && sha_out_ready) begin
                    if (pk_cnt == 2'd2) begin
                        pk_cnt <= 2'd0;
                    end else begin
                        pk_buf[{pk_cnt[0], 3'd0} +: 8] <= sha_out_data;
                        pk_cnt <= pk_cnt + 2'd1;
                    end
                end
            end

            // ---------- 基乘累加 ----------
            S_MAC_A0: state <= S_MAC_A1;
            S_MAC_A1: begin
                a0_r   <= $signed(rej_rd_data);
                b0_r   <= ba_dout;
                acc0_r <= bb_dout;
                state  <= S_MAC_C;
            end
            S_MAC_C: begin
                a1_r   <= $signed(rej_rd_data);
                b1_r   <= ba_dout;
                acc1_r <= bb_dout;
                state  <= S_MAC_W0;
            end
            S_MAC_W0: state <= S_MAC_W1;
            S_MAC_W1: begin
                if (pair == 8'd127) begin
                    if (idx_j + 3'd1 == k_r) begin
                        cnt   <= 9'd0;
                        state <= S_PO_RD;
                    end else begin
                        idx_j    <= idx_j + 3'd1;
                        hdr_cnt  <= 6'd0;
                        hdr_len  <= 6'd34;
                        hdr_seed <= rho_r;
                        hdr_tail0 <= {5'd0, idx_j + 3'd1};
                        hdr_tail1 <= {5'd0, idx_i};
                        state    <= S_A_START;
                    end
                end else begin
                    pair  <= pair + 8'd1;
                    state <= S_MAC_A0;
                end
            end

            // ---------- 搬进蒙域 + 加 ê ----------
            S_PO_RD: state <= S_PO_C;
            S_PO_C: begin
                acc0_r <= bb_dout;      // Â∘ŝ 的累加值
                acc1_r <= ba_dout;      // ê[i]
                state  <= S_PO_W;
            end
            S_PO_W: begin
                if (cnt == 9'd255) begin
                    cnt <= 9'd0;
                    if (idx_i + 3'd1 == k_r) begin
                        // 全部 t̂ 就绪 → 开始输出，第一段同时喂 H
                        sec      <= 3'd0;
                        out_poly <= 3'd0;
                        obyte    <= 2'd0;
                        feed_h   <= 1'b1;
                        state    <= S_H_START;
                    end else begin
                        idx_i    <= idx_i + 3'd1;
                        idx_j    <= 3'd0;
                        state    <= S_ACC_CLR;
                    end
                end else begin
                    cnt   <= cnt + 9'd1;
                    state <= S_PO_RD;
                end
            end

            // ---------- 输出 ----------
            S_H_START: state <= S_OUT_RD0;
            // 存储是同步读：RD0 发偶数地址、RD1 收它并发奇数地址、
            // RD2 收奇数地址。少了 RD2 的话进 S_OUT_B 的第一拍 enc_c1 还是旧值，
            // 而 out_valid 已经拉高 —— 每三个字节里就有一个是错的。
            S_OUT_RD0: state <= S_OUT_RD1;
            S_OUT_RD1: begin
                enc_c0 <= bb_dout;
                state  <= S_OUT_RD2;
            end
            S_OUT_RD2: begin
                enc_c1 <= bb_dout;
                state  <= S_OUT_B;
            end
            S_OUT_B: begin
                if (byte_fire) begin
                    if (obyte == 2'd2) begin
                        obyte <= 2'd0;
                        if (cnt == 9'd127) begin
                            cnt <= 9'd0;
                            if (out_poly + 3'd1 == k_r) begin
                                out_poly <= 3'd0;
                                // 一段多项式向量结束
                                case (sec)
                                3'd0: begin sec <= 3'd1; cnt <= 9'd0; state <= S_OUT_REG; end
                                3'd2: begin sec <= 3'd3; state <= S_OUT_RD0; end
                                3'd3: begin sec <= 3'd4; cnt <= 9'd0; state <= S_OUT_REG; end
                                default: state <= S_DONE;
                                endcase
                            end else begin
                                out_poly <= out_poly + 3'd1;
                                state    <= S_OUT_RD0;
                            end
                        end else begin
                            cnt   <= cnt + 9'd1;
                            state <= S_OUT_RD0;
                        end
                    end else begin
                        obyte <= obyte + 2'd1;
                    end
                end
            end
            S_OUT_REG: if (byte_fire) begin
                if (cnt == 9'd31) begin
                    cnt <= 9'd0;
                    case (sec)
                    3'd1: begin
                        // ek 吸收完了 → 收尾出 H(ek)
                        feed_h <= 1'b0;
                        state  <= S_H_FLUSH;
                    end
                    3'd4: begin sec <= 3'd5; state <= S_OUT_REG; end
                    3'd5: begin sec <= 3'd6; state <= S_OUT_REG; end
                    default: state <= S_DONE;
                    endcase
                end else begin
                    cnt <= cnt + 9'd1;
                end
            end
            S_H_FLUSH: begin
                cnt   <= 9'd0;
                state <= S_H_SQ;
            end
            S_H_SQ: if (sha_out_valid) begin
                h_r[{cnt[4:0], 3'd0} +: 8] <= sha_out_data;
                if (cnt == 9'd31) begin
                    cnt      <= 9'd0;
                    sec      <= 3'd2;
                    out_poly <= 3'd0;
                    obyte    <= 2'd0;
                    state    <= S_OUT_RD0;
                end else begin
                    cnt <= cnt + 9'd1;
                end
            end

            S_DONE: begin
                done  <= 1'b1;
                state <= S_IDLE;
            end
            default: state <= S_IDLE;
            endcase
        end
    end

    // ================= 仿真观察口 =================
    generate
    if (DEBUG_BANK != 0) begin : g_dbg
        // 只在仿真里例化。存储是双口的，两个口都被状态机占着，
        // 所以这里不能再挤一个读口 —— 直接看数组本身。
        assign dbg_data = u_bank.mem[dbg_addr];
    end else begin : g_nodbg
        assign dbg_data = 16'sd0;
    end
    endgenerate
endmodule

`default_nettype wire
