// mldsa_engine —— ML-DSA 三个整核（KeyGen / Sign / Verify）的**适配层**
//
//   mldsa_axi ──▶ 输入字节缓冲(32 KB) ──喂数 FSM──┬─▶ mldsa_keygen
//                                                 ├─▶ mldsa_sign
//                                                 └─▶ mldsa_verify
//                 out_addr ──读口选通──────────────┘（直接读核里的输出缓冲）
//
// ============================================================================
// 【这一层解决什么】
// ============================================================================
// 三个核的**原生输入形状完全不同**：
//   KeyGen : 一个 256 位并行的 ξ
//   Sign   : sk / msg / ctx 三条字节流（各自独立的写口与地址空间）+ 256 位并行 rnd
//   Verify : pk / sig / msg / ctx 四条字节流
// 而 AXI 那一侧只有**一个**字节写口（in_we/in_addr/in_data）。
//
// 本模块把"一个字节口"翻译成"三个核各自的原生端口"，排布就是标准里的顺序：
//
//   KeyGen : ξ(32)
//   Sign   : sk(SK) ‖ rnd(32) ‖ ctx(ctx_len) ‖ msg(msg_len)
//   Verify : pk(PK) ‖ sig(SIG) ‖ ctx(ctx_len) ‖ msg(msg_len)
//
// 需要并行送进核里的那两个 256 位量（ξ / rnd）由本模块从缓冲区里读出来装进
// 寄存器 —— **软件不必知道哪些是"流"、哪些是"并行口"**。
// 这个模式照 mlkem_axi 的文件头那段，本仓库已有先例，不是新发明。
//
// ⚠️ **三个核的内部一个字都没动。** 各自保留自己的 NTT 与算术。
//    把 NTT / mldsa_mont_mul_pipe 提出来共享是第二期的事，
//    而按面积账（三个核 87 下约 12.4K LUT / 60 DSP，余量 34.7K / 220）**不共享也塞得下**，
//    所以第二期要不要做由整体综合的实测决定，不预先动已对上 ACVP 的核。
//
// ============================================================================
// 【输出不另开缓冲：直接选通核里的读口】
// ============================================================================
// KeyGen 的 pk/sk、Sign 的 sig 本来就在核内部的输出缓冲里，各自有按字节的读口。
// 再抄一份到 engine 自己的缓冲里，只是多花 8 个 BRAM tile 去存同一份字节。
// 所以 out_addr 直接按 op 与段边界翻译成核的 pk_addr/sk_addr/sig_addr。
//   KeyGen : [0,PKLEN) → pk_addr，[PKLEN, PKLEN+SKLEN) → sk_addr
//   Sign   : [0,SIGLEN) → sig_addr
//   Verify : 不出字节（结果看 verify_ok）
// 读口是 ram_dp 的同步读，一拍延迟 —— 与 AXI 侧的约定一致。
//
// ============================================================================
// 【⚠️ 消息长度的真实上限是 8192，不是地址空间算出来的那个数】
// ============================================================================
// in_addr 有 15 位（32768 字节），但**三个核内部的 msg 缓冲是 AW=13，即 8192 字节**
// （sign.v / verify.v 里的 `ram_dp #(.DW(8), .AW(13)) u_msg`）。
// 也就是说地址空间算出来的"Sign-87 可喂 27840 字节"是够不着的 ——
// 真正的瓶颈在核里，不在缓冲区。
// 本模块因此在 START 时校验 msg_len ≤ 8192，超了**拒绝启动并置 param_err**，
// 而不是让高位地址安静回绕（那会算出一个错签名）。
// ctx 的 255 上限由 FIPS 204 的单字节长度决定，与实现无关。
//
// ============================================================================
// 【擦除】
// ============================================================================
// zeroize 拉起 → wiping 置位 → 把输入缓冲逐地址写 0 → wiping 落下。
// **逐地址写，不是靠复位** —— BRAM 的存储阵列不因复位清零（ram_dp 文件头也写了
// 这一条），靠复位"擦"等于没擦。
// ⚠️ 本期只擦 engine 自己的输入缓冲：三个核内部的存储（s₁/s₂/t₀、y、z、c、
//    以及它们自己的 sk/msg 缓冲）没有擦除口，要覆盖到它们必须给核加口，
//    那属于"动核内部"，与本期范围冲突。**这是一条已知缺口，写在这里免得
//    日后误以为已经覆盖。**
`default_nettype none

module mldsa_engine #(
    // ⚠️ 这一版三个核仍是**编译期参数化**的，所以 engine 也是。
    //    pset 端口存在且会被校验：与本次综合的参数集不符时拒绝启动并置 param_err，
    //    **绝不按错误的参数集算下去**（那会安静产生错误的密钥/签名）。
    //    运行时选参数集要等三个核的 K/L/η/τ/ω/β/c̃ 也改成运行时配置，见设计文档第 6 节。
    parameter integer K     = 4,
    parameter integer L     = 4,
    parameter integer ETA   = 2,
    parameter integer TAU   = 39,
    parameter integer G1LOG = 17,
    parameter integer MODE  = 0,
    parameter integer OMG   = 80,
    parameter integer BETA  = 78,
    parameter integer CTB   = 32,
    parameter integer PSET  = 0     // 0=44 1=65 2=87，用来校验 pset 端口
) (
    input  wire        clk,
    input  wire        rst_n,

    input  wire        zeroize,
    output wire        wiping,

    input  wire        start,          // 脉冲
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

    // ---- 长度（FIPS 204 表 2），由参数算出来，不写死 ----
    localparam integer PEB    = (ETA == 2) ? 96 : 128;      // η 打包每条字节数
    localparam integer PKLEN  = 32 + K*320;
    localparam integer SKLEN  = 128 + (L+K)*PEB + K*416;
    localparam integer ZB     = (G1LOG == 17) ? 576 : 640;  // z 每条字节数
    localparam integer SIGLEN = CTB + L*ZB + OMG + K;
    localparam integer MSGMAX = 8192;                       // 核里 u_msg 的容量

    localparam [14:0] SK_BASE  = 15'd0;
    localparam [14:0] RND_BASE = SK_BASE + SKLEN[14:0];
    localparam [14:0] CTX_BASE_S = RND_BASE + 15'd32;
    localparam [14:0] PK_BASE  = 15'd0;
    localparam [14:0] SIG_BASE = PK_BASE + PKLEN[14:0];
    localparam [14:0] CTX_BASE_V = SIG_BASE + SIGLEN[14:0];

    // ================= 输入字节缓冲（32 KB）=================
    // 空闲时接 AXI 的写口；跑起来之后由喂数 FSM 读。
    reg         ib_we;
    reg  [14:0] ib_waddr;
    reg  [7:0]  ib_wdata;
    reg  [14:0] ib_raddr;
    wire [7:0]  ib_rdata;
    ram_dp #(.DW(8), .AW(15)) u_ib (
        .clk(clk),
        .a_we(ib_we), .a_addr(ib_waddr), .a_din(ib_wdata), .a_dout(),
        .b_we(1'b0),  .b_addr(ib_raddr), .b_din(8'd0),     .b_dout(ib_rdata));

    // ================= 控制 =================
    localparam [3:0] S_IDLE = 4'd0, S_FEED_A = 4'd1, S_FEED_D = 4'd2,
                     S_GO   = 4'd3, S_RUN    = 4'd4, S_DONE   = 4'd5,
                     S_WIPE = 4'd6;
    reg [3:0]  state;
    reg [1:0]  op_r;
    reg [15:0] msg_r, ctx_r;
    reg [14:0] fidx;              // 喂数扫描指针
    reg        done_r, param_err;
    reg [14:0] wipe_addr;

    reg [255:0] xi_r, rnd_r;      // 两个要并行送进核的量
    reg         kg_start, sg_start, vf_start;

    // 本次运行需要喂多少字节
    wire [16:0] var_len  = {1'b0, ctx_r} + {1'b0, msg_r};
    wire [16:0] in_total =
        (op_r == OP_KEYGEN) ? 17'd32
      : (op_r == OP_SIGN)   ? ({2'd0, RND_BASE} + 17'd32 + var_len)
                            : ({2'd0, CTX_BASE_V} + var_len);

    // START 时的校验：参数集要对得上，消息不能超过核里 u_msg 的容量
    wire start_ok = (pset == PSET[1:0]) && (msg_len <= MSGMAX[15:0])
                    && (ctx_len <= 16'd255);

    assign wiping = (state == S_WIPE);
    assign busy   = (state != S_IDLE) && (state != S_DONE);
    assign done   = done_r;

    // ================= 三个核 =================
    // 各核的写口：由喂数 FSM 按段驱动
    reg        sk_we, msg_we_s, ctx_we_s;
    reg        pk_we, sig_we, msg_we_v, ctx_we_v;
    reg [12:0] seg_addr;   // 段内偏移（sk/pk/sig/msg 共用）
    reg [7:0]  seg_addr_c; // ctx 的段内偏移
    reg [7:0]  seg_data;

    wire        kg_done, sg_done, vf_done, vf_valid;
    wire [7:0]  kg_pk_data, kg_sk_data, sg_sig_data;

    // 输出读口的地址翻译（组合，out_addr 直接落到核的读口上）
    wire [12:0] kg_pk_addr = out_addr[12:0];
    wire [12:0] kg_sk_addr = out_addr[12:0] - PKLEN[12:0];
    wire [12:0] sg_sig_addr = out_addr[12:0];

    // ---- 海绵三选一 ----
    wire        kg_ss, kg_siv, kg_sif, kg_sor;
    wire [7:0]  kg_sr, kg_su, kg_sid;
    wire        sg_ss, sg_siv, sg_sif, sg_sor;
    wire [7:0]  sg_sr, sg_su, sg_sid;
    wire        vf_ss, vf_siv, vf_sif, vf_sor;
    wire [7:0]  vf_sr, vf_su, vf_sid;

    wire sel_kg = (op_r == OP_KEYGEN);
    wire sel_sg = (op_r == OP_SIGN);
    wire sel_vf = (op_r == OP_VERIFY);

    assign sha_start    = sel_kg ? kg_ss  : sel_sg ? sg_ss  : vf_ss;
    assign sha_rate     = sel_kg ? kg_sr  : sel_sg ? sg_sr  : vf_sr;
    assign sha_suffix   = sel_kg ? kg_su  : sel_sg ? sg_su  : vf_su;
    assign sha_in_valid = sel_kg ? kg_siv : sel_sg ? sg_siv : vf_siv;
    assign sha_in_data  = sel_kg ? kg_sid : sel_sg ? sg_sid : vf_sid;
    assign sha_in_flush = sel_kg ? kg_sif : sel_sg ? sg_sif : vf_sif;
    assign sha_out_ready= sel_kg ? kg_sor : sel_sg ? sg_sor : vf_sor;

    mldsa_keygen #(.K(K), .L(L), .ETA(ETA)) u_kg (
        .clk(clk), .rst_n(rst_n),
        .start(kg_start), .xi(xi_r), .done(kg_done),
        .sha_start(kg_ss), .sha_rate(kg_sr), .sha_suffix(kg_su),
        .sha_in_valid(kg_siv), .sha_in_data(kg_sid), .sha_in_flush(kg_sif),
        .sha_in_ready(sha_in_ready && sel_kg),
        .sha_out_valid(sha_out_valid && sel_kg),
        .sha_out_ready(kg_sor), .sha_out_data(sha_out_data),
        .rho(), .rho_prime(), .key_out(),
        .dbg_sel(6'd0), .dbg_idx(8'd0), .dbg_coef(),
        .pk_addr(kg_pk_addr), .pk_data(kg_pk_data),
        .sk_addr(kg_sk_addr), .sk_data(kg_sk_data));

    mldsa_sign #(.K(K), .L(L), .ETA(ETA), .TAU(TAU), .G1LOG(G1LOG),
                 .MODE(MODE), .OMG(OMG), .BETA(BETA), .CTB(CTB)) u_sg (
        .clk(clk), .rst_n(rst_n),
        .start(sg_start),
        .sk_wr_en(sk_we),  .sk_wr_addr(seg_addr),  .sk_wr_data(seg_data),
        .msg_wr_en(msg_we_s), .msg_wr_addr(seg_addr), .msg_wr_data(seg_data),
        .ctx_wr_en(ctx_we_s), .ctx_wr_addr(seg_addr_c), .ctx_wr_data(seg_data),
        .msg_len(msg_r[13:0]), .ctx_len(ctx_r[7:0]), .rnd(rnd_r),
        .done(sg_done),
        .sha_start(sg_ss), .sha_rate(sg_sr), .sha_suffix(sg_su),
        .sha_in_valid(sg_siv), .sha_in_data(sg_sid), .sha_in_flush(sg_sif),
        .sha_in_ready(sha_in_ready && sel_sg),
        .sha_out_valid(sha_out_valid && sel_sg),
        .sha_out_ready(sg_sor), .sha_out_data(sha_out_data),
        .rho(), .key_out(), .tr_out(), .mu(), .rhopp(), .ctilde(),
        .dbg_sel(7'd0), .dbg_idx(8'd0), .dbg_coef(),
        .sig_addr(sg_sig_addr), .sig_data(sg_sig_data));

    mldsa_verify #(.K(K), .L(L), .TAU(TAU), .G1LOG(G1LOG),
                   .MODE(MODE), .OMG(OMG), .BETA(BETA), .CTB(CTB)) u_vf (
        .clk(clk), .rst_n(rst_n),
        .start(vf_start),
        .pk_wr_en(pk_we),   .pk_wr_addr(seg_addr),  .pk_wr_data(seg_data),
        .sig_wr_en(sig_we), .sig_wr_addr(seg_addr), .sig_wr_data(seg_data),
        .msg_wr_en(msg_we_v), .msg_wr_addr(seg_addr), .msg_wr_data(seg_data),
        .ctx_wr_en(ctx_we_v), .ctx_wr_addr(seg_addr_c), .ctx_wr_data(seg_data),
        .msg_len(msg_r[13:0]), .ctx_len(ctx_r[7:0]),
        .done(vf_done), .valid(vf_valid),
        .sha_start(vf_ss), .sha_rate(vf_sr), .sha_suffix(vf_su),
        .sha_in_valid(vf_siv), .sha_in_data(vf_sid), .sha_in_flush(vf_sif),
        .sha_in_ready(sha_in_ready && sel_vf),
        .sha_out_valid(sha_out_valid && sel_vf),
        .sha_out_ready(vf_sor), .sha_out_data(sha_out_data),
        .ctilde(), .ctilde_p(), .tr_out(), .mu(), .zbad(), .hbad(),
        .dbg_sel(7'd0), .dbg_idx(8'd0), .dbg_coef());

    // ---- 出口 ----
    assign out_data = (op_r == OP_KEYGEN)
                        ? ((out_addr < PKLEN[14:0]) ? kg_pk_data : kg_sk_data)
                        : sg_sig_data;
    assign out_len  = (op_r == OP_KEYGEN) ? (PKLEN[15:0] + SKLEN[15:0])
                    : (op_r == OP_SIGN)   ? SIGLEN[15:0] : 16'd0;
    assign verify_ok = vf_valid && !param_err;

    // ================= 缓冲写口与喂数的段译码 =================
    // 空闲时缓冲的写口接 AXI；擦除时接擦除机。
    always @(*) begin
        if (state == S_WIPE) begin
            ib_we = 1'b1; ib_waddr = wipe_addr; ib_wdata = 8'd0;
        end else begin
            ib_we = in_we && (state == S_IDLE);
            ib_waddr = in_addr; ib_wdata = in_data;
        end
    end

    // 段边界（按 op）。fidx 是缓冲区里的绝对地址。
    wire [14:0] ctx_base = (op_r == OP_SIGN) ? CTX_BASE_S : CTX_BASE_V;
    wire [14:0] msg_base = ctx_base + ctx_r[14:0];
    wire in_sk  = (op_r == OP_SIGN)   && (fidx < RND_BASE);
    wire in_rnd = (op_r == OP_SIGN)   && (fidx >= RND_BASE) && (fidx < CTX_BASE_S);
    wire in_pk  = (op_r == OP_VERIFY) && (fidx < SIG_BASE);
    wire in_sig = (op_r == OP_VERIFY) && (fidx >= SIG_BASE) && (fidx < CTX_BASE_V);
    wire in_ctx = (op_r != OP_KEYGEN) && (fidx >= ctx_base) && (fidx < msg_base);
    wire in_msg = (op_r != OP_KEYGEN) && (fidx >= msg_base);

    always @(*) begin
        sk_we = 1'b0; msg_we_s = 1'b0; ctx_we_s = 1'b0;
        pk_we = 1'b0; sig_we = 1'b0; msg_we_v = 1'b0; ctx_we_v = 1'b0;
        seg_addr = 13'd0; seg_addr_c = 8'd0; seg_data = ib_rdata;
        if (state == S_FEED_D) begin
            if (in_sk) begin
                sk_we = 1'b1; seg_addr = fidx[12:0];
            end else if (in_pk) begin
                pk_we = 1'b1; seg_addr = fidx[12:0];
            end else if (in_sig) begin
                // 段内偏移必然 < 该段长度，显式取低位（差值算满 15 位）
                sig_we = 1'b1; seg_addr = fidx[12:0] - SIG_BASE[12:0];
            end else if (in_ctx) begin
                seg_addr_c = fidx[7:0] - ctx_base[7:0];
                ctx_we_s = (op_r == OP_SIGN);
                ctx_we_v = (op_r == OP_VERIFY);
            end else if (in_msg) begin
                seg_addr = fidx[12:0] - msg_base[12:0];
                msg_we_s = (op_r == OP_SIGN);
                msg_we_v = (op_r == OP_VERIFY);
            end
        end
    end

    // ================= 主状态机 =================
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= S_IDLE; op_r <= 2'd0; msg_r <= 16'd0; ctx_r <= 16'd0;
            fidx <= 15'd0; done_r <= 1'b0; param_err <= 1'b0;
            xi_r <= 256'd0; rnd_r <= 256'd0;
            kg_start <= 1'b0; sg_start <= 1'b0; vf_start <= 1'b0;
            wipe_addr <= 15'd0;
        end else begin
            kg_start <= 1'b0; sg_start <= 1'b0; vf_start <= 1'b0;

            // 擦除优先于一切：拉起就进擦除态（正在跑的运算被放弃）
            if (zeroize && (state != S_WIPE)) begin
                state <= S_WIPE; wipe_addr <= 15'd0; done_r <= 1'b0;
            end else begin
                case (state)
                S_IDLE: if (start) begin
                    if (!start_ok) begin
                        // 参数集对不上或长度越界：置错并**立刻完成**，不启动任何核。
                        param_err <= 1'b1; done_r <= 1'b1;
                    end else begin
                        param_err <= 1'b0; done_r <= 1'b0;
                        op_r  <= op; msg_r <= msg_len; ctx_r <= ctx_len;
                        fidx  <= 15'd0;
                        state <= S_FEED_A;
                    end
                end

                // 逐字节把缓冲翻译到核的原生口：摆地址 / 写数据，两拍一个字节
                S_FEED_A: state <= S_FEED_D;
                S_FEED_D: begin
                    // ξ 与 rnd 是并行口，边扫边装进寄存器
                    if ((op_r == OP_KEYGEN) && (fidx < 15'd32))
                        xi_r[fidx[4:0]*8 +: 8] <= ib_rdata;
                    if (in_rnd)
                        rnd_r[(fidx - RND_BASE)*8 +: 8] <= ib_rdata;

                    if ({2'd0, fidx} + 17'd1 >= in_total) state <= S_GO;
                    else begin fidx <= fidx + 15'd1; state <= S_FEED_A; end
                end

                S_GO: begin
                    case (op_r)
                        OP_KEYGEN: kg_start <= 1'b1;
                        OP_SIGN:   sg_start <= 1'b1;
                        default:   vf_start <= 1'b1;
                    endcase
                    state <= S_RUN;
                end

                S_RUN: begin
                    if ((op_r == OP_KEYGEN && kg_done) ||
                        (op_r == OP_SIGN   && sg_done) ||
                        (op_r == OP_VERIFY && vf_done)) state <= S_DONE;
                end

                // done 是**电平**，保持到下一次 start（与三个核、与替身一致）
                S_DONE: begin done_r <= 1'b1; state <= S_IDLE; end

                // 逐地址写 0。BRAM 不因复位清零，所以必须真写一遍。
                S_WIPE: begin
                    if (wipe_addr == 15'h7FFF) begin
                        state <= S_IDLE;
                        xi_r <= 256'd0; rnd_r <= 256'd0;
                    end else begin
                        wipe_addr <= wipe_addr + 15'd1;
                    end
                end

                default: state <= S_IDLE;
                endcase
            end
        end
    end

    // 缓冲的读地址一直跟着 fidx（同步读，S_FEED_A 摆地址、S_FEED_D 拿数据）
    always @(*) ib_raddr = fidx;

endmodule

`default_nettype wire
