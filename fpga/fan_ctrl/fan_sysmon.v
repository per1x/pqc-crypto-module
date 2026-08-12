// fan_sysmon —— SYSMONE4 原语 + DRP 轮询，出一个 16 位温度码
//
// ⚠️ **本文件含厂商原语（SYSMONE4），只能在 Vivado 里综合/仿真。**
//    cocotb 与 Yosys 都跳过它，测的是 sysmon_drp.v 那一半（纯 RTL）
//    和 fan_ctrl.v（温度码进、PWM 出）。这样"厂商原语"这件事只污染一个文件。
//
// ============================================================================
// 【INIT_41 / INIT_42 这两个字是拿一次上板换来的，别随手改】
// ============================================================================
// 第一版写的是 INIT_41=0x20F0、INIT_42=0x0400，两个都错，而且**错得没有症状**：
// DRP 有应答、0x00 读出来 39924（换算 32.5°C，数值完全合理），风扇老实停在
// 最低档 —— 但那个数在五分钟六十次采样里一个比特都没变，ADC 其实一次都没转换。
//
//   ① INIT_41[15:12] 是 SEQ。写 0x2 是"连续序列模式"，而序列模式要求另外
//      配置 SEQ_CHSEL（0x48/0x49）指定扫哪些通道 —— 我一个都没配，
//      于是序列里没有任何通道，ADC 无事可做。
//      改回 **默认模式（SEQ=0）**：这个模式下 ADC 自己循环采样温度与
//      VCCINT/VCCAUX/VCCBRAM，一个通道都不用配，正是文件头原本想要的东西。
//
//   ② INIT_42[15:8] 是 DCLK 分频比。写 4 的话 ADCCLK = DCLK/4，
//      在 75 MHz 下是 18.75 MHz，**远超 SYSMONE4 约 5.2 MHz 的上限**。
//      分频比改成参数 DCLK_DIV，由例化处按自己的时钟算：
//        75 MHz  → 16（4.69 MHz）
//        200 MHz → 40（5.00 MHz）
//      两个 bitstream 时钟不同，所以这个必须是参数，不能写死。
//
// 【DCLK 用系统时钟】
// SYSMONE4 的 DRP 时钟上限远高于本设计的时钟，直接接 clk 即可；
// 真正有上限的是上面那个 ADCCLK。
`default_nettype none

module fan_sysmon #(
    parameter integer PERIOD   = 75_000,  // 采样间隔（75 MHz 下 1 ms）
    parameter integer DCLK_DIV = 16       // ADCCLK = clk/DCLK_DIV，必须 ≤ 5.2 MHz
) (
    input  wire        clk,
    input  wire        rst_n,
    output wire [15:0] temp_code,
    output wire        temp_valid,
    output wire        sysmon_timeout,

    // 软件调试读窗口（见 sysmon_drp.v 的文件头）
    input  wire        dbg_req,
    input  wire [7:0]  dbg_addr,
    output wire [15:0] dbg_data,
    output wire        dbg_valid,
    output wire        dbg_timeout
);
    wire        den, dwe;
    wire [7:0]  daddr;   // 由 sysmon_drp 驱动（温度地址 / 调试地址切换）
    wire [15:0] di, dout;
    wire        drdy;

    sysmon_drp #(.PERIOD(PERIOD), .ADDR(8'h00)) u_drp (
        .clk(clk), .rst_n(rst_n),
        .den(den), .daddr(daddr), .di(di), .dwe(dwe),
        .dout(dout), .drdy(drdy),
        .value(temp_code), .value_valid(temp_valid), .timed_out(sysmon_timeout),
        .dbg_req(dbg_req), .dbg_addr(dbg_addr), .dbg_data(dbg_data),
        .dbg_valid(dbg_valid), .dbg_timeout(dbg_timeout));

    // ⚠️ **SIM_DEVICE 必须显式写成 ZYNQ_ULTRASCALE。**
    // 原语的默认值是 ULTRASCALE_PLUS；Vivado 初始化网表时会把它自动改成器件
    // 实际的架构，然后在**最后一步 write_bitstream 的 DRC**（ADEF-911）里
    // 因为"被改过"而报错拒绝出图。也就是说：综合、布局布线、时序全部跑完
    // （三十多分钟）之后才失败。所以这一行不是可选的风格问题。
    // impl_bitstream.tcl 里另有一条综合后断言，把这个错提前到五分钟内。
    SYSMONE4 #(
        .SIM_DEVICE       ("ZYNQ_ULTRASCALE"),
        .SIM_MONITOR_FILE ("design.txt"),
        .INIT_40 (16'h0000),      // 通道/平均：默认
        .INIT_41 (16'h00F0),      // SEQ=0 → **默认模式**，报警全关（见文件头 ①）
        .INIT_42 (DCLK_DIV << 8)  // [15:8] = DCLK 分频比（见文件头 ②）
    ) u_sysmon (
        .DCLK      (clk),
        .RESET     (~rst_n),
        .DEN       (den),
        .DADDR     (daddr),
        .DI        (di),
        .DWE       (dwe),
        .DO        (dout),
        .DRDY      (drdy),
        .CONVST    (1'b0),
        .CONVSTCLK (1'b0),
        .VP        (1'b0),
        .VN        (1'b0),
        .VAUXP     (16'd0),
        .VAUXN     (16'd0),
        .I2C_SCLK  (1'b0),
        .I2C_SDA   (1'b0),
        .SMBALERT_TS (),
        .I2C_SCLK_TS (),
        .I2C_SDA_TS  (),
        .ALM       (),
        .OT        (),
        .MUXADDR   (),
        .CHANNEL   (),
        .EOC       (),
        .EOS       (),
        .BUSY      (),
        .JTAGBUSY  (),
        .JTAGLOCKED(),
        .JTAGMODIFIED()
    );

endmodule

`default_nettype wire
