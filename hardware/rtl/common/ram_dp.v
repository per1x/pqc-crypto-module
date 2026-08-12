// ram_dp —— 真双口同步 RAM（推断成 Xilinx BRAM 的标准写法）
//
// 为什么要有这个模块：原来 ntt_core 的系数存储写成 `reg [15:0] mem [0:255]`
// 加**组合读**（`assign rd_data = mem[rd_addr]`）。这种写法**不可能**变成 BRAM ——
// BRAM 的读口是带输出寄存器的同步读，组合读只能落到 LUT 上。综合报告里的证据：
//
//     ntt_core:  CLB LUTs 28494 (40.38%)   Block RAM Tile 0   LUT as Memory 0
//
// 28000 个 LUT 里绝大部分不是算术，是 4 个读地址（rd_addr / j / j+len / scale_i）
// 各自的 256:1 十六位选择器，加上 256 个寄存器每个都要的写地址译码与保持选择器。
// 一整颗 ZU3EG 才 70560 个 LUT，光一个 NTT 核就吃掉四成，S4 的 ML-KEM 顶层无从谈起。
//
// 换成这里的写法之后同一块存储是 1 个 RAMB18（256×16 = 4096 bit），LUT 归零。
// 代价是**读有一拍延迟**，所以调用方的时序要跟着改，见 ntt_core 的两段式蝶形。
//
// 写法要点（照 UG901 的推断模板）：
//   * 两个口各自一个 always 块，各自 `dout <= mem[addr]`，这样综合器认得出 TDP；
//   * 读优先（read-first）：同一个口同拍读写时 dout 给的是**旧值**；
//   * **两个口写同一个地址的行为未定义** —— 调用方必须保证不会发生。
//     ntt_core 的蝶形一定是 j 与 j+len（len ≥ 2），恒不相等；
//     pqc_accel_axi 的缓冲区同一时刻只有一个口在写。
//   * 不能有复位。BRAM 的存储阵列没有复位口，一旦写了复位就退回 LUT，前功尽弃。
`default_nettype none

module ram_dp #(
    parameter integer DW = 16,
    parameter integer AW = 8
) (
    input  wire            clk,

    input  wire            a_we,
    input  wire [AW-1:0]   a_addr,
    input  wire [DW-1:0]   a_din,
    output reg  [DW-1:0]   a_dout,

    input  wire            b_we,
    input  wire [AW-1:0]   b_addr,
    input  wire [DW-1:0]   b_din,
    output reg  [DW-1:0]   b_dout
);

    (* ram_style = "block" *)
    reg [DW-1:0] mem [0:(1<<AW)-1];

    // 仿真里给个确定初值：Icarus/Verilator 下未初始化数组是 X，
    // 而 ntt_core 的用法是"全写一遍再读"，X 不会传到输出；但 X 会让波形难看，
    // 也会掩盖真正的读越界。综合时 Vivado 把它变成 BRAM 的初始化值，不占资源。
    integer i;
    initial begin
        for (i = 0; i < (1<<AW); i = i + 1) mem[i] = {DW{1'b0}};
    end

    always @(posedge clk) begin
        if (a_we) mem[a_addr] <= a_din;
        a_dout <= mem[a_addr];
    end

    always @(posedge clk) begin
        if (b_we) mem[b_addr] <= b_din;
        b_dout <= mem[b_addr];
    end

`ifndef SYNTHESIS
    // 两口同址写是未定义行为，仿真里直接喊出来，别等到板上才发现
    always @(posedge clk) begin
        if (a_we && b_we && (a_addr == b_addr)) begin
            $display("[%0t] ram_dp 断言失败：两个口同时写同一地址 %0d", $time, a_addr);
            $stop;
        end
    end
`endif

endmodule

`default_nettype wire
