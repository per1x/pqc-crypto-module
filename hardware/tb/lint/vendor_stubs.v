// vendor_stubs.v —— 厂商原语的空壳，**只给 lint / 可综合性检查用，绝不参与综合**
//
// ============================================================================
// 【为什么要有这个文件，而不是把板级顶层从 lint 里排除掉】
// ============================================================================
// zu3eg_hsm_top 例化了 BUFGCE_DIV、zynq_ultra_ps_e_0；fan_sysmon 例化了
// SYSMONE4。Verilator 与 Icarus 手上都没有这些模块，于是报 MODMISSING ——
// 而 Verilator 是**把命令行上的所有文件一起看**的，所以这三个原语一出现，
// 整仓 lint 就全红了（每个模块都失败），等于没有 lint。
//
// 省事的做法是把这两个文件从 lint 里排掉。但**板级顶层恰恰是最需要 lint 的
// 那个文件**：让板子挂死两次、断电两次的两个 bug 都在它里面 ——
//   · 引用了后面才声明的 wire（Vivado 静默造了条无驱动网络，PS 没时钟）
//   · 把 M_AXI_HPM0_LPD 当 AXI4-Lite 接，rlast/bid/rid 悬空
// 这两类**正是 Verilator 一眼能看出来的**（Icarus 当时就报了 Unable to bind
// wire）。把它排除出 lint，等于把唯一一道能自动挡住这类错误的门关掉。
//
// 所以：给原语写桩，让顶层照样进 lint。
//
// ============================================================================
// 【桩的端口表是有价值的，不是形式】
// ============================================================================
// 端口名写错了，lint 会报"这个端口不存在"—— 也就是说这份桩顺带把
// **顶层的端口名拼写**给校验了。所以端口表要照着 Xilinx 的原语抄全，
// 不要图省事只写用到的几个。
//
// ⚠️ 这个文件**不在** hardware/rtl/ 下，也不进 impl_bitstream.tcl 的文件清单。
//    它一旦被综合到，就会把真原语顶掉，出来的 bitstream 里 PS 是空的。
`default_nettype none

// ---------------------------------------------------------------------------
// BUFGCE_DIV —— UltraScale+ 的分频时钟缓冲
// ---------------------------------------------------------------------------
module BUFGCE_DIV #(
    parameter integer BUFGCE_DIVIDE   = 1,
    parameter         IS_CE_INVERTED  = 1'b0,
    parameter         IS_CLR_INVERTED = 1'b0,
    parameter         IS_I_INVERTED   = 1'b0
) (
    output wire O,
    input  wire CE,
    input  wire CLR,
    input  wire I
);
    // 行为上就是直通：lint 只关心连接关系，分频比在这里没有意义。
    assign O = I;
    wire _unused = &{1'b0, CE, CLR, 1'b0};
endmodule

// ---------------------------------------------------------------------------
// SYSMONE4 —— UltraScale+ 的系统监测单元（这里只用它的 DRP 口读结温）
// ---------------------------------------------------------------------------
module SYSMONE4 #(
    parameter SIM_DEVICE       = "ULTRASCALE_PLUS",
    parameter SIM_MONITOR_FILE = "design.txt",
    parameter [15:0] INIT_40 = 16'h0000,
    parameter [15:0] INIT_41 = 16'h0000,
    parameter [15:0] INIT_42 = 16'h0800
) (
    output wire [15:0] ALM,
    output wire        BUSY,
    output wire [5:0]  CHANNEL,
    output wire        EOC,
    output wire        EOS,
    output wire        JTAGBUSY,
    output wire        JTAGLOCKED,
    output wire        JTAGMODIFIED,
    output wire [4:0]  MUXADDR,
    output wire        OT,
    output wire        I2C_SCLK_TS,
    output wire        I2C_SDA_TS,
    output wire        SMBALERT_TS,
    output wire [15:0] DO,
    output wire        DRDY,
    input  wire        CONVST,
    input  wire        CONVSTCLK,
    input  wire [15:0] VAUXN,
    input  wire [15:0] VAUXP,
    input  wire        VN,
    input  wire        VP,
    input  wire [7:0]  DADDR,
    input  wire        DCLK,
    input  wire [15:0] DI,
    input  wire        DEN,
    input  wire        DWE,
    input  wire        I2C_SCLK,
    input  wire        I2C_SDA,
    input  wire        RESET
);
    // 桩不产生温度：DRDY 恒零，于是 sysmon_drp 会走超时那条路。
    // 这正好让"温度读不到"这条分支在 lint 视角下也是可达的。
    assign {ALM, BUSY, CHANNEL, EOC, EOS, JTAGBUSY, JTAGLOCKED, JTAGMODIFIED,
            MUXADDR, OT, I2C_SCLK_TS, I2C_SDA_TS, SMBALERT_TS, DO, DRDY} = 0;
    wire _unused = &{1'b0, CONVST, CONVSTCLK, VAUXN, VAUXP, VN, VP,
                     DADDR, DCLK, DI, DEN, DWE, I2C_SCLK, I2C_SDA, RESET, 1'b0};
endmodule

// ---------------------------------------------------------------------------
// zynq_ultra_ps_e_0 —— PS 的 IP 封装
//
// ⚠️ 端口方向是从 **PS 的角度**看的：M_AXI_HPM0_LPD 上 PS 是**主**，
//    所以 awaddr/awvalid/... 是 PS 的输出，而 awready/bresp/rdata/rlast 是
//    PS 的输入。第一版把 rlast/bid/rid 当成 PS 的输出漏接了，板子就挂了 ——
//    这份桩把方向写对，Verilator 才能替我盯着这件事。
// ---------------------------------------------------------------------------
module zynq_ultra_ps_e_0 (
    input  wire        maxihpm0_lpd_aclk,
    output wire        pl_clk0,
    output wire        pl_resetn0,

    // 写地址通道（PS 出）
    output wire [39:0] maxigp2_awaddr,
    output wire [2:0]  maxigp2_awprot,
    output wire        maxigp2_awvalid,
    input  wire        maxigp2_awready,
    output wire [15:0] maxigp2_awid,
    output wire [7:0]  maxigp2_awlen,
    output wire [2:0]  maxigp2_awsize,
    output wire [1:0]  maxigp2_awburst,
    output wire        maxigp2_awlock,
    output wire [3:0]  maxigp2_awcache,
    output wire [3:0]  maxigp2_awqos,
    output wire [15:0] maxigp2_awuser,

    // 写数据通道（PS 出）
    output wire [31:0] maxigp2_wdata,
    output wire [3:0]  maxigp2_wstrb,
    output wire        maxigp2_wlast,
    output wire        maxigp2_wvalid,
    input  wire        maxigp2_wready,

    // 写响应（**PS 入** —— 由 PL 驱动）
    input  wire [15:0] maxigp2_bid,
    input  wire [1:0]  maxigp2_bresp,
    input  wire        maxigp2_bvalid,
    output wire        maxigp2_bready,

    // 读地址通道（PS 出）
    output wire [39:0] maxigp2_araddr,
    output wire [2:0]  maxigp2_arprot,
    output wire        maxigp2_arvalid,
    input  wire        maxigp2_arready,
    output wire [15:0] maxigp2_arid,
    output wire [7:0]  maxigp2_arlen,
    output wire [2:0]  maxigp2_arsize,
    output wire [1:0]  maxigp2_arburst,
    output wire        maxigp2_arlock,
    output wire [3:0]  maxigp2_arcache,
    output wire [3:0]  maxigp2_arqos,
    output wire [15:0] maxigp2_aruser,

    // 读数据（**PS 入** —— rid / rlast 由 PL 驱动，漏了就 CPU 卡死）
    input  wire [15:0] maxigp2_rid,
    input  wire [31:0] maxigp2_rdata,
    input  wire [1:0]  maxigp2_rresp,
    input  wire        maxigp2_rlast,
    input  wire        maxigp2_rvalid,
    output wire        maxigp2_rready
);
    assign pl_clk0    = maxihpm0_lpd_aclk;
    assign pl_resetn0 = 1'b1;
    assign {maxigp2_awaddr, maxigp2_awprot, maxigp2_awvalid, maxigp2_awid,
            maxigp2_awlen, maxigp2_awsize, maxigp2_awburst, maxigp2_awlock,
            maxigp2_awcache, maxigp2_awqos, maxigp2_awuser,
            maxigp2_wdata, maxigp2_wstrb, maxigp2_wlast, maxigp2_wvalid,
            maxigp2_bready,
            maxigp2_araddr, maxigp2_arprot, maxigp2_arvalid, maxigp2_arid,
            maxigp2_arlen, maxigp2_arsize, maxigp2_arburst, maxigp2_arlock,
            maxigp2_arcache, maxigp2_arqos, maxigp2_aruser,
            maxigp2_rready} = 0;
    wire _unused = &{1'b0, maxigp2_awready, maxigp2_wready,
                     maxigp2_bid, maxigp2_bresp, maxigp2_bvalid,
                     maxigp2_arready, maxigp2_rid, maxigp2_rdata,
                     maxigp2_rresp, maxigp2_rlast, maxigp2_rvalid, 1'b0};
endmodule

`default_nettype wire
