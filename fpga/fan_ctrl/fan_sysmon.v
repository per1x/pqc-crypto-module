// fan_sysmon —— SYSMONE4 原语 + DRP 轮询，出一个 16 位温度码
//
// ⚠️ **本文件含厂商原语（SYSMONE4），只能在 Vivado 里综合/仿真。**
//    cocotb 与 Yosys 都跳过它，测的是 sysmon_drp.v 那一半（纯 RTL）
//    和 fan_ctrl.v（温度码进、PWM 出）。这样"厂商原语"这件事只污染一个文件。
//
// 【为什么用默认模式，不去写 SYSMON 的配置寄存器】
// SYSMONE4 上电后就在默认模式：自己循环采样片上温度与各路电源电压，
// 结果放在 DRP 的状态寄存器里（0x00 = 温度）。我要的就只是温度，
// 所以一个配置字都不用写 —— 少一整套"写配置、确认配置生效"的代码，
// 也少一类"配置写错导致读到的不是温度"的错误。
//
// 【DCLK 用系统时钟】
// SYSMONE4 的 DRP 时钟上限远高于本设计的 75 MHz，直接接 clk 即可。
// 片内 ADC 自己的采样时钟由原语内部分频，与 DCLK 无关。
`default_nettype none

module fan_sysmon #(
    parameter integer PERIOD = 75_000        // 采样间隔（75 MHz 下 1 ms）
) (
    input  wire        clk,
    input  wire        rst_n,
    output wire [15:0] temp_code,
    output wire        temp_valid,
    output wire        sysmon_timeout
);
    wire        den, dwe;
    wire [7:0]  daddr;
    wire [15:0] di, dout;
    wire        drdy;

    sysmon_drp #(.PERIOD(PERIOD), .ADDR(8'h00)) u_drp (
        .clk(clk), .rst_n(rst_n),
        .den(den), .daddr(daddr), .di(di), .dwe(dwe),
        .dout(dout), .drdy(drdy),
        .value(temp_code), .value_valid(temp_valid), .timed_out(sysmon_timeout));

    SYSMONE4 #(
        .SIM_MONITOR_FILE ("design.txt"),
        .INIT_40 (16'h0000),      // 默认模式：内部循环采样
        .INIT_41 (16'h20F0),      // 连续采样、使能校准
        .INIT_42 (16'h0400)       // DCLK 分频
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
