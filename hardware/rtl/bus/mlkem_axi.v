// mlkem_axi —— ML-KEM 三个整核（KeyGen / Encaps / Decaps）的 AXI4-Lite 从机
//
//   s_axi ──▶ axi4lite_firewall ──▶ 寄存器组 ──┬─▶ mlkem_keygen
//              AxPROT/窗口/tamper              ├─▶ mlkem_encaps
//                                              └─▶ mlkem_decaps
//                    输入缓冲(8 KB) ──喂──┘   └──收──▶ 输出缓冲(8 KB)
//
// ============================================================================
// 【为什么所有输入都从同一个字节缓冲走】
// ============================================================================
// 三个核的输入形状完全不同：KeyGen 要两个 256 位的并行种子，Encaps 要一个
// 256 位的 m 加一条 ek 字节流，Decaps 要两条字节流（dk 与 c）。给每一种开一
// 组寄存器的话，寄存器表会长成三份互不相干的东西，软件侧也要写三套搬运。
//
// 这里统一成"**一切都往 IN_DATA 里灌，顺序就是标准里的顺序**"：
//
//   KeyGen : d(32) ‖ z(32)
//   Encaps : m(32) ‖ ek(384k+32)
//   Decaps : dk(768k+96) ‖ c(32·(du·k+dv))
//
// 需要并行送进核里的那几个 256 位量（d/z/m），由本模块从缓冲区头部读出来
// 装进寄存器 —— 软件不必知道哪些是"流"、哪些是"并行口"。
//
// 长度全部由 param_set 算出来，软件不用报长度，也就不存在"报错长度"这种
// 会安静产生错误结果的输入方式。
//
// 但"不用报长度"不等于"不用喂够"：START 时会校验 IN_PTR 是否达到本次运行
// 所需的字节数，不够就置 PARAM_ERR 且不启动（见 need_len）。少了这道校验，
// 只喂 32 字节的 KeyGen 会让隐式拒绝密钥 z 取到全 0 残留 —— 一个 KAT 抓不到
// 的安静错误。
//
// ============================================================================
// 【密钥材料的去向，说清楚】
// ============================================================================
// 与 key_vault 那条不变量**不同**：ML-KEM 的 dk 本来就要交给软件（它是这套
// 协议的私钥，密码机把它包装之后存到外面去），所以输出缓冲里确实有私钥字节，
// 也确实能从 OUT_DATA 读出来 —— 这不是漏洞，是接口定义。
//
// 真正的边界在于：
//   · 中间量（ŝ、ê、Â、r̂、m′、重加密出来的 c′）**一个都不进缓冲区**，
//     它们只在核内部的 BRAM 里存在过；
//   · DEBUG_BANK 在这里恒为 0，多项式存储的读口根本没有引出来；
//   · tamper 一根线同时打掉三个核与两个缓冲区。
//
// 换句话说，**软件能拿到的只有算法定义里本来就该给它的那些字节**。
//
// ============================================================================
// 【zeroize 必须真的擦 BRAM，不能只清指针】
// ============================================================================
// 第一版的 zeroize 只把 in_ptr / out_len / out_rd / seed 清零。从软件看确实
// 什么都读不到了（out_len=0，OUT_DATA 不返回任何字节），但**两块 8 KB BRAM
// 里上一次运算的 dk 一个字节都没少**。
//
// 那不是 zeroize，是把目录页撕了而正文还在。剩下的路径都还在：
//   · 下一次运算只覆盖它用到的那一段，用不到的尾巴留着上一把私钥；
//   · 位流回读、扫描链、或者哪天有人给缓冲区加个调试读口，都能把它捞出来；
//   · 更直接的：把 in_ptr 推到旧数据那一段再启动一次运算，旧字节就进了核。
//
// 所以这里是一台真正的擦除机：tamper / zeroize 的**上升沿**启动，
// 两块 BRAM 并行逐地址写 0，8192 拍走完，期间 WIPING=1、拒绝读输出、
// 拒绝写输入、拒绝启动。key_vault 那边用寄存器阵列所以能一拍全清（见该文件
// 头），BRAM 没有这个待遇 —— 有"擦了一半"的窗口，就必须把这个窗口
// 明确地暴露成一个状态位，而不是假装它不存在。
//
// 用上升沿而不是电平：fw_tampered 是**锁存**的，用电平的话擦除会永远重启，
// WIPING 再也不会落下来。
//
// ============================================================================
// 【安全世界暂存的种子：CODE-1 的 PL 侧落点】
// ============================================================================
// KeyGen 的 d‖z 原来只有一条来路：软件往 IN_DATA 灌 64 字节。也就是说种子在
// **普通世界 daemon 的栈上**待过一趟（service/pqchsm_fpgad.c 里 trng_bytes
// 取 64 字节再逐字节写回来）。有 root 的人：
//   · 能**读**到它 —— 种子完全决定私钥，读到种子等于读到私钥；
//   · 能**换**掉它 —— 灌一个自己知道的种子，此后这块板生成的每一把密钥他都
//     算得出来，而板子照常工作、ACVP 照常过、没有任何一处报错。
//
// 现在多一条来路：**SEED_DATA 暂存口**。安全世界（今天是 BL31 的 EL3 SiP，
// 批 2 之后是 OP-TEE 的 TA）在自己那一侧生成 d‖z，经这个口写进来 16 个 32 位
// 字；`MODE.SEED_STAGED` 置位的 KeyGen 就用这份暂存的种子，**不看 IN_DATA**。
// 普通世界从头到尾只发一条"给我生成一把密钥"的命令，看不到种子明文。
//
// 这是**过渡态，而且是朝着 TEE 主线的那一档**：保管方是安全世界，PL 只是
// 收下、展开、算完即弃。批 2 把保管从 EL3 上移到 TA 时，这一侧一个字都不用改
// —— 换的只是"谁在写 SEED_DATA"。所以这里没有、也不该有任何"种子由 PL 自己
// 生成并常驻 PL"的东西：那是被否掉的另一条路线，做了批 2 还得推翻。
//
// 四件配套的事，缺一条这个改动就是装饰：
//
//  ① **SEED_DATA 只认安全世界事务（AxPROT[1]==0），且与 SECURE_ONLY 无关。**
//     整块从机的防火墙参数是可配的（演示位流 SECURE_ONLY=0），但种子写口
//     **永远**只认安全事务 —— 演示形态下普通世界经 /dev/mem 也写不进来。
//     被拒的写计入 SEED_STAT[31:16]，不静默。
//
//     ⚠️⚠️ **被拒的写回 OKAY 并丢弃（RAZ/WI），绝不回 SLVERR。**
//     第一版这里回的是 SLVERR，理由是"静默丢弃最危险，写的人会以为进去了"。
//     那个理由本身没错，但它被一个更大的问题压过去了，而这个问题本仓库
//     已经用几次断电换过教训（axi4lite_firewall.v 文件头写着全过程）：
//
//       **AXI 的写是 posted 的。** 错误响应过一会儿才以 **SError** 打回来，
//       SError 不属于任何一条指令，aarch64 的内核只能 panic。
//       于是"往种子口写一次"的代价是**一次断电** —— 而触发它的不必是攻击者，
//       一个探测寄存器的脚本就够了。防火墙当初正是为这个从 DECERR 改成 RAZ/WI。
//       更糟的是从 EL3 发的那一笔：SError 在 EL3 里根本没有处理器，核当场卡死。
//
//     所以补偿改成与防火墙同一套：**丢弃 + 计数**。写的人查 SEED_STAT 就知道
//     字数一步没动、被拒计数涨了一格。"可核对"由计数器提供，不由总线错误提供。
//     ⚠️ 这一条挡的是"普通世界自己发事务"。它**挡不住**"root 经 EL3 的通用
//        PL_WR SiP 转一手"—— 那条要在 BL31 的白名单里把这个偏移排除掉
//        （boot/atf/patch_atf_secmmio.py），两边合起来才成立。少了任一边，
//        这个口就是白做的。
//
//  ② **SEED_DATA 没有读回路径。** 读它恒为 0（不是"被拒才回 0"，是压根没有
//     那条 case）。SEED_STAT 只报字数与闩锁状态，一个种子字节都不报。
//
//  ③ **用一次就作废。** START 那一刻把暂存搬进 d/z 并当场清掉暂存与字计数。
//     一份种子只能生出一把密钥 —— 既是"算完即弃"，也堵掉"安全世界早就忘了
//     这份种子，普通世界却还能拿它再生成一把"。
//
//  ④ `CTRL.SEED_LOCK` 一次性闩锁：置上之后 KeyGen **永远**走暂存口，
//     MODE 里怎么写都没用。写 1 置上、没有清零路径、zeroize 都不清，
//     只有复位整块 PL 能放开。**它不是熔丝**，掉电/重配即回到未闩状态，
//     不在本项目"不做任何一次性/不可逆写入"的红线内。
//
//     ⚠️ **它与 DK_LOCK 是两把方向相反的闩，绝不能连动。**
//        DK_LOCK 守的是"私钥留在 PL"，而最终架构（FINAL-PLAN §7 V-04）要
//        **删掉**它——PL 不该是私钥的保管方。SEED_LOCK 守的是"种子只能来自
//        安全世界"，与最终架构同向。写 SEED_LOCK 顺手置上 DK_LOCK 会把一条
//        待删的机制焊得更死，所以这里**不做**那件事。
//
// ============================================================================
// 【这个口在批 1 能挡住什么、挡不住什么 —— 别高估它】
// ============================================================================
// 挡得住：**读种子**（无读回路径）、**自己塞种子**（非安全事务被拒 + BL31
// 白名单排除该偏移）、**重放同一份种子**（用一次即作废）。
//
// 挡不住，且必须写在这里：
//  · **降级**。批 1 的 MODE 仍由普通世界 daemon 写，root 可以干脆不置
//    SEED_STAGED、退回自己往 IN_DATA 灌种子那条老路。堵它要么置 SEED_LOCK，
//    要么等批 2 让 daemon 退到 TA 后面（FINAL-PLAN D3 ⚠️②）。
//  · **dk 里本来就含种子等价物**。ML-KEM 的 dk = dk_PKE‖ek‖H(ek)‖z，
//    **z 字面就在 dk 里**，dk_PKE 又是 d 派生的 s。所以在 PL 金库还在的
//    批 1 形态里，种子进 EL3 只有**同时用 DK_TO_SLOT**（dk 不出 OUT_DATA）
//    才是完整的。批 2 删掉金库、私钥只在一次运算内存在之后，这条自然消失。
//    ——这是形态的事实，不是本模块的缺陷，但不写出来就成了夸大。
//
// 默认仍然是关的（MODE.SEED_STAGED=0、SEED_LOCK 未置）：ACVP 的 KeyGen 向量
// 要按指定的 d‖z 复现，出厂验证必须留 IN_DATA 那条路。
`default_nettype none

module mlkem_axi #(
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

    input  wire        tamper,

    // 译码器（axi4lite_xbar）的"没命中任何槽"计数。
    // 从这里出口，是因为本从机 SECURE_ONLY=1 —— **只有安全世界读得到**。
    // 译码器改 RAZ/WI 之后走错地址不再报错，这个计数是唯一的痕迹。
    input  wire [31:0] xbar_viol_count
);
    localparam [1:0] RESP_OKAY = 2'b00, RESP_SLVERR = 2'b10;

    localparam [3:0] A_VERSION = 4'h0, A_CTRL   = 4'h1, A_STATUS = 4'h2,
                     A_MODE    = 4'h3, A_INDATA = 4'h4, A_INPTR  = 4'h5,
                     A_OUTDATA = 4'h6, A_OUTLEN = 4'h7, A_OUTRD  = 4'h8,
                     A_VIOL    = 4'h9, A_PARAM0 = 4'hA,
                     A_XBAR_VIOL = 4'hB, A_KEYSTAT = 4'hC, A_KEYPSET = 4'hD,
                     // 0x38 SEED_DATA  W  安全世界写 16 个字暂存 d‖z；**读恒 0**
                     // 0x3C SEED_STAT  R  只报字数/闩锁/被拒计数，不报种子字节
                     A_SEEDDATA = 4'hE, A_SEEDSTAT = 4'hF;

    // ================= 4 字节打包口（D11）=================
    // 0x40 IN_DATA4   W  一笔写 4 个字节，小端：wdata[7:0] 落 in_ptr+0
    // 0x44 OUT_DATA4  R  一笔读 4 个字节，小端：rdata[7:0] 是 out_rd+0
    //
    // 【为什么值得做】交付路径上每一笔核访问都经 EL3（ioctl + SMC），
    // 一个字节一笔。实测时间随**传输字节数**线性走而不随计算量走
    // （KeyGen 512/768/1024 = 1 : 1.53 : 2.10，正好是输出字节数之比 1 : 1.48
    // : 1.96；算力需求按 k² 应是 1 : 2.25 : 4）。也就是说量到的是管子不是核。
    // 打包 4 字节 = 同样的字节数少发 3/4 的 ioctl 与 SMC。
    //
    // 【为什么是新地址而不是 MODE 里的一位】一位模式开关会让"同一个地址
    // 读出几个字节"取决于一个可能陈旧的状态位 —— 写错了不报错，只是从某个
    // 字节起整段错位，而错位后的密文/公钥看起来完全合法。新地址是**无状态**
    // 的：一笔事务自己说清楚要几个字节。
    //
    // 【尾巴归尾巴】OUT_DATA4 只在**还剩 ≥4 字节**时给数据，否则回 0 且不推进。
    // 长度不是 4 的倍数的情形真的存在（ML-DSA-87 的 σ 是 4627 字节），
    // 软件读到剩 <4 就退回逐字节的 OUT_DATA。判据是软件自己算得出来的
    // （它知道 OUT_LEN），不需要额外的状态位。
    localparam [5:0] A6_INDATA4 = 6'h10, A6_OUTDATA4 = 6'h11;

    localparam [1:0] M_KEYGEN = 2'd0, M_ENCAPS = 2'd1, M_DECAPS = 2'd2;

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
        // ⚠️ 窗口从 0x3F 放到 0x7F：打包口 IN_DATA4/OUT_DATA4 在 0x40/0x44。
        // 0x00~0x3F 那 16 个寄存器已经排满（SEED_STAT 占到 0xF），而**不想**
        // 用 MODE 里的一位来切换打包 —— 那会让"同一个地址读出几个字节"
        // 取决于一个可能陈旧的状态位，错了不报错、只是数据错位。
        .ADDR_BASE(32'h0000_0000), .ADDR_MASK(32'h0000_0080)
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

    // ================= 两块字节缓冲 =================
    // 输入最大是 Decaps 的 dk+c = 3168+1568 = 4736；
    // 输出最大是 KeyGen-1024 的 ek+dk = 6208。8 KB 两块都装得下。
    reg         ina_we;   reg [12:0] ina_addr;  reg [7:0] ina_din;
    reg  [12:0] inb_addr; wire [7:0] inb_dout;
    ram_dp #(.DW(8), .AW(13)) u_inbuf (
        .clk(clk),
        .a_we(ina_we), .a_addr(ina_addr), .a_din(ina_din), .a_dout(),
        .b_we(1'b0),   .b_addr(inb_addr), .b_din(8'd0),    .b_dout(inb_dout));

    reg         outa_we;  reg [12:0] outa_addr; reg [7:0] outa_din;
    reg  [12:0] outb_addr; wire [7:0] outb_dout;
    ram_dp #(.DW(8), .AW(13)) u_outbuf (
        .clk(clk),
        .a_we(outa_we), .a_addr(outa_addr), .a_din(outa_din), .a_dout(),
        .b_we(1'b0),    .b_addr(outb_addr), .b_din(8'd0),     .b_dout(outb_dout));

    // ================= 单操作展开区（原"片内私钥金库"）=================
    // ============================================================================
    // 【批 2 的重定位：从"金库"变成"展开区"】
    // ============================================================================
    // 这块 BRAM 原来是 **16 槽 × 4 KB 的 dk 金库**：KeyGen 按槽号存 dk、
    // Decaps 按槽号取，私钥跨会话留在 PL 里。那让 **PL 成了私钥的权威保管方** ——
    // 与"密钥归 TEE、PL 无状态"正面冲突（登记表 V-01/V-05/V-06）。
    //
    // 现在它降级成**匿名的单操作展开区**，四条性质缺一不可（§7.2）：
    //
    //  ① **没有槽号、没有 valid 位。** 寻址只有"本次运算的第几个字节"，
    //     对外寄存器面上**根本没有能引用它的名字** —— 上一次运算的展开态
    //     无从被下一次引用，这比"擦干净了"更强：即便擦除有 bug 也引用不到。
    //  ② **擦除由硬件在 S_FIN 无条件触发**，不由任何软件（包括 TA）决定。
    //     软件能做的只有"等它擦完"（EXP_WIPING 状态位）。
    //  ③ 擦除期间拒绝下一次 START。
    //  ④ 逐地址真写零，不靠复位、不靠"下次覆盖"。
    //
    // 【那 Decaps 的 dk 从哪来】
    // 从**同一条命令里现展开的那一份**。批 2 的形态是"种子每次重送"：
    // 安全世界（TA）保管种子，每次运算把它送进暂存口，然后一条
    // `MODE.CHAIN` 的命令让 PL **先从种子展开、紧接着做本次运算、算完就擦**。
    // 于是 dk 既没有经过总线，也没有在 PL 里过夜。
    //
    // 【尺寸】4096 字节，够放最大的 dk（ML-KEM-1024 = 3168）。
    // 从 64 KB 缩到 4 KB 的直接好处：擦除从 65536 拍（874 µs）降到 4096 拍
    // （55 µs），而擦除现在每次运算都要做 —— 不缩的话这一条就等于给每次运算
    // 加上近一毫秒。
    reg         exp_we;   reg [11:0] exp_waddr; reg [7:0] exp_din;
    reg  [11:0] exp_raddr; wire [7:0] exp_dout;
    ram_dp #(.DW(8), .AW(12)) u_expand (
        .clk(clk),
        .a_we(exp_we), .a_addr(exp_waddr), .a_din(exp_din), .a_dout(),
        .b_we(1'b0),   .b_addr(exp_raddr), .b_din(8'd0),    .b_dout(exp_dout));

    // 展开区的专用擦除机：S_FIN 无条件启动，走完 4096 个地址。
    // 与 zeroize 那台分开是有意的 —— 那台要盖住两块 8 KB 缓冲（输出缓冲里
    // 有软件还没读走的结果），而这台**只擦展开区**，所以可以每次运算都跑。
    reg        exp_wiping;
    reg [11:0] exp_waddr_w;

    // ================= 安全世界暂存的种子 =================
    // 16 个 32 位字 = d(32 字节) ‖ z(32 字节)。字 i 落在 [32i +: 32]，
    // 字内小端 —— 于是"第 j 个字节"就是 seed_stage[8j +: 8]，与 S_PRE 那条
    // 从 IN_DATA 读的路（先到的字节落最低位）是**同一种解释**。
    // 两条路解释不一致的话，同一份 RTL 会有两种"种子长什么样"，
    // 将来对波形或做形式验证时无从下手。
    reg [511:0] seed_stage;
    reg [4:0]   seed_wcnt;              // 已收下几个字，0..16
    reg         seed_lock;              // 一次性闩锁（KeyGen 永远走暂存口）
    reg         seed_err;               // 上一次 START 因暂存种子没备好被拒
    reg [15:0]  seed_viol;              // 非安全世界写种子口的次数（饱和）
    reg         seed_ovf;               // 收满之后还有人写（安全世界那侧的 bug）
    wire        seed_ready = (seed_wcnt == 5'd16);

    // ================= 控制寄存器 =================
    reg [1:0]  mode, pset;
    // ---- 链式运算的相位（批 2：PL 无状态的落点）----
    // chain_ph：0 = 展开相位（跑 KeyGen 把 dk 写进展开区），1 = 运算相位。
    // 非链式运算恒在相位 1，此时 mode_eff 就是 mode —— 老路一个字节没变。
    reg        chain_ph;
    // MODE 现在只剩三样（V-05：槽位 ABI 已删）：
    //   [1:0] OP           0 KeyGen / 1 Encaps / 2 Decaps
    //   [3:2] PSET         参数集
    //   [4]   CHAIN        **本次运算先从暂存种子展开私钥**，再做 OP，算完即擦。
    //                      这是"PL 无状态"的落点：Decaps 不再按槽引用上一次的
    //                      dk，而是在同一条命令里现展开一份、用完擦掉。
    //   [10]  SEED_STAGED  d‖z 取自 SEED_DATA 暂存口，不看 IN_DATA
    //
    // ⚠️ 原来的 DK_TO_SLOT / DK_FROM_SLOT / SLOT[9:6] **已删除**，位置不复用
    //    （CHAIN 占了原 DK_TO_SLOT 的 [4]，因为它替代的正是那条语义）。
    //    写它们不再有任何效果 —— 不给"已废弃"的专用错误码，那等于向调用方
    //    确认这里曾经有个按槽引用私钥的口子。
    reg        chain;
    reg        seed_staged;
    reg [12:0] in_ptr, out_len, out_rd;

    // ---- IN_DATA4 的 4 拍写出机 ----
    // 输入缓冲是 8 位宽的 BRAM，一拍只写得进一个字节。所以一笔打包写在
    // 总线上是一次事务，在 PL 里摊成 4 拍：第 0 拍就地写掉最低那个字节，
    // 余下 3 个字节由这台小机器接着写。
    // ⚠️ 期间必须**挡住下一笔写**（wr_now 里加 !pkw_run），否则第二笔会和
    //    这台机器抢 ina_* 那三根线，表现是缓冲区里某几个字节静默变成别人的。
    reg        pkw_run;
    reg [1:0]  pkw_i;
    reg [23:0] pkw_data;
    reg [12:0] pkw_addr;

    // ---- OUT_DATA4 的 4 拍读入机 ----
    // ram_dp 的 B 口是**同步读、慢一拍**（b_dout <= mem[b_addr]），所以逐字节
    // 那条路一直靠"空闲时地址就停在 out_rd 上"来让数据提前备好。打包读沿用
    // 这个性质：握手那一拍 outb_dout 已经是 out_rd 那个字节，直接收下，
    // 同时把地址推到 out_rd+1，此后每拍收一个。四个字节齐了才给 RVALID。
    reg        pkr_run;
    reg [2:0]  pkr_i;
    reg [23:0] pkr_d;
    reg [13:0] ocnt;        // 本次运行核已经吐出的字节总数（含进金库的那部分）
    reg        zero_pulse;

    // ---- BRAM 擦除机 ----
    reg        wiping;
    // 13 位：批 2 把 64 KB 的私钥金库降级成 4 KB 的展开区之后，三块 BRAM 里
    // 最大的就是两块 8 KB 的缓冲。展开区只用低 12 位，于是它会被写两遍 ——
    // 写两遍 0 和写一遍 0 没有区别，为此再加一个计数器不值得。
    //
    // ⚠️ 擦除从 65536 拍回到 **8192 拍**（@75 MHz 约 109 µs）。这是删金库的
    // 直接好处，也是必须的：展开区现在**每次运算之后都要擦**，65536 拍会给
    // 每次运算加上近一毫秒。软件轮询 WIPING 的上限可以跟着收回来。
    reg [12:0] wipe_addr;
    reg        zall_d;

    // ---- 非法参数 ----
    // mode 与 pset 各 2 位，但**只有 0/1/2 有意义**。值 3 不是"另一种配置"，
    // 是一个不存在的东西：长度计算会走到 pset==2 那条分支（k=4、dv=5），
    // 而模式选择会落到 default 也就是 Decaps —— 于是核按 1024 的长度收 Decaps
    // 的输入，喂不满就永远等下去。软件看到的是 BUSY 一直不落，
    // 与"算得慢"分不开。所以在 START 那一刻就判掉，并且**明确报错**。
    reg        param_err;
    wire       params_ok = (mode != 2'd3) && (pset != 2'd3);

    // ---- 链式：先展开、再运算 ----
    // 只有 Decaps 需要私钥，所以只有它有链式形态；KeyGen 本来就是从种子展开，
    // Encaps 只要公钥。
    wire       chain_run = chain && (mode == M_DECAPS);
    wire [1:0] mode_eff  = (chain_run && !chain_ph) ? M_KEYGEN : mode;

    // 本次 KeyGen 走不走暂存的种子。闩上之后软件说了不算。
    wire       use_staged = (mode_eff == M_KEYGEN) && (seed_staged || seed_lock);

    // ---- 由 param_set 算出来的长度（软件不用报，也就报不错）----
    wire [2:0]  k    = (pset == 2'd0) ? 3'd2 : (pset == 2'd1) ? 3'd3 : 3'd4;
    wire        d11  = (pset == 2'd2);
    wire [3:0]  dv   = d11 ? 4'd5 : 4'd4;
    wire [13:0] eklen = {3'b0, k, 8'd0} + {4'b0, k, 7'd0} + 14'd32;   // 384k+32
    wire [13:0] dklen = {2'b0, k, 9'd0} + {3'b0, k, 8'd0} + 14'd96;   // 768k+96
    wire [13:0] c1len = {3'b0, k, 8'd0} + {5'b0, k, 6'd0}
                        + (d11 ? {6'b0, k, 5'd0} : 14'd0);
    wire [13:0] clen  = c1len + {5'b0, dv, 5'd0};

    // ================= 状态机 =================
    localparam [2:0] S_IDLE = 3'd0, S_PRE = 3'd1, S_KICK = 3'd2,
                     S_RUN  = 3'd3, S_FIN = 3'd4;
    reg [2:0]  state;
    reg [6:0]  pre_cnt;                 // 预读 d/z/m 的字节计数
    reg [255:0] seed_a, seed_b;         // KeyGen 的 d/z；Encaps 的 m 放 seed_a
    reg [12:0] fp;                      // 输入流的读指针
    reg [7:0]  fb_r;                    // 从缓冲取出的一个字节
    reg        fb_v, fb_wait;
    reg        run_done;
    // start 是非阻塞赋值，要到下一拍才真正拉高；而三个核的 done 是**电平**，
    // 一直保持到下一次 start 才清。所以进 S_RUN 的头几拍绝不能看 core_dn ——
    // 看了就会读到上一次运行残留的 done，当场结束、out_len 是 0。
    // 表现：**每个核第一次跑永远对，第二次必错**（真机上就是这么暴露的；
    // 仿真里每个核只跑一次，跑两次那条中间又有 zeroize 复位，所以没抓到）。
    reg [1:0]  kickdly;

    // 本次要往核里喂多少字节（KeyGen 不喂流）
    wire [13:0] feed_len = (mode_eff == M_ENCAPS) ? (14'd32 + eklen)
                         : (mode_eff == M_DECAPS) ? (dklen + clen)
                                              : 14'd0;
    // 预读多少字节进并行寄存器
    wire [6:0]  pre_len  = (mode_eff == M_KEYGEN) ? 7'd64
                         : (mode_eff == M_ENCAPS) ? 7'd32 : 7'd0;

    // ========================================================================
    // 【本次运行必须喂够多少字节 —— 不够就不许启动】
    // ========================================================================
    // 少了这道校验会出一个**安静而且危险**的错误：
    //
    //   软件只写了 32 字节 d 就 START（KeyGen 需要 d‖z 共 64 字节），
    //   于是 z 取的是输入缓冲里 32..63 那一段的**残留**。冷启动或刚 zeroize
    //   之后那一段是全 0 —— 于是 **z = 0**。
    //
    // z 是 ML-KEM 的**隐式拒绝密钥**：解封装失败时返回 J(z‖c)。z 可预测，
    // 攻击者就能算出任意密文的隐式拒绝值，从而把"解封装失败"和"解封装成功"
    // 区分开 —— 而隐式拒绝的全部意义就是让这两者不可区分。
    //
    // 这个错误 KAT 抓不到：KAT 总是喂满的。它只在软件写少了的时候出现，
    // 而那时硬件照常给出一个**看起来完全合法**的密钥对。
    //
    // Encaps/Decaps 同理：喂不满时核会一直等，或者用到残留字节。
    // 展开相位把 dk 收进展开区。**不再有"软件说了算"这回事**：
    // 收不收由 chain_run 决定，而 chain_run 由本次运算需不需要私钥决定。
    // （原来这里是 dk_to_slot || dk_lock —— 一个软件位加一个一次性闩，
    //   两者都已随 V-04/V-05 删除。）
    wire store_dk = (mode_eff == M_KEYGEN) && chain_run;
    // 核吐出的第 ocnt 个字节该进金库还是进输出缓冲：ek 在前，dk 在后。
    wire out_to_vault = store_dk && (ocnt >= eklen);
    // dk 在金库里的槽内偏移。单列一个中间量是因为 Verilog 不允许对括号
    // 表达式直接做位选（(a-b)[11:0] 是语法错误）。
    wire [13:0] dk_off = ocnt - eklen;

    // 运算相位从展开区取 dk（就是相位 0 刚展开的那一份）。
    wire take_dk = (mode_eff == M_DECAPS) && chain_run && chain_ph;
    wire feed_from_vault = take_dk && ({1'b0, fp} < dklen);

    wire [13:0] need_len = (feed_len > {7'd0, pre_len}) ? feed_len
                                                       : {7'd0, pre_len};
    // 从金库取 dk 时软件只需要送 c，欠填的门槛也要跟着降 ——
    // 否则一个完全正确的调用会被判成参数错误。
    // 走暂存种子的 KeyGen 一个字节都不用软件送，门槛降到 0
    // （use_staged 蕴含 mode==KEYGEN，与 take_dk 互斥，两条分支不会打架）。
    wire [13:0] need_eff = take_dk    ? clen
                         : use_staged ? 14'd0
                                      : need_len;
    wire        len_ok   = ({1'b0, in_ptr} >= need_eff);

    // 走暂存种子就必须真的有一份备好的种子（16 个字全到齐）。
    // 不查这一条的后果不是报错，是**拿一份只灌了一半、其余是残留（冷启动为
    // 全 0）的种子当私钥种子** —— 出来的密钥对看起来完全合法。
    // 这与 need_len 处那条"喂不满让 z=0"是同一类安静错误，判法也一样：
    // 在 START 那一刻挡住，且不启动任何核。
    wire        seed_gate_ok = !use_staged || seed_ready;

    // 从金库取 dk 还要求：那个槽真的装了东西，而且**装的时候用的是同一个
    // 参数集**。pset 不一致时长度全错，表现是"喂不满、BUSY 一直不落"，
    // 与算得慢分不开 —— 所以在 START 那一刻就判掉。
    // 原来这里判"那个槽装了东西吗、参数集对得上吗"。**槽没有了，这条判据
    // 也就不存在了** —— 链式运算里 dk 是同一条命令自己展开的，参数集必然一致。
    // 这不是把检查删掉，是把**需要检查的那个状态**删掉了。
    wire        slot_ok   = 1'b1;

    // ================= 三个核 =================
    // ⚠️ DEBUG_BANK 恒为 0：多项式存储的读口在这里根本没有引出来。
    wire zeroize_all = zero_pulse || tamper || fw_tampered;

    // 三个核各自的擦除进度。**要一起等完**：只等本层那台擦除机，会在核还没
    // 擦干净时就放行启动与读出（与 mldsa_axi 的 wiping_any 同一条理由）。
    wire u_kg_wiping, u_en_wiping, u_de_wiping;
    wire core_wiping = u_kg_wiping || u_en_wiping || u_de_wiping;
    // 本层的擦除机 + 三个核的，**要一起等完**。声明放在这里而不是 r_status
    // 旁边：Icarus 要求先声明后使用，而 rd_outdata 那一行在它前面。
    wire wiping_any  = wiping || core_wiping;
    // 展开区擦除期间拒绝下一次 START（§7.2 条件③）。
    // ⚠️ **不并进 wiping_any**：那会连带拒掉读 OUT_DATA，而软件此刻正要读
    //    本次运算的结果 —— 擦的是展开区，与输出缓冲无关。
    wire busy_any    = wiping_any || exp_wiping;

    reg  kg_start, en_start, de_start;
    wire kg_done, en_done, de_done, de_hash_ok;
    wire kg_ov, en_ov, de_ov;
    wire kg_ol, en_ol, de_ol;
    wire [7:0] kg_od, en_od, de_od;
    wire en_ekr, de_dkr, de_cr;

    // 输出侧：跑起来之后一直 ready —— 缓冲区总收得下（8 KB 大于任何一次输出）
    wire out_rdy = (state == S_RUN);

    mlkem_keygen #(.DEBUG_BANK(0)) u_kg (
        // ⚠️ 复位改回**纯 rst_n**：核现在有真的擦除口了，
        // 就不该再拿复位充数 —— 复位本来也擦不掉 BRAM，那种写法只是
        // 看起来像擦了。擦除走 zeroize，核擦完把 wiping 落下。
        .clk(clk), .rst_n(rst_n),
        .zeroize(zeroize_all), .wiping(u_kg_wiping),
        .param_set(pset), .d_in(seed_a), .z_in(seed_b),
        .start(kg_start), .done(kg_done),
        .out_valid(kg_ov), .out_ready(out_rdy && (mode_eff == M_KEYGEN)),
        .out_data(kg_od), .out_last(kg_ol),
        .dbg_addr(12'd0), .dbg_data());

    mlkem_encaps #(.DEBUG_BANK(0)) u_en (
        // ⚠️ 复位改回**纯 rst_n**：核现在有真的擦除口了，
        // 就不该再拿复位充数 —— 复位本来也擦不掉 BRAM，那种写法只是
        // 看起来像擦了。擦除走 zeroize，核擦完把 wiping 落下。
        .clk(clk), .rst_n(rst_n),
        .zeroize(zeroize_all), .wiping(u_en_wiping),
        .param_set(pset), .m_in(seed_a),
        .start(en_start), .done(en_done),
        .ek_valid(fb_v && (mode_eff == M_ENCAPS) && (fp >= 13'd32)),
        .ek_ready(en_ekr), .ek_data(fb_r),
        .out_valid(en_ov), .out_ready(out_rdy && (mode_eff == M_ENCAPS)),
        .out_data(en_od), .out_last(en_ol),
        .dbg_addr(12'd0), .dbg_data());

    mlkem_decaps #(.DEBUG_BANK(0)) u_de (
        // ⚠️ 复位改回**纯 rst_n**：核现在有真的擦除口了，
        // 就不该再拿复位充数 —— 复位本来也擦不掉 BRAM，那种写法只是
        // 看起来像擦了。擦除走 zeroize，核擦完把 wiping 落下。
        .clk(clk), .rst_n(rst_n),
        .zeroize(zeroize_all), .wiping(u_de_wiping),
        .param_set(pset),
        .start(de_start), .done(de_done),
        .dk_valid(fb_v && (mode_eff == M_DECAPS) && ({1'b0, fp} <  dklen)),
        .dk_ready(de_dkr), .dk_data(fb_r),
        .c_valid (fb_v && (mode_eff == M_DECAPS) && ({1'b0, fp} >= dklen)),
        .c_ready(de_cr), .c_data(fb_r),
        .out_valid(de_ov), .out_ready(out_rdy && (mode_eff == M_DECAPS)),
        .out_data(de_od), .out_last(de_ol),
        .dk_hash_ok(de_hash_ok),
        .dbg_addr(11'd0), .dbg_data());

    // 当前模式下核的输出与"喂进去了没有"
    wire core_ov = (mode_eff == M_KEYGEN) ? kg_ov : (mode_eff == M_ENCAPS) ? en_ov : de_ov;
    wire [7:0] core_od = (mode_eff == M_KEYGEN) ? kg_od
                       : (mode_eff == M_ENCAPS) ? en_od : de_od;
    wire core_ol = (mode_eff == M_KEYGEN) ? kg_ol : (mode_eff == M_ENCAPS) ? en_ol : de_ol;
    wire core_dn = (mode_eff == M_KEYGEN) ? kg_done
                 : (mode_eff == M_ENCAPS) ? en_done : de_done;

    wire feed_fire = (mode_eff == M_ENCAPS) ? (fb_v && (fp >= 13'd32) && en_ekr)
                   : (mode_eff == M_DECAPS) ? (fb_v && (({1'b0, fp} < dklen) ? de_dkr
                                                                        : de_cr))
                                        : 1'b0;

    // ================= 写通道 =================
    reg aw_got, w_got;
    reg [7:0]  aw_addr_r;
    reg [2:0]  aw_prot_r;
    reg [31:0] w_data_r;
    reg [3:0]  w_strb_r;

    assign f_awready = !aw_got && !f_bvalid;
    assign f_wready  = !w_got  && !f_bvalid;

    // ⚠️ !pkw_run：IN_DATA4 摊成 4 拍写，期间不接新的写事务（见 pkw_run 处）。
    // 顺带一个好性质：写响应因此发生在**四个字节都落地之后**。
    wire wr_now = (aw_got || (f_awvalid && f_awready))
                  && (w_got || (f_wvalid && f_wready)) && !f_bvalid && !pkw_run;
    wire [7:0]  wr_addr = (f_awvalid && f_awready) ? f_awaddr : aw_addr_r;
    wire [2:0]  wr_prot = (f_awvalid && f_awready) ? f_awprot : aw_prot_r;
    wire [31:0] wr_data = (f_wvalid  && f_wready)  ? f_wdata  : w_data_r;
    wire [3:0]  wr_strb = (f_wvalid  && f_wready)  ? f_wstrb  : w_strb_r;

    wire wr_indata = wr_now && wr_strb[0] && !wr_addr[6]
                     && (wr_addr[5:2] == A_INDATA)
                     && (state == S_IDLE) && !wiping;

    // 打包写：与逐字节那条**同样的四个前提**（空闲、没在擦、strb 有效），
    // 只是一笔进 4 个字节。不加任何额外许可 —— 打包不是一条新的权限路径，
    // 只是同一条路上少发几笔事务。
    wire wr_indata4 = wr_now && wr_strb[0]
                      && (wr_addr[7:2] == A6_INDATA4)
                      && (state == S_IDLE) && !wiping;

    // ---- 种子暂存口：**永远只认安全世界事务，与 SECURE_ONLY 无关** ----
    // 前面那道防火墙的 SECURE_ONLY 是可配的（演示位流是 0），种子口不跟它走：
    // AxPROT[1]==0 才收。演示形态下普通世界经 /dev/mem 发出的事务恒为 1，
    // 于是写不进来 —— 而 daemon 走的是 /dev/secmmio → EL3，事务是安全的，
    // 两种位流下都能正常staging。
    //
    // ⚠️ 再说一次（文件头①）：这挡的是"自己发事务"，挡不住"经 EL3 通用
    //    PL_WR 转一手"。那一半在 BL31 的白名单里。
    wire wr_seed_addr = wr_now && wr_strb[0] && (wr_addr[5:2] == A_SEEDDATA)
                        && !wiping;
    wire wr_seed_sec  = (wr_prot[1] == 1'b0);
    wire wr_seed      = wr_seed_addr && wr_seed_sec && !seed_ready;
    // 被拒的（非安全世界发起的）种子写：**丢弃 + 计数，响应仍是 OKAY**。
    // 绝不回 SLVERR —— posted 写的错误以 SError 回来，内核只能 panic，
    // 代价是一次断电（完整理由见文件头①）。可核对性由计数器提供。
    wire wr_seed_deny = wr_seed_addr && !wr_seed_sec;
    // 已经收满 16 个字还继续写：同样丢弃 + 计数，不回总线错误。
    // 写第 17 个字只可能是安全世界那边的 bug（多写、或忘了 START 就重写），
    // 而它恰恰是**从 EL3 发的** —— SError 在 EL3 里没有处理器，核会当场卡死，
    // 比在内核里 panic 还糟。所以这一条更不能回错误。
    // 判据留在 SEED_STAT：字数停在 16、overfull 计数涨。
    // 要重写就先 CTRL.SEED_CLR 作废旧的。
    wire wr_seed_full = wr_seed_addr && wr_seed_sec && seed_ready;

    // ================= 读通道 =================
    // ⚠️ !pkr_run：OUT_DATA4 要花 4 拍把字节收齐，期间 f_rvalid 还是 0，
    // 少了这一条 arready 会保持为高、放第二笔读进来，两笔一起用 outb_addr
    // 那一根地址线，结果是两笔都读到错位的字节。
    assign f_arready = !f_rvalid && !pkr_run;
    // 擦除期间一律不给输出：out_len 这时已经是 0，但不靠它 ——
    // 靠一个显式条件，免得哪天 out_len 的清零时机变了就漏出去。
    wire rd_outdata = f_arvalid && f_arready && !f_araddr[6]
                      && (f_araddr[5:2] == A_OUTDATA)
                      && !wiping_any && ({1'b0, out_rd} < {1'b0, out_len});

    // 打包读只在**还剩至少 4 个字节**时给数据。剩得不够就回 0 且不推进 ——
    // 软件知道 OUT_LEN，自己算得出什么时候该退回逐字节的 OUT_DATA。
    // 不做"零填充并推进到末尾"那种半吊子语义：那会让 out_rd 越过 out_len，
    // 而 out_rd 是下一次读的起点，越界之后再读什么都说不清。
    wire rd_out4 = f_arvalid && f_arready
                   && (f_araddr[7:2] == A6_OUTDATA4)
                   && !wiping_any
                   && ({1'b0, out_rd} + 14'd4 <= {1'b0, out_len});

    // [0] BUSY  [1] DONE  [2] HASH_OK  [3] TAMPER  [4] WIPING  [5] PARAM_ERR
    // [6] SEED_ERR —— 要走暂存种子，而 START 那一刻暂存里没有备好的一份。
    //     与 PARAM_ERR 分开是因为处置不同：PARAM_ERR 是软件写错了参数，
    //     SEED_ERR 是**安全世界那一侧没把种子送进来**（或者被 SEED_CLR 作废了），
    //     重试同一条命令没有意义，要回去看 SiP 那一端。
    wire [31:0] r_status = {25'd0, seed_err, param_err, wiping_any, fw_tampered,
                            de_hash_ok,
                            (state == S_IDLE) && run_done, (state != S_IDLE)};

    // SEED_STAT：**一个种子字节都不出现在这里**。
    //   [4:0]   已收下的字数（0..16）
    //   [8]     SEED_READY   16 个字齐了
    //   [9]     SEED_LOCK    闩锁已置（KeyGen 永远走暂存口）
    //   [10]    SEED_STAGED  MODE 里那一位的回读
    //   [11]    SEED_OVF     收满之后还有人写（安全世界那侧的 bug，锁存）
    //   [31:16] 非安全世界写种子口被拒的次数（饱和）
    wire [31:0] r_seedstat = {seed_viol, 4'd0, seed_ovf, seed_staged, seed_lock,
                              seed_ready, 3'd0, seed_wcnt};

    // ================= 端口归属 =================
    always @(*) begin
        // 擦除机接管写口：两块 BRAM 同一个地址、同时写 0，8192 拍走完。
        // 两块并行是因为它们各自有独立的写口，没有理由排队。
        if (wiping) begin
            ina_we   = 1'b1;
            ina_addr = wipe_addr;
            ina_din  = 8'd0;
        end else if (pkw_run) begin
            // 打包写的第 1..3 个字节。优先级在逐字节之上是没有歧义的：
            // wr_now 里已经有 !pkw_run，这两条不可能同时成立。
            ina_we   = 1'b1;
            ina_addr = pkw_addr;
            ina_din  = pkw_data[7:0];
        end else begin
            // 打包写的第 0 个字节就地写掉，与逐字节走同一条线。
            ina_we   = wr_indata || wr_indata4;
            ina_addr = in_ptr;
            ina_din  = wr_data[7:0];
        end

        // 输入缓冲的读口。
        // ⚠️ 预读阶段地址要**提前一拍**：同步读的数据下一拍才出来，
        //    照着 pre_cnt 给地址的话每个字节会被读两遍、后一半直接丢掉
        //    （表现是 ek 整个不对，而 Decaps 因为不走预读所以照样过）。
        //    喂流阶段不用提前，那里靠 fb_wait 等那一拍。
        // 从金库取 dk 时，输入缓冲里装的只有 c，所以喂到第 fp 个字节时
        // 该读的是 inbuf[fp - dklen]（前面 dklen 个字节由金库供）。
        inb_addr = (state == S_PRE)
                   ? (fb_wait ? 13'd0 : ({6'd0, pre_cnt} + 13'd1))
                   : (take_dk ? (fp - dklen[12:0]) : fp);

        if (wiping) begin
            outa_we   = 1'b1;
            outa_addr = wipe_addr;
            outa_din  = 8'd0;
        end else begin
            // 存 dk 的那一趟，超过 ek 长度的字节**不进输出缓冲** ——
            // 它们进金库（下面那块），所以软件那边一个字节都读不到。
            outa_we   = (state == S_RUN) && core_ov && !out_to_vault;
            outa_addr = ocnt[12:0];
            outa_din  = core_od;
        end

        // 同步读要提前一拍：这一拍读命中就把地址推到下一个。
        // 打包读期间地址由那台小机器给（out_rd + 已收下的个数）。
        // ⚠️ 地址要**领先一拍**：B 口慢一拍，这一拍驱动的地址是下一拍才拿得到
        // 的字节。pkr_i 是"已经收下几个"，所以要驱动的是 out_rd + pkr_i + 1。
        // 第一版写成 out_rd + pkr_i，于是第 1 个字节被读了两遍、第 3 个从来
        // 没被读到 —— 输出整段错位，而错位后的 ek 看起来完全合法。
        outb_addr = pkr_run ? (out_rd + {9'd0, pkr_i} + 13'd1)
                  : rd_out4 ? (out_rd + 13'd1)
                            : (out_rd + {12'd0, rd_outdata});

        // ---- 金库的两个口 ----
        // 写：擦除时归擦除机；否则只在"存 dk 且已经过了 ek 那一段"时写。
        // 地址就是 {槽号, 槽内偏移} 的拼接 —— 跨度取 4096 就是为了这里
        // 不需要乘法。
        if (wiping) begin
            exp_we    = 1'b1;
            exp_waddr = wipe_addr[11:0];
            exp_din   = 8'd0;
        end else begin
            // 展开区的擦除机优先级最高（S_FIN 无条件启动，见它的声明处）
            exp_we    = exp_wiping || ((state == S_RUN) && core_ov && out_to_vault);
            exp_waddr = exp_wiping ? exp_waddr_w : dk_off[11:0];
            exp_din   = exp_wiping ? 8'd0 : core_od;
        end
        exp_raddr = fp[11:0];
    end

    // ================= 时序 =================
    integer unused_i;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            aw_got <= 1'b0; w_got <= 1'b0;
            aw_addr_r <= 8'd0; aw_prot_r <= 3'd0;
            w_data_r <= 32'd0; w_strb_r <= 4'd0;
            f_bvalid <= 1'b0; f_bresp <= RESP_OKAY;
            f_rvalid <= 1'b0; f_rresp <= RESP_OKAY; f_rdata <= 32'd0;
            mode <= 2'd0; pset <= 2'd1;
            chain <= 1'b0; chain_ph <= 1'b0;
            exp_wiping <= 1'b0; exp_waddr_w <= 12'd0;
            seed_staged <= 1'b0;
            // 复位是唯一能把闩锁放开的事件。（dk_lock 已随 V-04 删除。）
            seed_lock <= 1'b0;
            seed_stage <= 512'd0; seed_wcnt <= 5'd0;
            seed_err <= 1'b0; seed_viol <= 16'd0; seed_ovf <= 1'b0;
            in_ptr <= 13'd0; out_len <= 13'd0; out_rd <= 13'd0; ocnt <= 14'd0;
            pkw_run <= 1'b0; pkw_i <= 2'd0; pkw_data <= 24'd0; pkw_addr <= 13'd0;
            pkr_run <= 1'b0; pkr_i <= 3'd0; pkr_d <= 24'd0;
            zero_pulse <= 1'b0;
            wiping <= 1'b0; wipe_addr <= 13'd0; zall_d <= 1'b0;
            param_err <= 1'b0;
            state <= S_IDLE; pre_cnt <= 7'd0; fp <= 13'd0; kickdly <= 2'd0;
            seed_a <= 256'd0; seed_b <= 256'd0;
            fb_r <= 8'd0; fb_v <= 1'b0; fb_wait <= 1'b0;
            kg_start <= 1'b0; en_start <= 1'b0; de_start <= 1'b0;
            run_done <= 1'b0;
        end else begin
            zero_pulse <= 1'b0;
            kg_start <= 1'b0; en_start <= 1'b0; de_start <= 1'b0;

            // ---------- BRAM 擦除机 ----------
            // 上升沿启动。fw_tampered 是锁存电平，所以 tamper 之后只会启动一次，
            // 擦完 WIPING 就落下来 —— 用电平的话它永远落不下来。
            zall_d <= zeroize_all;
            if (zeroize_all && !zall_d) begin
                wiping    <= 1'b1;
                wipe_addr <= 13'd0;
            end else if (wiping) begin
                // 最后一个地址那一拍 ina_we 仍为高（组合自 wiping），
                // 所以 0x1FFF 也真的被写了 0，一个字节都不留。
                if (wipe_addr == 13'h1FFF) wiping <= 1'b0;
                else                       wipe_addr <= wipe_addr + 13'd1;
            end

            if (zeroize_all) begin
                // 指针与并行寄存器一拍清掉；BRAM 交给上面那台擦除机。
                in_ptr <= 13'd0; out_len <= 13'd0; out_rd <= 13'd0;
                ocnt   <= 14'd0;
                // ⚠️ 两台打包机也要当场停。漏了它们的后果是：擦除已经开始、
                // in_ptr 已经清零，而 pkw 还按旧地址把剩下几个字节写进刚被
                // 擦过的缓冲区 —— 擦完之后缓冲区里躺着三个字节的旧输入。
                // pkr 那边则会在擦除中途把读到的字节拼出来交给软件。
                pkw_run <= 1'b0; pkw_i <= 2'd0; pkw_data <= 24'd0;
                pkr_run <= 1'b0; pkr_i <= 3'd0; pkr_d <= 24'd0;
                // 展开区交给 BRAM 擦除机；相位与它的擦除机一起归零。
                chain_ph <= 1'b0; exp_wiping <= 1'b0;
                seed_a <= 256'd0; seed_b <= 256'd0;
                // 暂存的那份种子也是秘密，zeroize 当然要擦掉它。
                // **seed_lock 不在这里** —— 与 dk_lock 同理，擦秘密不等于撤防线。
                seed_stage <= 512'd0; seed_wcnt <= 5'd0;
                state  <= S_IDLE; run_done <= 1'b0;
                param_err <= 1'b0; seed_err <= 1'b0;
                fb_v <= 1'b0; fb_wait <= 1'b0;
            end

            // ---------- 展开区擦除机（S_FIN 无条件启动）----------
            // 4096 个地址逐个写零。最后一个地址那一拍 exp_we 仍为高
            // （组合自 exp_wiping），所以 0xFFF 也真的被写了 0。
            if (exp_wiping) begin
                if (exp_waddr_w == 12'hFFF) exp_wiping <= 1'b0;
                else                        exp_waddr_w <= exp_waddr_w + 12'd1;
            end

            // ---------- IN_DATA4 的写出机（4 拍）----------
            // ⚠️ **必须在 `if (wr_now)` 之外**。wr_now 里有 !pkw_run，
            // 所以这台机器一旦跑起来 wr_now 就恒为假 —— 把推进写在里面的话
            // 它一步也走不动，第 1..3 个字节永远写不进去，而第 0 个已经进了
            // 缓冲、in_ptr 也已经 +4：表现是**每 4 个字节里有 3 个是旧内容**，
            // 长度校验还照样通过。（第一版就是这么写的。）
            if (pkw_run) begin
                pkw_data <= {8'd0, pkw_data[23:8]};
                pkw_addr <= pkw_addr + 13'd1;
                pkw_i    <= pkw_i + 2'd1;
                if (pkw_i == 2'd2) pkw_run <= 1'b0;   // 刚写完第 3 个字节
            end

            // ---------- 写 ----------
            if (f_awvalid && f_awready) begin
                aw_got <= 1'b1; aw_addr_r <= f_awaddr; aw_prot_r <= f_awprot;
            end
            if (f_wvalid  && f_wready)  begin w_got  <= 1'b1; w_data_r  <= f_wdata;
                                              w_strb_r <= f_wstrb; end
            if (wr_now) begin
                aw_got <= 1'b0; w_got <= 1'b0;
                // 擦除期间拒绝一切写，而且**明确回 SLVERR**。
                // 静默丢弃是这里最危险的选项：软件会以为 IN_DATA 灌进去了，
                // 实际 in_ptr 一步没动，接着按错误的长度启动 —— 出来的是
                // 一个安静的错误结果。读仍然放行，否则软件没法轮询 WIPING。
                f_bvalid <= 1'b1;
                // ⚠️ 种子口的两种拒绝**不进这个表达式** —— 它们回 OKAY。
                // 只有"擦除期间写"仍回 SLVERR，那是既有行为：软件本来就
                // 被要求先轮询 WIPING 再写，而它是**读**得出来的。
                f_bresp  <= wiping ? RESP_SLVERR : RESP_OKAY;
                // 非安全世界写种子口：留痕。RAZ/WI 之后被拒的访问在总线上
                // 什么都不留，而这一条恰恰是最该留下的 —— 它意味着有人在
                // 试着自己塞种子。计数出口在 SEED_STAT[31:16]，
                // 本从机 SECURE_ONLY=1 时只有安全世界读得到。
                // 两种被拒的种子写都留痕：非安全世界那类进 SEED_STAT[31:16]，
                // 收满后多写那类进 SEED_STAT[11]（一位就够，它只该是 bug）。
                if (wr_seed_deny && (seed_viol != 16'hFFFF)) begin
                    seed_viol <= seed_viol + 16'd1;
                end
                if (wr_seed_full) begin
                    seed_ovf <= 1'b1;
                end
                // ---- IN_DATA4：起一趟 4 拍的写 ----
                // 放在 case 之外，因为 case 判的是 wr_addr[5:2]，而 0x40 的
                // [5:2] 是 0（A_VERSION）—— 落进 case 只会什么都不做。
                if (wr_indata4) begin
                    // in_ptr 一次推进 4：len_ok 与 IN_PTR 回读要立刻反映
                    // "这 4 个字节已经算数了"。真正的写由 pkw 接着做，
                    // 而 wr_now 的 !pkw_run 保证这期间没有别的写能插进来。
                    in_ptr   <= in_ptr + 13'd4;
                    pkw_run  <= 1'b1;
                    pkw_i    <= 2'd0;
                    pkw_data <= wr_data[31:8];
                    pkw_addr <= in_ptr + 13'd1;
                end

                if (wr_strb[0] && !wiping && !wr_addr[6]) begin
                    case (wr_addr[5:2])
                    A_CTRL: begin
                        if (wr_data[1]) zero_pulse <= 1'b1;
                        /* [4] 原来是 DK_LOCK（一次性闩锁，强制 dk 留在 PL）。
                         * **已随 V-04 删除**：它守在与"密钥归 TEE"相反的方向。
                         * 关闭私钥导出的闸门改落在 TEE 策略侧。
                         * 这里不写 case —— 写它不再有任何效果，也不给专用
                         * 错误码，那等于向调用方确认这里曾经有个闩。 */
                        // [5] SEED_LOCK：同样一次性、同样没有清零路径。
                        // ⚠️ **不连动 DK_LOCK**（文件头④）：两把闩守的方向
                        // 相反，DK_LOCK 是待删的机制，别把它焊得更死。
                        if (wr_data[5]) seed_lock <= 1'b1;
                        // [6] SEED_CLR：作废暂存的那份种子。给安全世界一条
                        // 显式的"这份不要了"的路 —— 否则它只能靠再写满 16 个
                        // 字来覆盖，而写满之前那份半新半旧的东西是可用的。
                        if (wr_data[6]) begin
                            seed_stage <= 512'd0;
                            seed_wcnt  <= 5'd0;
                        end
                        if (wr_data[2]) in_ptr  <= 13'd0;
                        if (wr_data[3]) out_rd  <= 13'd0;
                        // START 只在空闲时有效，且要放在最后判 ——
                        // 同一拍写 IN_RST|START 的语义是"清指针再启动"
                        if (wr_data[0] && (state == S_IDLE) && !zeroize_all
                            && !wiping_any) begin
                            /* ⚠️ exp_wiping 必须走**这条拒绝路径**，不能放进
                             * 上面那个外层守卫。放外层的后果是一次 START 被
                             * 静默丢弃，而上一次的 DONE 与 OUT_LEN 还留着 ——
                             * 软件轮询到 DONE=1、读出上一次的结果，当成这一次的。
                             * 展开区擦除现在**每次运算之后都跑**（4096 拍），
                             * 所以"紧接着再发一条命令"是常态，不是边角情况。
                             * 这个 bug 第一版就踩了：链式 Decaps 的 K 对不上、
                             * 而"同一份种子生出第二把密钥"—— 同一个成因。 */
                            if (!params_ok || !len_ok || !slot_ok
                                || !seed_gate_ok || exp_wiping) begin
                                // 参数非法**或输入没喂够**：置错误位，
                                // **不启动任何核**。
                                // 不启动这一点比报错更要紧 —— 启动了再报错
                                // 的话，核已经按一个不存在的参数集开始收数了。
                                //
                                // 欠填这一条尤其要在启动前挡住：喂不满时
                                // 并行口会取到缓冲区的残留（冷启动后是全 0），
                                // KeyGen 的 z 就成了可预测值，而输出看起来
                                // 完全合法。见 need_len 处的说明。
                                // PARAM_ERR 是"这次 START 被拒"的总括位；
                                // 种子没备好那一类另外点亮 SEED_ERR，
                                // 因为处置不同（见 r_status 处的说明）。
                                param_err <= 1'b1;
                                seed_err  <= !seed_gate_ok;
                                // ⚠️ 上一次运行的 DONE 与 OUT_LEN 必须一起清掉。
                                // **这一条是上板才发现的**：仿真里每条用例都从
                                // 复位开始，OUT_LEN 本来就是 0，所以"拒绝之后
                                // 留着上一次的结果"这个形状根本没出现过。
                                // 板上是连着跑的 —— 于是软件轮询到 DONE=1、
                                // 读出 OUT_LEN=2432，拿着**上一次**的输出当成
                                // 这一次的结果。比不报错更糟：它看起来成功了。
                                // 一次 START 尝试就作废上一次的结果，
                                // 不管这次是否被接受。
                                run_done <= 1'b0;
                                out_len  <= 13'd0;
                                out_rd   <= 13'd0;
                            end else begin
                                param_err <= 1'b0;
                                seed_err  <= 1'b0;
                                out_len <= 13'd0;
                                out_rd  <= 13'd0;
                                // ⚠️ ocnt 必须和 out_len 一起清。漏了这一条的
                                // 症状极具误导性：**第一次运行完全正确**
                                // （复位后 ocnt 本来就是 0），从第二次起输出
                                // 缓冲的写地址从上一次的末尾接着走，读出来的
                                // 全是垃圾。仿真里表现为"KeyGen 对、Encaps 错"，
                                // 很容易去怀疑 Encaps 核本身。
                                ocnt    <= 14'd0;
                                pre_cnt <= 7'd0;
                                fp      <= 13'd0;
                                fb_v    <= 1'b0;
                                fb_wait <= 1'b1;   // 让输入缓冲的同步读跟上
                                run_done <= 1'b0;
                                // ---- 暂存的种子在这里交班，并**当场作废** ----
                                // 搬进 d/z 之后暂存清零、字计数归零：一份种子
                                // 只生一把密钥（文件头③）。清零与搬运同一拍，
                                // 中间没有"既在暂存里、又在 d/z 里"的窗口。
                                if (use_staged) begin
                                    seed_a     <= seed_stage[255:0];
                                    seed_b     <= seed_stage[511:256];
                                    seed_stage <= 512'd0;
                                    seed_wcnt  <= 5'd0;
                                end
                                // Decaps 没有并行口要预读。少了这个判断，
                                // pre_cnt 会一路数到回绕才碰巧退出（白跑 128 拍）。
                                // 走暂存种子的 KeyGen 同样不需要预读 ——
                                // d/z 上一行已经装好了，直接去 S_KICK。
                                state   <= (use_staged || (mode == M_DECAPS))
                                           ? S_KICK : S_PRE;
                            end
                        end
                    end
                    A_MODE: begin
                        mode <= wr_data[1:0]; pset <= wr_data[3:2];
                        chain        <= wr_data[4];
                        seed_staged  <= wr_data[10];
                    end
                    A_INDATA: if (state == S_IDLE) in_ptr <= in_ptr + 13'd1;
                    // 种子暂存口。到这里的写已经过了 wr_seed 的三道判定
                    // （地址、AxPROT[1]==0、还没收满），非安全的那一路在
                    // 上面已经回了 SLVERR 且**根本走不到这里**。
                    A_SEEDDATA: if (wr_seed) begin
                        seed_stage[seed_wcnt[3:0]*32 +: 32] <= wr_data;
                        seed_wcnt <= seed_wcnt + 5'd1;
                    end
                    default: ;
                    endcase
                end
            end
            if (f_bvalid && f_bready) f_bvalid <= 1'b0;

            // ---------- 读 ----------
            // ---------- OUT_DATA4 的读入机（4 拍）----------
            // 与写那台同理，**必须在 AR 握手之外**：arready 里有 !pkr_run。
            if (pkr_run) begin
                pkr_i <= pkr_i + 3'd1;
                case (pkr_i)
                3'd1: pkr_d[15:8]  <= outb_dout;
                3'd2: pkr_d[23:16] <= outb_dout;
                default: begin
                    // pkr_i == 3：这一拍 outb_dout 是第 4 个字节，齐了。
                    f_rdata  <= {outb_dout, pkr_d[23:0]};
                    f_rvalid <= 1'b1;
                    f_rresp  <= RESP_OKAY;
                    out_rd   <= out_rd + 13'd4;
                    pkr_run  <= 1'b0;
                end
                endcase
            end

            if (f_arvalid && f_arready) begin
                // 打包读这一拍**不给 RVALID** —— 字节还没收齐。
                f_rvalid <= !rd_out4; f_rresp <= RESP_OKAY;
                if (rd_out4) begin
                    // B 口是慢一拍的同步读，而空闲时地址就停在 out_rd 上，
                    // 所以此刻 outb_dout 已经是第 0 个字节，直接收下。
                    // 同一拍组合逻辑已经把地址推到 out_rd+1。
                    pkr_d[7:0] <= outb_dout;
                    pkr_run    <= 1'b1;
                    pkr_i      <= 3'd1;
                end
                case (f_araddr[5:2])
                A_VERSION: f_rdata <= VERSION;
                A_STATUS:  f_rdata <= r_status;
                // MODE 原来只回读 mode/pset 两个字段，DK_TO_SLOT /
                // DK_FROM_SLOT / SLOT 写进去就再也读不回来 —— 驱动没法核对
                // 自己写对了没有（登记表 DOC-3 记的就是这一条）。
                // 现在整字回读，含新加的 SEED_STAGED。
                A_MODE:    f_rdata <= {21'd0, seed_staged, 4'd0,
                                       1'b0, chain, pset, mode};
                A_INPTR:   f_rdata <= {19'd0, in_ptr};
                A_OUTDATA: f_rdata <= wiping_any ? 32'd0 : {24'd0, outb_dout};
                A_OUTLEN:  f_rdata <= {19'd0, out_len};
                A_OUTRD:   f_rdata <= {19'd0, out_rd};
                A_VIOL:    f_rdata <= {viol_rd_count, viol_wr_count};
                // 译码违规数。注意这**不是**本核的防火墙拒的，是上游译码器
                // 判"根本没有这个地址"的次数 —— 借这个 SECURE_ONLY=1 的
                // 窗口出口，普通世界读不到。
                A_XBAR_VIOL: f_rdata <= xbar_viol_count;
                // [15:0] 哪些槽装了东西；
                // [16] 私钥外泄闩锁（1 = KeyGen 再也不会把 dk 交出来）；
                // [17] 种子闩锁（1 = KeyGen 的 d‖z 永远取自安全世界暂存口）。
                // 16 个槽之后 pset 要 32 位，一个寄存器装不下 valid+pset+lock，
                // 所以拆两个：KEYSTAT 放有效位与闩锁，KEYPSET 放每槽的参数集。
                // ⚠️ [16] 与 [17] **互相独立**：两把闩守的方向相反（见文件头④），
                //    四种组合都是合法状态，别在软件里假设其中任何一种蕴含关系。
                // KEYSTAT 现在**只报与密钥无关的健康位**（V-06）：
                //   [0] EXP_WIPING —— 展开区正在擦
                //   [1] SEED_LOCK
                // 原来这里有 16 位"哪些槽装了东西"与一个 dk_lock，
                // 那是"PL 有状态且状态可查"的对外暴露 —— 不泄密钥字节，
                // 但泄存在性、数量、参数集。槽没有了，这些位也就没有了。
                A_KEYSTAT: f_rdata <= {30'd0, seed_lock, exp_wiping};
                // KEYPSET（每槽的参数集）随槽一起删除，读回 0。
                A_KEYPSET: f_rdata <= 32'd0;
                A_PARAM0:  f_rdata <= 32'h2000_2000;    // 两块 8 KB 缓冲
                A_SEEDSTAT: f_rdata <= r_seedstat;
                // ⚠️ SEED_DATA **没有读回路径**，这一条写出来是为了让"读它
                // 恒为 0"是一句可以在代码里指着看的话，而不是靠 default 兜底。
                // 种子字节在本模块里只存在于 seed_stage / seed_a / seed_b 三个
                // 寄存器上，它们一个都不出现在任何 f_rdata 的赋值里。
                A_SEEDDATA: f_rdata <= 32'd0;
                default:   f_rdata <= 32'd0;
                endcase
                // ⚠️ 0x40/0x44 的 [5:2] 分别是 0 与 1，会撞上 A_VERSION 与
                // A_CTRL。这一行把打包窗口那两个地址的 f_rdata 覆盖掉：
                // IN_DATA4 是只写口，读它回 0；OUT_DATA4 的值由上面那台
                // 读入机在第 4 拍给，这里先压成 0，免得握手这一拍先把
                // VERSION/CTRL 的值放进去（RVALID 那时还没抬，但压掉更省事）。
                if (f_araddr[6]) f_rdata <= 32'd0;
                if (rd_outdata) out_rd <= out_rd + 13'd1;
            end
            if (f_rvalid && f_rready) f_rvalid <= 1'b0;

            // ---------- 运行 ----------
            if (!zeroize_all) begin
                case (state)
                S_IDLE: ;

                // 把 d/z（或 m）从缓冲区头部读进并行寄存器。
                //
                // ⚠️ 字节序：**先写进来的字节落在最低位**（右移进来）。
                // 三个核的 d_in / z_in / m_in 都是这个约定（cocotb 里写的是
                // int.from_bytes(d, "little")），而字节**流**那一路
                // （ek/dk/c）是先出的字节在前 —— 两个方向不一样，
                // 装反了的表现是 ek 整个不对，但 Decaps 照样过（它不用并行口）。
                S_PRE: begin
                    if (fb_wait) begin
                        fb_wait <= 1'b0;
                    end else begin
                        if (pre_cnt < 7'd32)
                            seed_a <= {inb_dout, seed_a[255:8]};
                        else
                            seed_b <= {inb_dout, seed_b[255:8]};
                        if (pre_cnt + 7'd1 == pre_len) begin
                            state <= S_KICK;
                        end else begin
                            pre_cnt <= pre_cnt + 7'd1;
                        end
                    end
                end

                S_KICK: begin
                    /* ⚠️ 这里必须是 mode_eff，不是 mode。链式运算的展开
                     * 相位里 mode 还是 M_DECAPS，用 mode 会去启动 Decaps 核 ——
                     * 而这一相位要跑的是 KeyGen。症状是"链式 Decaps 一直不完成"
                     * 或"解出来的 K 对不上"，都不指向这一行。第一版就踩了。 */
                    case (mode_eff)
                    M_KEYGEN: kg_start <= 1'b1;
                    M_ENCAPS: en_start <= 1'b1;
                    default:  de_start <= 1'b1;
                    endcase
                    kickdly <= 2'd3;
                    // Encaps 的 ek 从 32 开始；Decaps 从 0 开始
                    fp      <= (mode_eff == M_ENCAPS) ? 13'd32 : 13'd0;
                    fb_v    <= 1'b0;
                    fb_wait <= 1'b1;
                    state   <= S_RUN;
                end

                S_RUN: begin
                    // 头三拍不看 core_dn：这段时间里 start 才刚拉高、
                    // 核还没来得及把上一次的 done 清掉（见 kickdly 的声明处）
                    if (kickdly != 2'd0) kickdly <= kickdly - 2'd1;

                    // ---- 喂输入流（三拍一个字节，同步读要等一拍）----
                    if (fb_v && feed_fire) begin
                        fb_v <= 1'b0; fp <= fp + 13'd1; fb_wait <= 1'b1;
                    end else if (fb_wait) begin
                        fb_wait <= 1'b0;
                    end else if (!fb_v && ({1'b0, fp} < feed_len)) begin
                        // dk 从金库来还是从输入缓冲来 —— 对核而言毫无区别，
                        // 它只看到一条字节流。这正是这个改动能这么小的原因。
                        fb_r <= feed_from_vault ? exp_dout : inb_dout;
                        fb_v <= 1'b1;
                    end

                    // ---- 收输出流 ----
                    // ocnt 数核吐出来的**全部**字节；out_len 只数**软件读得到**的
                    // 那部分。存 dk 的那一趟，两者在 ek 结束处分岔。
                    if (core_ov) begin
                        ocnt <= ocnt + 14'd1;
                        if (!out_to_vault) out_len <= out_len + 13'd1;
                    end

                    if (core_dn && (kickdly == 2'd0)) begin
                        run_done <= 1'b1;
                        state    <= S_FIN;
                    end
                end

                S_FIN: begin
                    out_rd <= 13'd0;
                    if (chain_run && !chain_ph) begin
                        // ---- 展开相位跑完，接着跑运算相位 ----
                        // **不擦展开区**：里面正是运算相位要用的那份 dk。
                        // 计数器全部归零，就像一次新的 START —— 但 in_ptr
                        // 不动（软件送的密文还在输入缓冲里等着）。
                        chain_ph <= 1'b1;
                        run_done <= 1'b0;
                        out_len  <= 13'd0;
                        ocnt     <= 14'd0;
                        pre_cnt  <= 7'd0;
                        fp       <= 13'd0;
                        fb_v     <= 1'b0;
                        fb_wait  <= 1'b0;
                        state    <= S_KICK;   // dk 在展开区，不用再走 S_PRE
                    end else begin
                        // ---- 一次运算真正结束 ----
                        // **展开区无条件擦**（§7.2 条件①）：由硬件在这里触发，
                        // 不由任何软件决定。软件唯一能做的是等 EXP_WIPING 落下。
                        //
                        // ⚠️ 只擦展开区，**不碰输出缓冲** —— 软件还没读走结果。
                        //    zeroize 那台擦除机才盖两块 8 KB 缓冲。
                        chain_ph    <= 1'b0;
                        exp_wiping  <= 1'b1;
                        exp_waddr_w <= 12'd0;
                        state <= S_IDLE;
                    end
                end

                default: state <= S_IDLE;
                endcase
            end
        end
    end

    wire _unused = &{1'b0, f_arprot, wr_prot[2], wr_prot[0], wr_strb[3:1], core_ol,
                     kg_ol, en_ol, de_ol, 1'b0};

endmodule

`default_nettype wire
