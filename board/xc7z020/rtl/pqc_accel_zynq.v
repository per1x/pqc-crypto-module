// pqc_accel_zynq —— 板级包装：把厂商中立的加速器接进 Zynq-7000 的 block design
//
// 内部只例化 hardware/rtl/bus/pqc_accel_axi，不改动它的任何行为。存在的理由
// 是两件板级的事，都不该污染通用 RTL：
//
// 一、**端口命名**。Vivado 的 block design 靠端口名前缀推断总线接口，靠 aclk /
//     aresetn 这两个名字把时钟复位关联上去。厂商中立的顶层用 clk / rst_n ——
//     那是通用 RTL 的写法，不该为了迁就某一家工具改掉。这里做一层纯连线转接：
//
//       aclk / aresetn      → 关联到下面三个接口
//       s_axi_*             → AXI4-Lite 从机（控制/状态寄存器）
//       s_axis_* / m_axis_* → AXI4-Stream 从机/主机（数据）
//
//     aresetn 低有效，与 AXI 规范一致，也与内部核的 rst_n 一致，直接透传。
//
// 二、**板级状态指示**。上板之后，"寄存器读不回来"与"核根本没在跑"是两类完全
//     不同的故障；有几个 LED 就能在接上串口之前先把范围分开。状态不是从内部
//     层次化引用出来的（那不可综合），而是**旁路读通道**得到的：软件本来就会
//     轮询 STATUS，这里顺带把读回的值截下来点灯，与软件看到的是同一份数据。
`default_nettype none

module pqc_accel_zynq (
    input  wire        aclk,
    input  wire        aresetn,

    // AXI4-Lite 从机：控制与状态寄存器
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

    // AXI4-Stream 从机：输入数据
    input  wire [31:0] s_axis_tdata,
    input  wire        s_axis_tvalid,
    output wire        s_axis_tready,
    input  wire        s_axis_tlast,

    // AXI4-Stream 主机：输出数据
    output wire [31:0] m_axis_tdata,
    output wire        m_axis_tvalid,
    input  wire        m_axis_tready,
    output wire        m_axis_tlast,

    // 板级状态指示（接 LED）
    //   [0] 复位已释放
    //   [1] 数据面有活动（收发任一拍握手后点亮一段时间）
    //   [2] 最近一次读到的 STATUS 带 ERR
    output wire [2:0]  status_led
);
    localparam [7:0] REG_STATUS = 8'h04;

    wire s_axi_rvalid_i;
    wire [31:0] s_axi_rdata_i;

    assign s_axi_rvalid = s_axi_rvalid_i;
    assign s_axi_rdata  = s_axi_rdata_i;

    pqc_accel_axi u_accel (
        .clk(aclk),
        .rst_n(aresetn),

        .s_axi_awaddr(s_axi_awaddr),
        .s_axi_awvalid(s_axi_awvalid),
        .s_axi_awready(s_axi_awready),
        .s_axi_wdata(s_axi_wdata),
        .s_axi_wstrb(s_axi_wstrb),
        .s_axi_wvalid(s_axi_wvalid),
        .s_axi_wready(s_axi_wready),
        .s_axi_bresp(s_axi_bresp),
        .s_axi_bvalid(s_axi_bvalid),
        .s_axi_bready(s_axi_bready),
        .s_axi_araddr(s_axi_araddr),
        .s_axi_arvalid(s_axi_arvalid),
        .s_axi_arready(s_axi_arready),
        .s_axi_rdata(s_axi_rdata_i),
        .s_axi_rresp(s_axi_rresp),
        .s_axi_rvalid(s_axi_rvalid_i),
        .s_axi_rready(s_axi_rready),

        .s_axis_tdata(s_axis_tdata),
        .s_axis_tvalid(s_axis_tvalid),
        .s_axis_tready(s_axis_tready),
        .s_axis_tlast(s_axis_tlast),

        .m_axis_tdata(m_axis_tdata),
        .m_axis_tvalid(m_axis_tvalid),
        .m_axis_tready(m_axis_tready),
        .m_axis_tlast(m_axis_tlast));

    // ---- 状态旁路 ----
    // 记住最近一次读地址，读数据回来时若是 STATUS 就把 ERR 位截下来。
    reg [7:0]  last_araddr;
    reg        err_seen;
    reg [23:0] act_timer;      // 活动指示的保持计数，约 0.17 s @100 MHz

    wire axis_active = (s_axis_tvalid && s_axis_tready)
                    || (m_axis_tvalid && m_axis_tready);

    always @(posedge aclk or negedge aresetn) begin
        if (!aresetn) begin
            last_araddr <= 8'd0;
            err_seen    <= 1'b0;
            act_timer   <= 24'd0;
        end else begin
            if (s_axi_arvalid && s_axi_arready) begin
                last_araddr <= s_axi_araddr;
            end
            if (s_axi_rvalid_i && s_axi_rready && last_araddr == REG_STATUS) begin
                err_seen <= s_axi_rdata_i[2];
            end
            if (axis_active) begin
                act_timer <= {24{1'b1}};
            end else if (act_timer != 24'd0) begin
                act_timer <= act_timer - 24'd1;
            end
        end
    end

    assign status_led = {err_seen, (act_timer != 24'd0), aresetn};

    // AWPROT / ARPROT 是 AXI4-Lite 的必备信号，主机会驱动它们，但本设计不做
    // 特权级区分：访问控制在软件侧的角色与 ACL 上做，PL 侧不重复一套。
    // 显式吸收掉，免得读者以为是漏接。
    wire unused_prot = &{1'b0, s_axi_awprot, s_axi_arprot};
endmodule

`default_nettype wire
