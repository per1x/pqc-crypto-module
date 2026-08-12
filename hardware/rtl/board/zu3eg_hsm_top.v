// zu3eg_hsm_top —— 上板顶层：PS 的 M_AXI_HPM0_LPD 接到 PL 里的四个密码从机
//
//   zynq_ultra_ps_e ──M_AXI_HPM0_LPD(AXI4-Lite,32bit)──▶ axi4lite_xbar
//        │ pl_clk0 = 150 MHz                                  │
//        │                                                    ├─ 0x8000_0000 trng_axi
//        └─ BUFGCE_DIV /2 ──▶ 75 MHz 给全部密码核             ├─ 0x8001_0000 key_vault_axi
//                                                             ├─ 0x8002_0000 sym_axi
//                                                             ├─ 0x8003_0000 mlkem_axi
//                                                             ├─ 0x8004_0000 金丝雀（只用来被拒）
//                                                             └─ 0x8005_0000 风扇观测口
//
//   另有一条**完全不经过 AXI** 的通路：SYSMONE4 ──▶ fan_ctrl ──▶ AA11。
//   风扇不依赖软件，理由见 fpga/fan_ctrl/fan_ctrl.v 的文件头。
//
// ============================================================================
// 【时钟：为什么要分频，为什么用 BUFGCE_DIV 而不是 MMCM】
// ============================================================================
// 厂家 BOOT.BIN 的 psu_init 把 pl_clk0 配成 **150 MHz**（PSU__CRL_APB__
// PL0_REF_CTRL__FREQMHZ=150），而本设计里最慢的核 mlkem_decaps 单独综合
// 只收敛到 **108.5 MHz**。直接用 150 MHz 必然过不了时序。
//
// 分频到 **75 MHz**，对最慢的核留了 45% 余量 —— 接上总线、加了时钟树之后
// 关键路径一定比单独综合时长，这个余量是留给那部分的。性能在这个项目里
// 不是目标（原型验证），换稳定收敛很划算。
//
// 用 BUFGCE_DIV 而不是 MMCM：MMCM 要等 lock、要排复位时序，多一组会出错的
// 状态；BUFGCE_DIV 是纯分频，上电即有，没有 lock 信号要管。
//
// ⚠️ **BUFGCE_DIV 是 UltraScale+ 的原语**，这是整个仓库里唯一一处厂商原语，
//    而且只在这个板级顶层里 —— 算法核仍然是可移植的纯 RTL。
//
// ============================================================================
// 【复位】
// ============================================================================
// PS 的 pl_resetn0 是异步的，且与 PL 时钟无关。这里过两级同步器再用，
// 否则复位释放的那一拍会在不同触发器上前后差一拍（亚稳态 + 偏斜），
// 表现是"偶尔上电起不来"这类最难查的问题。
//
// ============================================================================
// 【SECURE_ONLY 在这一版里为什么是 0 —— 以及那条「金丝雀」是干什么的】
// ============================================================================
// 防火墙的 AxPROT 门控要求 AxPROT[1]==0（secure）。而**板上的 Linux 跑在
// 非安全世界**，它经 /dev/mem 发出的每一笔事务 AxPROT[1] 都是 1。
// 四个功能从机若都设成 SECURE_ONLY=1，Linux 一个寄存器也读不到，
// 这一版 bitstream 就只能证明"全都拒绝"，证明不了密码算法对不对。
//
// 所以这一版：**四个功能从机 SECURE_ONLY=0**（让 Linux 能跑 KAT），
// 另加**槽 4 一个 SECURE_ONLY=1 的金丝雀实例** —— 它和槽 1 是同一个
// key_vault_axi 模块，唯一区别就是那个参数。于是在真硬件上：
//
//   · 槽 1..3 能用 → 算法、密钥仓、边界不变量都能实测；
//   · 槽 4 的每一次访问都必须 DECERR → **这就是 AxPROT 门控在真硬件、
//     真非安全 master 下确实生效的证据**，而不只是仿真里的结论。
//
// 生产版本里四个从机全都是 SECURE_ONLY=1，由安全世界（OP-TEE）驱动。
// 那一步归另一条线，这里不越界。
//
// ============================================================================
// 【为什么没有 pqc_accel_axi】
// ============================================================================
// 那个模块的批量数据走 AXI4-Stream，要从 PS 驱动它得配一路 DMA —— 那是独立
// 的一块工作。它里面的 ntt_core 与 sha3_core/keccak_f1600 **并没有因此
// 失去验证**：mlkem_axi 下面的三个 ML-KEM 核每一次运算都要跑满 SHA-3 海绵
// （G/PRF/XOF/H）和多次 NTT，ACVP 向量对上就等于这两个核在真硬件上是对的。
// 但这是**间接覆盖**，不是独立 KAT，报告里要照这样说。
`default_nettype none

// ============================================================================
// 【唯一的外部管脚：风扇 PWM】
// ============================================================================
// 密码那一半与外界的全部往来仍然只经过 PS（AXI、时钟、复位都从 PS 来），
// 一个管脚约束都不需要。**整个设计唯一的外部管脚是风扇 PWM（AA11）**，
// 约束在 hardware/syn/constraints/board_pins.xdc —— 管脚号抄自厂家
// system.xdc，不是猜的。
//
// 风扇和密码核在同一个 bitstream 里，是因为 **PL 只有一份**：运行时载进去的
// 那一个 bitstream 就是全部，分成两个"设计"没有意义（载了谁另一个就没了）。
// 但**代码是分开的**（fpga/fan_ctrl/ vs hardware/rtl/）：风扇不碰密码的任何
// 信号，密码也不碰风扇的，两边只共用时钟和复位。
module zu3eg_hsm_top (
    output wire fan          // AA11，低=转（见 fpga/fan_ctrl/fan_ctrl.v）
);
    // ================= PS =================
    wire        pl_clk0;
    wire        pl_resetn0;

    wire [39:0] m_awaddr;
    wire [2:0]  m_awprot;
    wire        m_awvalid, m_awready;
    wire [31:0] m_wdata;
    wire [3:0]  m_wstrb;
    wire        m_wvalid,  m_wready;
    wire [1:0]  m_bresp;
    wire        m_bvalid,  m_bready;
    wire [39:0] m_araddr;
    wire [2:0]  m_arprot;
    wire        m_arvalid, m_arready;
    wire [31:0] m_rdata;
    wire [1:0]  m_rresp;
    wire        m_rvalid,  m_rready;

    // ⚠️⚠️ **M_AXI_HPM0_LPD 是 AXI4，不是 AXI4-Lite。**
    // 第一版把它当 Lite 接，于是 bid / rid / rlast 这三个「由我驱动、送回 PS」
    // 的信号全部悬空 —— **PS 在等 rlast 来结束这笔读突发，等不到就永远不返回**，
    // CPU 卡死在那一条读指令上（串口能看到 RCU 报某个 CPU 死在用户进程里，
    // 而其它 CPU 还活着）。综合、布线、时序、bitstream 全部正常，
    // 载进板子也显示 operating —— 这个 bug 只在真机上现形。
    //
    // AXI4-Lite 本质上就是「突发长度恒为 1 的 AXI4」，所以：
    //   rlast = rvalid（单拍就是最后一拍）
    //   bid / rid 回显对应的 awid / arid
    wire [15:0] m_awid, m_arid;
    wire [7:0]  m_awlen, m_arlen;
    wire [2:0]  m_awsize, m_arsize;
    wire [1:0]  m_awburst, m_arburst;
    wire        m_wlast;

    // ⚠️ 这段回显逻辑用到 clk_sys，所以它**搬到时钟那一段之后**去了 ——
    //    就在 BUFGCE_DIV 下面。同一个模块里"先用后声明"正是让板子挂死两次的
    //    那类写法：Icarus 直接报 Unable to bind wire，Vivado 却可能静默接受
    //    （这一版恰好接对了，但那是运气，不是保证）。
    // ================= 时钟与复位 =================
    // ⚠️⚠️ **这一段必须放在 PS 例化之前。**
    // 第一版把它放在后面，于是 PS 的 .maxihpm0_lpd_aclk(clk_sys) 引用了一个
    // 还没声明的名字 —— **Vivado 不报错**，而是给那个端口新建了一条同名的
    // 无驱动网络。综合、布线、时序全部通过，bitstream 也正常生成并载进了
    // 板子，但 PS 的 AXI 主口没有时钟：CPU 发出的第一笔读永远不返回，
    // 整机 wedge，连 sysrq 兜底都跑不起来（实测挂了两次、断电两次）。
    //
    // Icarus 对同样的写法是直接报 "Unable to bind wire"。Vivado 的静默
    // 才是真正危险的地方 —— 所以 impl_bitstream.tcl 里加了一条综合后断言：
    // PS 的每个 AXI 时钟/复位输入都必须有驱动，没有就中止。
    wire clk_sys;
    BUFGCE_DIV #(
        .BUFGCE_DIVIDE   (2),          // 150 MHz → 75 MHz
        .IS_CE_INVERTED  (1'b0),
        .IS_CLR_INVERTED (1'b0),
        .IS_I_INVERTED   (1'b0)
    ) u_div (
        .O   (clk_sys),
        .CE  (1'b1),
        .CLR (1'b0),
        .I   (pl_clk0));

    // AXI4 的响应回显（上面那段搬下来的，因为要用 clk_sys）
    reg  [15:0] awid_r, arid_r;
    always @(posedge clk_sys) begin
        if (m_awvalid && m_awready) awid_r <= m_awid;
        if (m_arvalid && m_arready) arid_r <= m_arid;
    end
    wire [15:0] m_bid   = awid_r;
    wire [15:0] m_rid   = arid_r;
    wire        m_rlast = m_rvalid;

    zynq_ultra_ps_e_0 u_ps (
        .maxihpm0_lpd_aclk (clk_sys),
        .pl_clk0           (pl_clk0),
        .pl_resetn0        (pl_resetn0),

        .maxigp2_awaddr    (m_awaddr),
        .maxigp2_awprot    (m_awprot),
        .maxigp2_awvalid   (m_awvalid),
        .maxigp2_awready   (m_awready),
        .maxigp2_awid      (m_awid),
        .maxigp2_awlen     (m_awlen),
        .maxigp2_awsize    (m_awsize),
        .maxigp2_awburst   (m_awburst),
        .maxigp2_awlock    (),
        .maxigp2_awcache   (),
        .maxigp2_awqos     (),
        .maxigp2_awuser    (),
        .maxigp2_wdata     (m_wdata),
        .maxigp2_wstrb     (m_wstrb),
        .maxigp2_wlast     (m_wlast),
        .maxigp2_wvalid    (m_wvalid),
        .maxigp2_wready    (m_wready),
        .maxigp2_bid       (m_bid),
        .maxigp2_bresp     (m_bresp),
        .maxigp2_bvalid    (m_bvalid),
        .maxigp2_bready    (m_bready),

        .maxigp2_araddr    (m_araddr),
        .maxigp2_arprot    (m_arprot),
        .maxigp2_arvalid   (m_arvalid),
        .maxigp2_arready   (m_arready),
        .maxigp2_arid      (m_arid),
        .maxigp2_arlen     (m_arlen),
        .maxigp2_arsize    (m_arsize),
        .maxigp2_arburst   (m_arburst),
        .maxigp2_arlock    (),
        .maxigp2_arcache   (),
        .maxigp2_arqos     (),
        .maxigp2_aruser    (),
        .maxigp2_rid       (m_rid),
        .maxigp2_rdata     (m_rdata),
        .maxigp2_rresp     (m_rresp),
        .maxigp2_rlast     (m_rlast),
        .maxigp2_rvalid    (m_rvalid),
        .maxigp2_rready    (m_rready));

    // pl_resetn0 是异步的，两级同步器之后再用
    reg rst_n_meta, rst_n_sync;
    always @(posedge clk_sys or negedge pl_resetn0) begin
        if (!pl_resetn0) begin
            rst_n_meta <= 1'b0;
            rst_n_sync <= 1'b0;
        end else begin
            rst_n_meta <= 1'b1;
            rst_n_sync <= rst_n_meta;
        end
    end

    // 再加一道**织构自生的上电复位**：配置完成后 FPGA 的触发器一律是 0
    // （GSR），所以这个计数器必然从 0 开始数，数满 255 拍就放开复位。
    // 有了它，即使 pl_resetn0 因为运行时重配而没有正确释放，设计也能自己
    // 起来 —— 少一个「只在真机上才会暴露」的依赖。
    reg [7:0] porcnt = 8'd0;
    always @(posedge clk_sys) if (porcnt != 8'hFF) porcnt <= porcnt + 8'd1;

    wire rst_n = rst_n_sync & (porcnt == 8'hFF);

    // ================= 篡改输入 =================
    // 板上还没有接真实的开盖/电压/温度检测，这里恒零。
    // **不要**把它接成"软件可写的寄存器" —— 那就等于给攻击者一个开关。
    // 真接的时候是从 PL 管脚进来，接到这一根线上。
    wire tamper = 1'b0;

    // ================= 地址译码 =================
    localparam integer NS = 6;

    wire [8*NS-1:0]  x_awaddr, x_araddr;
    wire [3*NS-1:0]  x_awprot, x_arprot;
    wire [NS-1:0]    x_awvalid, x_awready, x_wvalid, x_wready;
    wire [32*NS-1:0] x_wdata, x_rdata;
    wire [4*NS-1:0]  x_wstrb;
    wire [2*NS-1:0]  x_bresp, x_rresp;
    wire [NS-1:0]    x_bvalid, x_bready, x_arvalid, x_arready;
    wire [NS-1:0]    x_rvalid, x_rready;

    axi4lite_xbar #(.AW(40), .NS(NS), .SEL_LSB(16)) u_xbar (
        .clk(clk_sys), .rst_n(rst_n),
        .s_awaddr(m_awaddr), .s_awprot(m_awprot),
        .s_awvalid(m_awvalid), .s_awready(m_awready),
        .s_wdata(m_wdata), .s_wstrb(m_wstrb),
        .s_wvalid(m_wvalid), .s_wready(m_wready),
        .s_bresp(m_bresp), .s_bvalid(m_bvalid), .s_bready(m_bready),
        .s_araddr(m_araddr), .s_arprot(m_arprot),
        .s_arvalid(m_arvalid), .s_arready(m_arready),
        .s_rdata(m_rdata), .s_rresp(m_rresp),
        .s_rvalid(m_rvalid), .s_rready(m_rready),
        .m_awaddr(x_awaddr), .m_awprot(x_awprot),
        .m_awvalid(x_awvalid), .m_awready(x_awready),
        .m_wdata(x_wdata), .m_wstrb(x_wstrb),
        .m_wvalid(x_wvalid), .m_wready(x_wready),
        .m_bresp(x_bresp), .m_bvalid(x_bvalid), .m_bready(x_bready),
        .m_araddr(x_araddr), .m_arprot(x_arprot),
        .m_arvalid(x_arvalid), .m_arready(x_arready),
        .m_rdata(x_rdata), .m_rresp(x_rresp),
        .m_rvalid(x_rvalid), .m_rready(x_rready));

    // ================= 槽 0：TRNG =================
    wire trng_ready, trng_alarm;
    trng_axi #(.SECURE_ONLY(0)) u_trng (
        .clk(clk_sys), .rst_n(rst_n),
        .s_axi_awaddr(x_awaddr[8*0 +: 8]), .s_axi_awprot(x_awprot[3*0 +: 3]),
        .s_axi_awvalid(x_awvalid[0]), .s_axi_awready(x_awready[0]),
        .s_axi_wdata(x_wdata[32*0 +: 32]), .s_axi_wstrb(x_wstrb[4*0 +: 4]),
        .s_axi_wvalid(x_wvalid[0]), .s_axi_wready(x_wready[0]),
        .s_axi_bresp(x_bresp[2*0 +: 2]), .s_axi_bvalid(x_bvalid[0]),
        .s_axi_bready(x_bready[0]),
        .s_axi_araddr(x_araddr[8*0 +: 8]), .s_axi_arprot(x_arprot[3*0 +: 3]),
        .s_axi_arvalid(x_arvalid[0]), .s_axi_arready(x_arready[0]),
        .s_axi_rdata(x_rdata[32*0 +: 32]), .s_axi_rresp(x_rresp[2*0 +: 2]),
        .s_axi_rvalid(x_rvalid[0]), .s_axi_rready(x_rready[0]),
        .tamper(tamper),
        .trng_ready(trng_ready), .trng_alarm(trng_alarm));

    // ================= 槽 1 与 2：密钥仓 + 对称核 =================
    // 这两个是一体的（密钥从 use 口直接进对称核），所以用 sym_vault_top
    wire vault_tampered;
    sym_vault_top #(.SECURE_ONLY(0)) u_symvault (
        .clk(clk_sys), .rst_n(rst_n), .tamper(tamper),
        .vault_awaddr(x_awaddr[8*1 +: 8]), .vault_awprot(x_awprot[3*1 +: 3]),
        .vault_awvalid(x_awvalid[1]), .vault_awready(x_awready[1]),
        .vault_wdata(x_wdata[32*1 +: 32]), .vault_wstrb(x_wstrb[4*1 +: 4]),
        .vault_wvalid(x_wvalid[1]), .vault_wready(x_wready[1]),
        .vault_bresp(x_bresp[2*1 +: 2]), .vault_bvalid(x_bvalid[1]),
        .vault_bready(x_bready[1]),
        .vault_araddr(x_araddr[8*1 +: 8]), .vault_arprot(x_arprot[3*1 +: 3]),
        .vault_arvalid(x_arvalid[1]), .vault_arready(x_arready[1]),
        .vault_rdata(x_rdata[32*1 +: 32]), .vault_rresp(x_rresp[2*1 +: 2]),
        .vault_rvalid(x_rvalid[1]), .vault_rready(x_rready[1]),

        .sym_awaddr(x_awaddr[8*2 +: 8]), .sym_awprot(x_awprot[3*2 +: 3]),
        .sym_awvalid(x_awvalid[2]), .sym_awready(x_awready[2]),
        .sym_wdata(x_wdata[32*2 +: 32]), .sym_wstrb(x_wstrb[4*2 +: 4]),
        .sym_wvalid(x_wvalid[2]), .sym_wready(x_wready[2]),
        .sym_bresp(x_bresp[2*2 +: 2]), .sym_bvalid(x_bvalid[2]),
        .sym_bready(x_bready[2]),
        .sym_araddr(x_araddr[8*2 +: 8]), .sym_arprot(x_arprot[3*2 +: 3]),
        .sym_arvalid(x_arvalid[2]), .sym_arready(x_arready[2]),
        .sym_rdata(x_rdata[32*2 +: 32]), .sym_rresp(x_rresp[2*2 +: 2]),
        .sym_rvalid(x_rvalid[2]), .sym_rready(x_rready[2]),
        .vault_tampered(vault_tampered));

    // ================= 槽 3：ML-KEM =================
    mlkem_axi #(.SECURE_ONLY(0)) u_mlkem (
        .clk(clk_sys), .rst_n(rst_n),
        .s_axi_awaddr(x_awaddr[8*3 +: 8]), .s_axi_awprot(x_awprot[3*3 +: 3]),
        .s_axi_awvalid(x_awvalid[3]), .s_axi_awready(x_awready[3]),
        .s_axi_wdata(x_wdata[32*3 +: 32]), .s_axi_wstrb(x_wstrb[4*3 +: 4]),
        .s_axi_wvalid(x_wvalid[3]), .s_axi_wready(x_wready[3]),
        .s_axi_bresp(x_bresp[2*3 +: 2]), .s_axi_bvalid(x_bvalid[3]),
        .s_axi_bready(x_bready[3]),
        .s_axi_araddr(x_araddr[8*3 +: 8]), .s_axi_arprot(x_arprot[3*3 +: 3]),
        .s_axi_arvalid(x_arvalid[3]), .s_axi_arready(x_arready[3]),
        .s_axi_rdata(x_rdata[32*3 +: 32]), .s_axi_rresp(x_rresp[2*3 +: 2]),
        .s_axi_rvalid(x_rvalid[3]), .s_axi_rready(x_rready[3]),
        .tamper(tamper));

    // ================= 槽 4：金丝雀（SECURE_ONLY=1）=================
    // 与槽 1 同一个模块，只差 SECURE_ONLY。板上的 Linux 是非安全 master，
    // 对它的每一次读写都必须回 DECERR —— 那就是 AxPROT 门控在真硬件上
    // 生效的直接证据。它的 use 口不接任何东西：这个实例只用来被拒绝。
    key_vault_axi #(.SECURE_ONLY(1), .SLOTS(8), .SLOT_BITS(3), .WORDS(8))
    u_canary (
        .clk(clk_sys), .rst_n(rst_n),
        .s_axi_awaddr(x_awaddr[8*4 +: 8]), .s_axi_awprot(x_awprot[3*4 +: 3]),
        .s_axi_awvalid(x_awvalid[4]), .s_axi_awready(x_awready[4]),
        .s_axi_wdata(x_wdata[32*4 +: 32]), .s_axi_wstrb(x_wstrb[4*4 +: 4]),
        .s_axi_wvalid(x_wvalid[4]), .s_axi_wready(x_wready[4]),
        .s_axi_bresp(x_bresp[2*4 +: 2]), .s_axi_bvalid(x_bvalid[4]),
        .s_axi_bready(x_bready[4]),
        .s_axi_araddr(x_araddr[8*4 +: 8]), .s_axi_arprot(x_arprot[3*4 +: 3]),
        .s_axi_arvalid(x_arvalid[4]), .s_axi_arready(x_arready[4]),
        .s_axi_rdata(x_rdata[32*4 +: 32]), .s_axi_rresp(x_rresp[2*4 +: 2]),
        .s_axi_rvalid(x_rvalid[4]), .s_axi_rready(x_rready[4]),
        .tamper(tamper),
        .use_sel(3'd0), .use_key(), .use_valid(), .vault_tampered());

    // ================= 槽 5：风扇温控（与密码无关）=================
    // ⚠️ **这一路的存在不影响散热的可靠性。** fan_ctrl 的温度直接来自 PL 里的
    //    SYSMONE4，PWM 直接出到 AA11，整条链路只依赖 clk_sys —— AXI 这一路
    //    只是"能读到温度和占空比"的观测口，拔掉它风扇照转。
    //
    // 有这个口是为了能**证明**温控在工作：要在板上看到"空载低占空比、加负载
    // 结温上去占空比跟上"，光靠耳朵不算验证。
    wire [15:0] fan_temp;
    wire        fan_temp_valid, fan_sysmon_to;
    wire [7:0]  fan_cur_duty, fan_ovr_duty;
    wire [2:0]  fan_cur_step;
    wire [15:0] fan_cur_temp;
    wire        fan_forced_full, fan_ovr_en, fan_stuck;
    wire        fan_dbg_req, fan_dbg_valid, fan_dbg_to;
    wire [7:0]  fan_dbg_addr;
    wire [15:0] fan_dbg_data;

    // DCLK_DIV=16 → ADCCLK = 75 MHz/16 = 4.69 MHz，在 SYSMONE4 的上限之内。
    // 这个数跟着 clk_sys 走，改分频比就要改它（见 fan_sysmon.v 文件头 ②）。
    fan_sysmon #(.PERIOD(75_000), .DCLK_DIV(16)) u_fan_sysmon (
        .clk(clk_sys), .rst_n(rst_n),
        .temp_code(fan_temp), .temp_valid(fan_temp_valid),
        .sysmon_timeout(fan_sysmon_to),
        .dbg_req(fan_dbg_req), .dbg_addr(fan_dbg_addr),
        .dbg_data(fan_dbg_data), .dbg_valid(fan_dbg_valid),
        .dbg_timeout(fan_dbg_to));

    fan_ctrl #(.PWM_PERIOD(3000),          // 75 MHz / 3000 = 25 kHz
               .STALE_LIMIT(64_000_000),   // 约 0.85 秒没温度就强制满速
               .STUCK_LIMIT(30_000))       // 30000 次采样 ≈ 30 秒读数不变
    u_fan (
        .clk(clk_sys), .rst_n(rst_n),
        .temp_code(fan_temp), .temp_valid(fan_temp_valid),
        .ovr_en(fan_ovr_en), .ovr_duty(fan_ovr_duty),
        .cur_temp(fan_cur_temp), .cur_duty(fan_cur_duty),
        .cur_step(fan_cur_step), .forced_full(fan_forced_full),
        .sensor_stuck(fan_stuck),
        .fan_pin(fan));

    fan_ctrl_axi u_fan_axi (
        .clk(clk_sys), .rst_n(rst_n),
        .s_axi_awaddr(x_awaddr[8*5 +: 8]), .s_axi_awprot(x_awprot[3*5 +: 3]),
        .s_axi_awvalid(x_awvalid[5]), .s_axi_awready(x_awready[5]),
        .s_axi_wdata(x_wdata[32*5 +: 32]), .s_axi_wstrb(x_wstrb[4*5 +: 4]),
        .s_axi_wvalid(x_wvalid[5]), .s_axi_wready(x_wready[5]),
        .s_axi_bresp(x_bresp[2*5 +: 2]), .s_axi_bvalid(x_bvalid[5]),
        .s_axi_bready(x_bready[5]),
        .s_axi_araddr(x_araddr[8*5 +: 8]), .s_axi_arprot(x_arprot[3*5 +: 3]),
        .s_axi_arvalid(x_arvalid[5]), .s_axi_arready(x_arready[5]),
        .s_axi_rdata(x_rdata[32*5 +: 32]), .s_axi_rresp(x_rresp[2*5 +: 2]),
        .s_axi_rvalid(x_rvalid[5]), .s_axi_rready(x_rready[5]),
        .cur_temp(fan_cur_temp), .cur_duty(fan_cur_duty),
        .cur_step(fan_cur_step), .forced_full(fan_forced_full),
        .sysmon_timeout(fan_sysmon_to), .sensor_stuck(fan_stuck),
        .ovr_en(fan_ovr_en), .ovr_duty(fan_ovr_duty),
        .dbg_req(fan_dbg_req), .dbg_addr(fan_dbg_addr),
        .dbg_data(fan_dbg_data), .dbg_valid(fan_dbg_valid),
        .dbg_timeout(fan_dbg_to));

    wire _unused = &{1'b0, m_awlen, m_arlen,
                     m_awsize, m_arsize, m_awburst, m_arburst,
                     m_wlast, trng_ready, trng_alarm, vault_tampered, 1'b0};

endmodule

`default_nettype wire
