// mldsa_axi —— ML-DSA（FIPS 204）共享引擎的 AXI4-Lite 从机
//
//   s_axi ──▶ axi4lite_firewall ──▶ 寄存器组 ──▶ mldsa_engine ──▶ sha3_core
//              AxPROT/窗口/tamper                  │  ▲
//                                    片内私钥金库 ─┘  └── 输出（pk / sig）
//
// 这一层与 mlkem_axi 是同一个范式（同样的防火墙前置、同样的字节流进出、
// 同样的 START 前校验、同样的私钥金库与一次性闩锁），差别只有两处，都是被
// engine 的接口逼出来的：
//
//   ① **输入缓冲不在这一层**。三个 ML-KEM 核吃的是字节流，所以 mlkem_axi
//      自己拿一块 8 KB BRAM 存着再喂；而 mldsa_engine 给的是一个存储写口
//      （in_we/in_addr/in_data），缓冲区在它里面。本层再放一块就是把同一份
//      输入存两遍（32 KB = 8 片 BRAM36），没有任何好处。所以 IN_DATA 是
//      **直写**：一笔 AXI 写就是一次 engine 输入存储的写。
//   ② **输出缓冲也不在这一层**（out_addr → out_data，同步读，一拍延迟）。
//      于是"哪些字节软件读得到"不能再靠"往不往输出缓冲里写"来实现，
//      改成靠 OUT_LEN 卡住读指针 —— 见下面【sk 怎么做到不出总线】。
//
// ============================================================================
// 【输入缓冲的排布 —— 这是与 engine 那条线共用的一份契约】
// ============================================================================
// 一切都往 IN_DATA 里灌，顺序固定（engine 侧照同一份实现）：
//
//   KeyGen : ξ(32)
//   Sign   : [sk(2560/4032/4896)，仅当 SK_FROM_SLOT=0] ‖ rnd(32)
//            ‖ ctx(CTX_LEN) ‖ msg(MSG_LEN)
//   Verify : pk(1312/1952/2592) ‖ sig(2420/3309/4627) ‖ ctx(CTX_LEN) ‖ msg(MSG_LEN)
//
// 两条要点：
//   · **rnd 永远在流里**（32 字节，紧跟 sk）。确定性签名就写 32 个零 ——
//     不另开 mode 位。ACVP 的 siggen 确定性条目正是 rnd = 0³²，没有这个入口
//     就没法对固定期望值验签名（hedged 每次取随机数，签出来必然对不上）。
//   · ctx 在 msg 之前，与 FIPS 204 的 M′ = 0x00 ‖ |ctx| ‖ ctx ‖ M 同序；
//     CTX_LEN 可以是 0（PKCS#11 那条路永远是 0，但 ACVP 里有非空 ctx）。
//
// **engine 看到的永远是完整的一份**：SK_FROM_SLOT=1 时软件不送 sk，本层把
// 金库里的 sk 写进 engine 输入存储的 [0, skLen)，软件后面送的 rnd‖ctx‖msg
// 顺延到 skLen 之后。也就是说两条路（软件自带 sk / 金库供 sk）在 engine 眼里
// 是**逐字节相同的一份排布**，engine 不需要知道 sk 是谁送的。
//
// ⚠️ 由此带来一个必须挡住的顺序陷阱：sk 排在最前，于是"软件写的字节落在
//    engine 的哪个地址"**取决于 MODE**（SK_FROM_SLOT 与 PSET）。先灌字节
//    再改 MODE 的话，那些字节就按另一份排布被解读了 —— 而且**毫无痕迹**。
//    所以这里定死一条：**写 MODE 就把 IN_PTR 清零**。
//    先灌后改 MODE 的写法于是会在 START 处报欠填（LEN_ERR），
//    而不是安静地算出一个错东西。宁可吵，不可静。
//    正常顺序本来就是 MODE → CLEAR → 灌字节 → START。
//
// ============================================================================
// 【START 前的校验：喂不够就不许启动】
// ============================================================================
// 长度全部由 PSET 算出来，软件不用报长度，也就不存在"报错长度"这种会安静
// 产生错误结果的输入方式。但"不用报长度"不等于"不用喂够"：
//
//   软件只写了一半的 sk 就 START，剩下那一段取的是输入存储里的**残留**
//   （冷启动是全 0，连着跑就是上一次运算的字节）。ML-DSA 的 sk 里有 K 与
//   s₁/s₂ —— 用残留当私钥签出来的签名，**verify 一样会过**（它是用这份坏 sk
//   对应的 pk 验的），而 ACVP 那种"逐字节对官方向量"的用例根本喂得满，
//   永远碰不到这条路。也就是说这个错误既不崩也不报，只是私钥不是你以为的
//   那一把。所以在 START 那一刻就判掉，并且**不启动 engine**。
//
// 与 mlkem_axi 同一个口径：不启动比报错更要紧 —— 启动了再报错的话，engine
// 已经按一个错的输入开始算了。
//
// ============================================================================
// 【片内私钥金库：sk 一个字节都不越过 AXI】
// ============================================================================
// 与 mlkem_axi 的 dk 金库是同一件事，理由也同一条：不这么做的话"私钥不出
// 硬件"这句话不成立 —— sk 会实实在在地越过 AXI 边界躺在 DDR 里。
//
//   · KeyGen 时 SK_TO_SLOT（或 sk_lock）置上：engine 吐出的 pk‖sk 里，
//     sk 那一段**不经过 OUT_DATA**，由本层从 engine 的输出口直接搬进金库；
//     软件拿到的只有 pk 和槽号。
//   · Sign 时 SK_FROM_SLOT 置上：sk 从金库搬进 engine 的输入口，软件只送
//     rnd‖ctx‖msg。
//
// 【8 个槽、每槽 8192 字节】
// sk 最大 4896 字节（ML-DSA-87），跨度取 8192 是为了寻址就是拼接
// （{slot, offset}），不用乘法器。8×8192 = 64 KB = 16 片 BRAM36。
// 槽数取 8 而不是 ML-KEM 那边的 16：签名密钥的用量形态和 KEM 不一样
// （PKCS#11 那边连续生成很多把 KEM 密钥的是封装，签名密钥是长期的），
// 8 个槽 + 64 KB 是这一版的取舍，不够了再加是同样的一行改动。
//
// Sign 那一趟软件送的是 rnd‖ctx‖msg（不含 sk），所以 START 的长度门槛也跟着
// 降到 32+CTX_LEN+MSG_LEN —— 否则一个完全正确的调用会被判成喂不够。
//
// 【sk 怎么做到不出总线 —— 这一层没有输出缓冲，所以判据换了地方】
// mlkem_axi 是靠"dk 那一段根本不往输出缓冲里写"。本层的输出缓冲在 engine
// 里，本层不能不让它写，于是改成**卡读指针**：
//   · OUT_LEN 对软件报的是 pk 的长度（不是 engine 的 out_len）；
//   · 读 OUT_DATA 时 out_ptr 必须 < OUT_LEN，否则不给字节、也不推指针；
//   · OUT_PTR 可以随便写（它只是个读游标），但读的时候仍然被 OUT_LEN 卡住。
// 也就是说 sk 那一段的地址**从来不会被摆到 engine 的 out_addr 上**（除了
// 本层自己搬进金库那一趟，那一趟 BUSY=1、OUT_LEN=0，软件读不到任何东西）。
//
// 【sk_lock 是一次性闩锁】
// 置上之后 KeyGen **再也不会**把 sk 送到 OUT_DATA，无论 MODE 里怎么写。
// CTRL 里没有清它的位，ZEROIZE 也不清它 —— zeroize 是"把秘密擦掉"，不是
// "把防线撤掉"，两件事。只有重新装载位流（复位整块 PL）能解开。
//
// 留一个能关掉的开关是有意的：ACVP 的 KeyGen 向量要核对 sk，那是出厂验证
// 必须做的事。但它必须是**一次性的方向** —— 演示与交付时把闩锁一置，
// "私钥出不来"就从一句承诺变成了硬件性质。与 ML-KEM 完全同一个口径。
//
// ⚠️ 闩锁**只管 KeyGen 这个方向**，不强迫 Sign 走金库 —— 与 mlkem_axi 的
//    dk_lock 只管 KeyGen、不管 Decaps 是同一条理由：软件自己送进来的 sk
//    本来就已经在硬件外面了，拦它并不能把它变回片内，只会让"用外部私钥签名"
//    这个合法用法在闩锁之后彻底不可用。闩锁守的是**出口**。
//
// ============================================================================
// 【擦除：本层只保证擦掉自己拥有的东西】
// ============================================================================
// ZEROIZE / tamper 的上升沿启动一台擦除机，把 64 KB 的金库逐地址写 0
// （65536 拍），期间 WIPING=1、拒绝写、拒绝启动。理由与 mlkem_axi 那段
// 一模一样：只清指针与有效位是"把目录页撕了而正文还在"。
//
// engine 自己的存储由**它自己那台擦除机**负责：本层把 zeroize 转给它，它擦完
// 把 wiping 落下。两台擦除机要**一起等完**（wiping_any），只等本层那台会在
// engine 还没擦干净时就放行启动与读出。
//
// ⚠️ 这里以前是一条已知缺口：engine 当时没有 zeroize 口，本层只能把它按在
//    复位上（`rst_n && !zeroize_all`）—— 而**复位擦不掉 BRAM**，那种写法只是
//    看起来像擦了，擦除之后 engine 存储里仍有上一次的 sk 字节。现在 engine
//    有了真擦除口，rst_n 也就改回纯复位：有了真的擦法就不该再拿复位充数。
//
// 同一条理由也用在海绵上：`sha3_core` **本来就带 zeroize 口**，却一直接的
//    1'b0、靠复位擦。它的状态里过过 sk 的派生量（ρ′/K/μ 那一路），所以现在
//    接真 zeroize。
`default_nettype none

module mldsa_axi #(
    parameter [31:0]  VERSION     = 32'h0001_0000,
    parameter integer SECURE_ONLY = 1
) (
    input  wire        clk,
    input  wire        rst_n,

    // ---- AXI4-Lite 从机 ----
    input  wire [7:0]  s_axi_awaddr,
    input  wire [2:0]  s_axi_awprot,
    input  wire        s_axi_awvalid,
    output wire        s_axi_awready,
    input  wire [31:0] s_axi_wdata,
    input  wire [3:0]  s_axi_wstrb,
    input  wire        s_axi_wvalid,
    output wire        s_axi_wready,
    output wire [1:0]  s_axi_bresp,
    output wire        s_axi_bvalid,
    input  wire        s_axi_bready,

    input  wire [7:0]  s_axi_araddr,
    input  wire [2:0]  s_axi_arprot,
    input  wire        s_axi_arvalid,
    output wire        s_axi_arready,
    output wire [31:0] s_axi_rdata,
    output wire [1:0]  s_axi_rresp,
    output wire        s_axi_rvalid,
    input  wire        s_axi_rready,

    input  wire        tamper
);
    localparam [1:0] RESP_OKAY = 2'b00, RESP_SLVERR = 2'b10;

    // 寄存器映射（槽内偏移；与 mlkem_axi 一样用 addr[5:2] 译码，共 16 个）
    //   0x00 VERSION  R
    //   0x04 CTRL     W   [0]START [1]CLEAR [2]ZEROIZE [4]SK_LOCK
    //   0x08 MODE     RW  [1:0]OP [3:2]PSET [4]SK_TO_SLOT [5]SK_FROM_SLOT [9:6]SLOT
    //   0x0C STATUS   R   [0]busy [1]done [2]verify_ok [3]param_err [4]len_err
    //                     [5]tamper [6]wiping
    //   0x10 IN_DATA  W   0x14 IN_PTR  RW
    //   0x18 OUT_DATA R   0x1C OUT_PTR RW   0x20 OUT_LEN R
    //   0x24 MSG_LEN  RW  0x28 CTX_LEN RW
    //   0x2C KEYSTAT  R   0x30 VIOL    R
    localparam [3:0] A_VERSION = 4'h0, A_CTRL    = 4'h1, A_MODE   = 4'h2,
                     A_STATUS  = 4'h3, A_INDATA  = 4'h4, A_INPTR  = 4'h5,
                     A_OUTDATA = 4'h6, A_OUTPTR  = 4'h7, A_OUTLEN = 4'h8,
                     A_MSGLEN  = 4'h9, A_CTXLEN  = 4'hA, A_KEYSTAT = 4'hB,
                     A_VIOL    = 4'hC;

    localparam [1:0] OP_KEYGEN = 2'd0, OP_SIGN = 2'd1, OP_VERIFY = 2'd2;

    // engine 的输入存储深度（in_addr 是 15 位）
    localparam [17:0] IN_CAP = 18'd32768;

    // ================= 防火墙 =================
    wire [7:0]  f_awaddr;  wire [2:0] f_awprot;
    wire        f_awvalid, f_awready;
    wire [31:0] f_wdata;   wire [3:0] f_wstrb;
    wire        f_wvalid,  f_wready;
    reg  [1:0]  f_bresp;   reg        f_bvalid;   wire f_bready;
    wire [7:0]  f_araddr;  wire [2:0] f_arprot;
    wire        f_arvalid, f_arready;
    reg  [31:0] f_rdata;   reg  [1:0] f_rresp;    reg  f_rvalid;  wire f_rready;
    wire [15:0] viol_wr_count, viol_rd_count;
    wire        fw_tampered;

    axi4lite_firewall #(
        .AW(8), .SECURE_ONLY(SECURE_ONLY), .PRIV_ONLY(0),
        .ALLOW_WRITE(1), .ALLOW_READ(1),
        .ADDR_BASE(32'h0000_0000), .ADDR_MASK(32'h0000_00C0)
    ) u_fw (
        .clk(clk), .rst_n(rst_n), .tamper(tamper),
        .s_awaddr(s_axi_awaddr), .s_awprot(s_axi_awprot),
        .s_awvalid(s_axi_awvalid), .s_awready(s_axi_awready),
        .s_wdata(s_axi_wdata), .s_wstrb(s_axi_wstrb),
        .s_wvalid(s_axi_wvalid), .s_wready(s_axi_wready),
        .s_bresp(s_axi_bresp), .s_bvalid(s_axi_bvalid), .s_bready(s_axi_bready),
        .s_araddr(s_axi_araddr), .s_arprot(s_axi_arprot),
        .s_arvalid(s_axi_arvalid), .s_arready(s_axi_arready),
        .s_rdata(s_axi_rdata), .s_rresp(s_axi_rresp),
        .s_rvalid(s_axi_rvalid), .s_rready(s_axi_rready),
        .m_awaddr(f_awaddr), .m_awprot(f_awprot),
        .m_awvalid(f_awvalid), .m_awready(f_awready),
        .m_wdata(f_wdata), .m_wstrb(f_wstrb),
        .m_wvalid(f_wvalid), .m_wready(f_wready),
        .m_bresp(f_bresp), .m_bvalid(f_bvalid), .m_bready(f_bready),
        .m_araddr(f_araddr), .m_arprot(f_arprot),
        .m_arvalid(f_arvalid), .m_arready(f_arready),
        .m_rdata(f_rdata), .m_rresp(f_rresp),
        .m_rvalid(f_rvalid), .m_rready(f_rready),
        .viol_wr_count(viol_wr_count), .viol_rd_count(viol_rd_count),
        .viol_first_valid(), .viol_first_addr(), .viol_first_prot(),
        .viol_first_is_write(), .tamper_latched(fw_tampered));

    // ================= 片内私钥金库 =================
    reg         skv_we;   reg [15:0] skv_waddr; reg [7:0] skv_din;
    reg  [15:0] skv_raddr; wire [7:0] skv_dout;
    ram_dp #(.DW(8), .AW(16)) u_skvault (
        .clk(clk),
        .a_we(skv_we), .a_addr(skv_waddr), .a_din(skv_din), .a_dout(),
        .b_we(1'b0),   .b_addr(skv_raddr), .b_din(8'd0),    .b_dout(skv_dout));

    // 每个槽：有没有装东西 + 装的是哪个参数集。
    // pset 必须跟着存 —— Sign 的 sk 长度由它算，软件报一个和存进去时不同的
    // pset，搬进 engine 的字节数就全错，而错法是"签出来一个看起来合法、
    // 但用的私钥不对"的安静错误。所以在 START 那一刻就判掉。
    reg [7:0]  slot_valid;
    reg [15:0] slot_pset;     // 每槽 2 位，8 槽正好 16 位

    // 一次性闩锁：写 1 置上，**没有清零路径**（见文件头）
    reg        sk_lock;

    // ================= 控制寄存器 =================
    reg [1:0]  op, pset;
    reg        sk_to_slot, sk_from_slot;
    reg [3:0]  slot;
    reg [15:0] in_ptr, out_ptr, out_len_r, msg_len, ctx_len;
    reg        run_done, verify_ok_r, param_err, len_err;
    reg        zero_pulse;

    // ---- 金库擦除机 ----
    reg        wiping;
    reg [15:0] wipe_addr;
    reg        zall_d;
    wire       zeroize_all = zero_pulse || tamper || fw_tampered;
    // engine 自己的擦除进度（它内部也有 sk 派生量）。**两台擦除机要一起等完**：
    // 只等本层那台，会在 engine 还没擦干净时就放行启动/读出。
    wire       eng_wiping;
    wire       wiping_any = wiping || eng_wiping;

    // ================= 由 PSET 算出来的长度 =================
    // FIPS 204 表 2。这些是常数，直接查表 —— 用移位加法凑出来只会更难核对。
    wire [12:0] pk_len  = (pset == 2'd0) ? 13'd1312
                        : (pset == 2'd1) ? 13'd1952 : 13'd2592;
    wire [12:0] sk_len  = (pset == 2'd0) ? 13'd2560
                        : (pset == 2'd1) ? 13'd4032 : 13'd4896;
    wire [12:0] sig_len = (pset == 2'd0) ? 13'd2420
                        : (pset == 2'd1) ? 13'd3309 : 13'd4627;

    // 非法参数：op / pset 各 2 位而只有 0/1/2 有意义；SLOT 4 位而只有 8 个槽。
    // 值 3（或槽 8..15）不是"另一种配置"，是一个不存在的东西 —— 长度会按
    // pset==2 那条分支算、op 会落到 default，于是 engine 收到一份对不上号的
    // 输入。软件看到的是 BUSY 一直不落，与"算得慢"分不开。
    wire op_ok = (op == OP_KEYGEN) || (op == OP_SIGN) || (op == OP_VERIFY);
    wire params_ok = op_ok && (pset != 2'd3) && (slot < 4'd8)
                     // FIPS 204：|ctx| ≤ 255。超了在这里判，不要送进 engine
                     && (ctx_len <= 16'd255);

    // 本次要从金库取 sk / 把 sk 收进金库
    wire take_sk  = (op == OP_SIGN)   && sk_from_slot;
    wire store_sk = (op == OP_KEYGEN) && (sk_to_slot || sk_lock);

    // ---- 软件必须写够多少字节（见文件头【START 前的校验】）----
    //   KeyGen : ξ(32)
    //   Sign   : rnd(32) + ctx + msg，再加 sk（**除非从金库取**）
    //   Verify : pk + sig + ctx + msg
    wire [17:0] var_len = {2'd0, ctx_len} + {2'd0, msg_len};
    wire [17:0] need_sw = (op == OP_KEYGEN) ? 18'd32
                        : (op == OP_SIGN)   ? (18'd32 + var_len
                                               + (take_sk ? 18'd0 : {5'd0, sk_len}))
                        :                     (var_len + {5'd0, pk_len}
                                               + {5'd0, sig_len});
    // engine 里实际要占多少字节（金库供的 sk 也占地方）
    wire [17:0] need_tot = need_sw + (take_sk ? {5'd0, sk_len} : 18'd0);

    wire len_ok  = ({2'd0, in_ptr} >= need_sw);
    wire cap_ok  = (need_tot <= IN_CAP);

    // 从金库取 sk 还要求：那个槽真的装了东西，而且装的时候是同一个参数集
    wire [1:0] this_slot_pset = slot_pset[slot[2:0]*2 +: 2];
    wire       slot_ok = !take_sk
                         || (slot_valid[slot[2:0]] && (this_slot_pset == pset));

    // ================= engine 与它自带的 SHA-3 =================
    // ⚠️ ML-DSA **自带一份 sha3_core**，不与 ML-KEM 那边共享。跨从机仲裁会动到
    //    已经验证过的安全边界（两个从机的 tamper/zeroize 会互相牵连），这一轮
    //    不做；面积上 ZU3EG 有余量。
    reg         eng_start;
    reg         in_we;
    reg  [14:0] in_addr;
    reg  [7:0]  in_data;
    reg  [14:0] out_addr;
    wire        eng_busy, eng_done, eng_vok;
    wire [7:0]  out_data;
    wire [15:0] eng_out_len;

    wire        sha_start, sha_in_valid, sha_in_flush, sha_out_ready;
    wire [7:0]  sha_rate, sha_suffix, sha_in_data;
    wire        sha_in_ready, sha_out_valid;
    wire [7:0]  sha_out_data;

    // ⚠️ rst_n 是**纯复位**，不再 `&& !zeroize_all`：有了真的 zeroize 口之后
    //    就不该再拿复位当擦除用 —— 复位本来也擦不掉 BRAM，那种写法只是看起来
    //    像擦了。擦除靠 zeroize，engine 擦完把 wiping 落下。
    mldsa_engine u_eng (
        .clk(clk), .rst_n(rst_n),
        .zeroize(zeroize_all), .wiping(eng_wiping),
        .start(eng_start), .op(op), .pset(pset),
        .busy(eng_busy), .done(eng_done), .verify_ok(eng_vok),
        .in_we(in_we), .in_addr(in_addr), .in_data(in_data),
        .msg_len(msg_len), .ctx_len(ctx_len),
        .out_addr(out_addr), .out_data(out_data), .out_len(eng_out_len),
        .sha_start(sha_start), .sha_rate(sha_rate), .sha_suffix(sha_suffix),
        .sha_in_valid(sha_in_valid), .sha_in_data(sha_in_data),
        .sha_in_flush(sha_in_flush), .sha_in_ready(sha_in_ready),
        .sha_out_valid(sha_out_valid), .sha_out_data(sha_out_data),
        .sha_out_ready(sha_out_ready));

    // 海绵也接真 zeroize：它的状态里过过 sk 派生量（ρ′/K/μ 那一路），
    // 原来接 1'b0 而靠复位擦是不够的 —— 这个核自己就带擦除口，用它。
    sha3_core u_sha (
        .clk(clk), .rst_n(rst_n),
        .rate_bytes(sha_rate), .suffix(sha_suffix),
        .start(sha_start), .zeroize(zeroize_all),
        .in_valid(sha_in_valid), .in_ready(sha_in_ready),
        .in_data(sha_in_data), .in_flush(sha_in_flush),
        .out_valid(sha_out_valid), .out_ready(sha_out_ready),
        .out_data(sha_out_data),
        .busy(), .absorbing(), .squeezing(),
        // 直通口不引出来：那是 pqc_accel_axi 借置换核用的，这里借出去等于
        // 把海绵的中间状态摆到总线上。
        .ext_start(1'b0), .ext_done(),
        .ext_wr_en(1'b0), .ext_wr_addr(5'd0), .ext_wr_data(64'd0),
        .ext_rd_addr(5'd0), .ext_rd_data());

    // ================= 状态机 =================
    //   S_LOAD  金库 → engine 输入（SK_FROM_SLOT）
    //   S_KICK  拉 start
    //   S_RUN   等 done
    //   S_STORE engine 输出 → 金库（SK_TO_SLOT / sk_lock）
    //   S_FIN   落 OUT_LEN、标槽有效、报 DONE
    localparam [2:0] S_IDLE = 3'd0, S_LOAD = 3'd1, S_KICK = 3'd2,
                     S_RUN  = 3'd3, S_STORE = 3'd4, S_FIN = 3'd5;
    reg [2:0]  state;
    reg [12:0] cp;          // 搬运计数（最大 sk 4896）
    reg        cp_wait;     // 同步读要等一拍
    // start 是非阻塞赋值，下一拍才真正拉高；而 engine 的 done 是**电平**，
    // 保持到下一次 start 才清。所以进 S_RUN 的头几拍绝不能看 eng_done ——
    // 看了就会读到上一次运行残留的 done，当场结束、OUT_LEN 是 0。
    // 表现是**第一次永远对、第二次必错**，ML-KEM 那边是在真硅上才暴露的
    // （见 mlkem_axi 的 kickdly）。这里照抄，连同那条用例一起。
    reg [1:0]  kickdly;

    // 软件写的字节落在 engine 输入存储的哪里：SK_FROM_SLOT 那一趟，前面
    // skLen 个字节是金库供的 sk，软件的 rnd‖ctx‖msg 顺延到它后面。
    // 这个偏移取决于 MODE —— 所以写 MODE 会把 IN_PTR 清零（见文件头那条⚠️）。
    wire [14:0] sw_base = take_sk ? {2'd0, sk_len} : 15'd0;
    wire [14:0] sw_addr = in_ptr[14:0] + sw_base;
    // 金库供的 sk 就摆在最前面
    wire [14:0] sk_base = 15'd0;

    // ================= 写通道 =================
    reg aw_got, w_got;
    reg [7:0]  aw_addr_r;
    reg [31:0] w_data_r;
    reg [3:0]  w_strb_r;

    assign f_awready = !aw_got && !f_bvalid;
    assign f_wready  = !w_got  && !f_bvalid;

    wire wr_now = (aw_got || (f_awvalid && f_awready))
                  && (w_got || (f_wvalid && f_wready)) && !f_bvalid;
    wire [7:0]  wr_addr = (f_awvalid && f_awready) ? f_awaddr : aw_addr_r;
    wire [31:0] wr_data = (f_wvalid  && f_wready)  ? f_wdata  : w_data_r;
    wire [3:0]  wr_strb = (f_wvalid  && f_wready)  ? f_wstrb  : w_strb_r;

    // 输入存储写满了就不再收：不加这条的话 in_ptr 会绕回 0 覆盖已经写好的
    // 前半段，而软件看到的是一路 OKAY。
    wire in_full = ({2'd0, in_ptr} >= IN_CAP);
    wire wr_indata = wr_now && wr_strb[0] && (wr_addr[5:2] == A_INDATA)
                     && (state == S_IDLE) && !wiping_any && !in_full;
    // IN_PTR 只接受写 0。**不给"任意设置写指针"这个能力**是有意的：
    // 那等于给了一条绕过喂够校验的路（把指针推到需要的长度，实际字节是残留），
    // 而这正是上面那条校验要挡的东西。非零的写回 SLVERR，不是静默忽略。
    wire wr_inptr_bad = wr_now && wr_strb[0] && (wr_addr[5:2] == A_INPTR)
                        && (wr_data != 32'd0);

    // ---- 运行期间：参数寄存器与 IN_DATA 是只读的 ----
    // MODE / MSG_LEN / CTX_LEN 在一次运行的中途被改掉的后果是**换前提**：
    // 长度、槽号、sk 的去向全跟着变 —— 搬 sk 搬到一半跳去另一个槽、
    // 或者按新 pset 去算 OUT_LEN。IN_DATA 则是白写（engine 的输入口这时
    // 归搬运用）。
    //
    // 两者都**明确回 SLVERR**。静默丢弃是这里最危险的选项：软件会以为参数改了、
    // 字节灌进去了，接着按一个错误的前提往下走 —— 与擦除期间拒写同一条理由。
    // CTRL 不在此列：CLEAR / ZEROIZE / SK_LOCK 在运行途中本来就该能写。
    wire wr_busy_reject = wr_now && wr_strb[0] && (state != S_IDLE)
                          && ((wr_addr[5:2] == A_MODE)
                              || (wr_addr[5:2] == A_MSGLEN)
                              || (wr_addr[5:2] == A_CTXLEN)
                              || (wr_addr[5:2] == A_INDATA));

    // ================= 读通道 =================
    assign f_arready = !f_rvalid;
    // 读 OUT_DATA 的唯一闸门：out_ptr < OUT_LEN。存 sk 那一趟 OUT_LEN 只到
    // pk 的长度，于是 sk 那一段的地址根本不会被摆到 engine 的 out_addr 上。
    wire rd_outdata = f_arvalid && f_arready && (f_araddr[5:2] == A_OUTDATA)
                      && !wiping_any && (out_ptr < out_len_r);

    wire [31:0] r_status = {25'd0, wiping_any, fw_tampered,
                            len_err, param_err, verify_ok_r,
                            (state == S_IDLE) && run_done, (state != S_IDLE)};

    // KEYSTAT：[7:0] 哪些槽装了东西；[8] 私钥外泄闩锁；
    //          [31:16] 每槽 2 位的参数集（8 槽 16 位，附加信息，与上层无关）
    wire [31:0] r_keystat = {slot_pset, 7'd0, sk_lock, slot_valid};

    // ================= 端口归属（组合）=================
    always @(*) begin
        // ---- 金库写口：擦除机 > 搬运 ----
        if (wiping) begin
            skv_we    = 1'b1;
            skv_waddr = wipe_addr;
            skv_din   = 8'd0;
        end else begin
            skv_we    = (state == S_STORE) && !cp_wait;
            skv_waddr = {slot[2:0], cp};
            skv_din   = out_data;
        end
        // 金库读口：S_LOAD 逐字节取 sk。
        // ⚠️ 地址要**比 cp 提前一个**：ram_dp 是同步读，这一拍摆的地址下一拍
        //    才出数。cp_wait 那一拍摆 cp 本身（第一个字节），之后每一拍都摆
        //    cp+1 —— 少了这个提前量，每个字节会被读两遍、sk 从第二个字节起
        //    整个错位，而"搬进去了 sk_len 个字节"这件事看起来完全正常。
        skv_raddr = {slot[2:0], cp + (cp_wait ? 13'd0 : 13'd1)};

        // engine 输出口的地址。S_STORE 那一趟由搬运用（同样要提前一个），
        // 其余时间跟着读游标 —— 这一拍读命中就把地址推到下一个。
        if (state == S_STORE)
            out_addr = {2'd0, pk_len} + {2'd0, cp} + {14'd0, !cp_wait};
        else
            out_addr = out_ptr[14:0] + {14'd0, rd_outdata};
    end

    // ================= 时序 =================
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            aw_got <= 1'b0; w_got <= 1'b0;
            aw_addr_r <= 8'd0; w_data_r <= 32'd0; w_strb_r <= 4'd0;
            f_bvalid <= 1'b0; f_bresp <= RESP_OKAY;
            f_rvalid <= 1'b0; f_rresp <= RESP_OKAY; f_rdata <= 32'd0;
            op <= 2'd0; pset <= 2'd0;
            sk_to_slot <= 1'b0; sk_from_slot <= 1'b0; slot <= 4'd0;
            slot_valid <= 8'd0; slot_pset <= 16'd0;
            sk_lock <= 1'b0;              // 复位是唯一能放开闩锁的事件
            in_ptr <= 16'd0; out_ptr <= 16'd0; out_len_r <= 16'd0;
            msg_len <= 16'd0; ctx_len <= 16'd0;
            run_done <= 1'b0; verify_ok_r <= 1'b0;
            param_err <= 1'b0; len_err <= 1'b0;
            zero_pulse <= 1'b0;
            wiping <= 1'b0; wipe_addr <= 16'd0; zall_d <= 1'b0;
            state <= S_IDLE; cp <= 13'd0; cp_wait <= 1'b0; kickdly <= 2'd0;
            eng_start <= 1'b0;
            in_we <= 1'b0; in_addr <= 15'd0; in_data <= 8'd0;
        end else begin
            zero_pulse <= 1'b0;
            eng_start  <= 1'b0;
            in_we      <= 1'b0;

            // ---------- 金库擦除机 ----------
            // 上升沿启动。fw_tampered 是锁存电平，用电平触发的话擦除会永远
            // 重启、WIPING 再也不会落下来。
            zall_d <= zeroize_all;
            if (zeroize_all && !zall_d) begin
                wiping    <= 1'b1;
                wipe_addr <= 16'd0;
            end else if (wiping) begin
                // 最后一个地址那一拍 skv_we 仍为高（组合自 wiping），
                // 所以 0xFFFF 也真的被写了 0，一个字节都不留。
                if (wipe_addr == 16'hFFFF) wiping <= 1'b0;
                else                       wipe_addr <= wipe_addr + 16'd1;
            end

            if (zeroize_all) begin
                // 指针、有效位一拍清掉；BRAM 交给上面那台擦除机。
                // **sk_lock 不在这里** —— 它是一次性的方向，擦秘密不等于撤防线。
                in_ptr <= 16'd0; out_ptr <= 16'd0; out_len_r <= 16'd0;
                slot_valid <= 8'd0; slot_pset <= 16'd0;
                state <= S_IDLE; run_done <= 1'b0; verify_ok_r <= 1'b0;
                param_err <= 1'b0; len_err <= 1'b0;
                cp <= 13'd0; cp_wait <= 1'b0;
            end

            // ---------- 写 ----------
            if (f_awvalid && f_awready) begin aw_got <= 1'b1; aw_addr_r <= f_awaddr; end
            if (f_wvalid  && f_wready)  begin w_got  <= 1'b1; w_data_r  <= f_wdata;
                                              w_strb_r <= f_wstrb; end
            if (wr_now) begin
                aw_got <= 1'b0; w_got <= 1'b0;
                // 擦除期间拒绝一切写，而且**明确回 SLVERR**。静默丢弃是这里
                // 最危险的选项：软件会以为 IN_DATA 灌进去了，实际 in_ptr 一步
                // 没动，接着按错误的长度启动。读仍然放行，否则软件没法轮询
                // WIPING。写满与非零写 IN_PTR 同理，也回 SLVERR。
                f_bvalid <= 1'b1;
                f_bresp  <= (wiping
                             || (wr_strb[0] && (wr_addr[5:2] == A_INDATA)
                                 && (state == S_IDLE) && in_full)
                             || wr_inptr_bad
                             || wr_busy_reject) ? RESP_SLVERR : RESP_OKAY;
                if (wr_strb[0] && !wiping_any) begin
                    case (wr_addr[5:2])
                    A_CTRL: begin
                        if (wr_data[1]) begin
                            // CLEAR：把"上一次运行"的痕迹一次清干净。
                            // 陈旧状态是上板抓到过的坑（见下面 START 被拒
                            // 那一段），所以给软件一条显式的清理路径。
                            run_done <= 1'b0; verify_ok_r <= 1'b0;
                            param_err <= 1'b0; len_err <= 1'b0;
                            in_ptr <= 16'd0; out_ptr <= 16'd0; out_len_r <= 16'd0;
                        end
                        if (wr_data[2]) zero_pulse <= 1'b1;
                        // [4] SK_LOCK：一次性闩锁，写 1 置上，**没有清零路径**。
                        // 想解开只能复位整块 PL。ZEROIZE 都不清它。
                        if (wr_data[4]) sk_lock <= 1'b1;
                        // START 放在最后判：同一拍写 CLEAR|START 的语义是
                        // "清干净再启动"。
                        //
                        // ⚠️ 同一拍写 ZEROIZE|START 则**擦除赢，不启动**。
                        // 少了 `!wr_data[2]` 这一条也不会出错，但会多一个
                        // 一拍的中间态：START 看的 zeroize_all 是本拍的值
                        // （zero_pulse 下一拍才高），于是状态机先进 S_KICK，
                        // 下一拍再被擦除拉回 S_IDLE。写成显式的更好解释，
                        // 也免得日后有人在那一拍上加动作。
                        // 一次"写全 1 到 CTRL"就会同时踩到这两位 —— 用例里
                        // 那条"没有任何写法能清掉闩锁"的反证正是这么写的。
                        if (wr_data[0] && !wr_data[2] && (state == S_IDLE)
                            && !zeroize_all && !wiping_any) begin
                            // ⚠️ 无论这次 START 是否被接受，**上一次的结果都要
                            // 当场作废**。这一条是 ML-KEM 在板上抓到的：仿真里
                            // 每条用例都从复位开始，OUT_LEN 本来就是 0，所以
                            // "拒绝之后留着上一次的结果"这个形状根本没出现过；
                            // 板上是连着跑的，于是软件轮询到 DONE=1、读出上一次
                            // 的 OUT_LEN，拿着**上一次**的输出当成这一次的结果。
                            // 比不报错更糟：它看起来成功了。
                            run_done  <= 1'b0;
                            verify_ok_r <= 1'b0;
                            out_len_r <= 16'd0;
                            out_ptr   <= 16'd0;
                            if (!params_ok || !slot_ok || !cap_ok || !len_ok) begin
                                // 不启动这一点比报错更要紧 —— 启动了再报错的话，
                                // engine 已经按一份对不上号的输入开始算了。
                                // PARAM_ERR 是"这次 START 被拒"的总括位，
                                // LEN_ERR 是其中"喂不够"那一类的细分，软件靠它
                                // 分得清是参数写错了还是字节没送完。
                                param_err <= 1'b1;
                                len_err   <= (!len_ok || !cap_ok);
                            end else begin
                                param_err <= 1'b0;
                                len_err   <= 1'b0;
                                cp        <= 13'd0;
                                cp_wait   <= 1'b1;   // 让同步读跟上一拍
                                state     <= take_sk ? S_LOAD : S_KICK;
                            end
                        end
                    end
                    // 运行途中改参数一律不认（回 SLVERR，见 wr_busy_reject）
                    A_MODE: if (state == S_IDLE) begin
                        op   <= wr_data[1:0]; pset <= wr_data[3:2];
                        sk_to_slot   <= wr_data[4];
                        sk_from_slot <= wr_data[5];
                        slot         <= wr_data[9:6];
                        // ⚠️ 写 MODE 就把写指针清零。MODE 决定软件字节落在
                        // engine 的哪个偏移（SK_FROM_SLOT 那趟要让开前面的 sk），
                        // 所以"先灌字节再改 MODE"必须变成一个**吵闹**的错误：
                        // 指针归零 → START 处报欠填，而不是按新排布去解读旧字节。
                        in_ptr <= 16'd0;
                    end
                    A_INDATA: if (wr_indata) begin
                        // 直写 engine 的输入存储：一笔 AXI 写就是一次存储写
                        in_we   <= 1'b1;
                        in_addr <= sw_addr;
                        in_data <= wr_data[7:0];
                        in_ptr  <= in_ptr + 16'd1;
                    end
                    // 只认写 0（理由见 wr_inptr_bad 处）
                    A_INPTR:  if (wr_data == 32'd0) in_ptr <= 16'd0;
                    // OUT_PTR 反过来可以随便写：它只是个读游标，读的时候还要
                    // 被 OUT_LEN 卡一道，seek 回去重读一段是正当用法。
                    A_OUTPTR: out_ptr  <= wr_data[15:0];
                    A_MSGLEN: if (state == S_IDLE) msg_len <= wr_data[15:0];
                    A_CTXLEN: if (state == S_IDLE) ctx_len <= wr_data[15:0];
                    default: ;
                    endcase
                end
            end
            if (f_bvalid && f_bready) f_bvalid <= 1'b0;

            // ---------- 读 ----------
            if (f_arvalid && f_arready) begin
                f_rvalid <= 1'b1; f_rresp <= RESP_OKAY;
                case (f_araddr[5:2])
                A_VERSION: f_rdata <= VERSION;
                A_STATUS:  f_rdata <= r_status;
                A_MODE:    f_rdata <= {22'd0, slot, sk_from_slot, sk_to_slot,
                                       pset, op};
                A_INPTR:   f_rdata <= {16'd0, in_ptr};
                // ⚠️ **只有 rd_outdata 成立时才把字节交出去**，别的情况一律回 0。
                // 这不是保守，是必须的：engine 的输出口是共用的，搬 sk 进金库
                // 那一趟 out_addr 摆的正是 sk 的地址（S_STORE）。若这里无条件
                // 接出 out_data，软件在那几千拍里读 OUT_DATA 就能一个字节一个
                // 字节地把 sk 捞出来 —— 整个金库就白做了。
                // 顺带也盖住了"读超过 OUT_LEN"与"擦除期间读"这两种情况。
                A_OUTDATA: f_rdata <= rd_outdata ? {24'd0, out_data} : 32'd0;
                A_OUTPTR:  f_rdata <= {16'd0, out_ptr};
                A_OUTLEN:  f_rdata <= {16'd0, out_len_r};
                A_MSGLEN:  f_rdata <= {16'd0, msg_len};
                A_CTXLEN:  f_rdata <= {16'd0, ctx_len};
                A_KEYSTAT: f_rdata <= r_keystat;
                A_VIOL:    f_rdata <= {viol_rd_count, viol_wr_count};
                default:   f_rdata <= 32'd0;
                endcase
                if (rd_outdata) out_ptr <= out_ptr + 16'd1;
            end
            if (f_rvalid && f_rready) f_rvalid <= 1'b0;

            // ---------- 运行 ----------
            if (!zeroize_all) begin
                case (state)
                S_IDLE: ;

                // 金库 → engine：sk 从来不经过总线，engine 也不知道它是谁送的
                S_LOAD: begin
                    if (cp_wait) begin
                        cp_wait <= 1'b0;         // skv_dout 这一拍还没跟上
                    end else begin
                        in_we   <= 1'b1;
                        in_addr <= sk_base + {2'd0, cp};
                        in_data <= skv_dout;
                        if ({1'b0, cp} + 14'd1 == {1'b0, sk_len}) begin
                            state <= S_KICK;
                        end else begin
                            cp <= cp + 13'd1;
                        end
                    end
                end

                S_KICK: begin
                    eng_start <= 1'b1;
                    kickdly   <= 2'd3;
                    cp        <= 13'd0;
                    cp_wait   <= 1'b1;
                    state     <= S_RUN;
                end

                S_RUN: begin
                    // 头三拍不看 eng_done（见 kickdly 的声明处）
                    if (kickdly != 2'd0) kickdly <= kickdly - 2'd1;
                    if (eng_done && (kickdly == 2'd0)) begin
                        verify_ok_r <= eng_vok;
                        cp      <= 13'd0;
                        cp_wait <= 1'b1;
                        state   <= store_sk ? S_STORE : S_FIN;
                    end
                end

                // engine 输出的 sk 段 → 金库。这一趟 BUSY 仍然是 1、
                // OUT_LEN 还是 0，软件读不到任何东西。
                S_STORE: begin
                    if (cp_wait) begin
                        cp_wait <= 1'b0;         // out_data 这一拍还没跟上
                    end else begin
                        if ({1'b0, cp} + 14'd1 == {1'b0, sk_len}) begin
                            state <= S_FIN;
                        end else begin
                            cp <= cp + 13'd1;
                        end
                    end
                end

                S_FIN: begin
                    out_ptr <= 16'd0;
                    // 软件读得到多少：存 sk 那一趟只到 pk 的长度。
                    // 取 min 是防 engine 报了个比 pk 还短的长度（那是一次
                    // 失败的运行）—— 那时候报 pk_len 等于凭空多给几个字节。
                    if (store_sk)
                        out_len_r <= (eng_out_len > {3'd0, pk_len})
                                     ? {3'd0, pk_len} : eng_out_len;
                    else
                        out_len_r <= eng_out_len;
                    // 槽到这里才标成有效：**中途失败的运行不该留下一个
                    // "看起来可用"的槽**，那会让后面的 Sign 拿半截 sk 去签，
                    // 出来的是一个安静的错误结果。
                    if (store_sk) begin
                        slot_valid[slot[2:0]] <= 1'b1;
                        slot_pset[slot[2:0]*2 +: 2] <= pset;
                    end
                    run_done <= 1'b1;
                    state    <= S_IDLE;
                end

                default: state <= S_IDLE;
                endcase
            end
        end
    end

    // eng_busy 没接：BUSY 报的是**本层的状态机**在不在跑（它还包含搬 sk 进出
    // 金库那两段，那时 engine 已经空闲了）。用 engine 的 busy 会让软件在
    // 搬运途中看到 BUSY=0 而去读输出。
    wire _unused = &{1'b0, f_awprot, f_arprot, wr_strb[3:1], eng_busy, 1'b0};

endmodule

`default_nettype wire
