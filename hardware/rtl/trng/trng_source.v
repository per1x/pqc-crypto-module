// trng_source —— 多环振荡器噪声源的数字化部分
//
// 【结构：MURO（multi-ring oscillator）】
// NUM_RO 条长度互不相同的环振，各自被系统时钟采样，采样结果异或成一比特。
// 这是 Sunar/Fischer 那一系公开设计里最经久的一种，理由有两条：
//   · 单条环振的抖动在一个采样周期里往往不够随机化一整个相位，靠多条频率
//     不同、相位自由漂移的环异或来把偏置压下去；
//   · 环之间要避免互锁（injection locking）。级数取互不相同的奇数，
//     13/15/17/…，频率岔开，锁到一起的概率显著下降。
//
// 【采样与亚稳态】
// 采样点上环振是异步信号，D 触发器必然偶尔进入亚稳态。这在 TRNG 里不是缺陷，
// 亚稳态恰恰是熵的来源之一 —— 但亚稳态电平不能直接进下游组合逻辑，否则会
// 在扇出上分裂成不一致的值。所以每条环后面接两级同步器（ASYNC_REG），
// 第一级吃亚稳态，第二级给出干净电平，异或取第二级。
//
// 【抽取（decimation）：为什么必须有】
// 环振周期约 2 ns 量级，系统时钟 10 ns。相邻两个采样点之间环只走了几个周期，
// 累积抖动远小于一个周期，相邻样本因此是相关的 —— 这时候声称"每样本
// 0.5 bit 最小熵"是站不住的。每 DECIM 个时钟只取一个样本，把采样间隔拉长，
// 让抖动有时间按随机游走累积。
//
// 关键的定义问题：**被抽取之后的那一比特才是 SP 800-90B 意义上的"噪声源
// 样本"**。健康检测和调理器吃的必须是同一条流 —— 如果健康检测跑在抽取前的
// 原始流上、调理器吃抽取后的流，那检测的就不是真正被使用的那个源。
// 所以本模块只输出抽取后的样本，上层无从拿到抽取前的流。
//
// 【真实最小熵仍然是未知数】
// DECIM 的默认值 8 和 trng_health 里 H = 0.5 的假设都还没有硅上实测支撑。
// 上板之后要做的事：把原始比特（DECIM 可配，从 AXI 直接读原始流）导出，
// 跑 NIST EntropyAssessment，按实测的 H 反过来定 DECIM 与健康检测阈值。
// 在那之前这些数字是设计假设，不是结论。
`default_nettype none

module trng_source #(
    parameter integer NUM_RO      = 8,    // 环振条数
    parameter integer RO_STAGES_0 = 13,   // 第 0 条的级数，之后每条 +2
    parameter integer DECIM       = 8     // 抽取比，每 DECIM 个时钟出一个样本
) (
    input  wire clk,
    input  wire rst_n,
    input  wire enable,                   // 低电平时环振停振

    output reg  sample_valid,
    output reg  sample
);

    // ---- 环振阵列 ----
    wire [NUM_RO-1:0] osc;

    genvar g;
    generate
        for (g = 0; g < NUM_RO; g = g + 1) begin : g_ro
            // 级数：13, 15, 17, … 全为奇数且互不相同
            localparam integer ST = RO_STAGES_0 + 2 * g;
            // 种子只在仿真模型里有意义，用来让每条环的抖动序列各不相同
            localparam integer SD = 32'h1000_0001 + g * 32'h0DEF_2B1F;

            ring_osc #(.STAGES(ST), .SEED(SD)) u_ro (
                .enable(enable),
                .osc(osc[g]));
        end
    endgenerate

    // ---- 两级同步器 ----
    // ASYNC_REG 告诉 Vivado 这两级要放在同一个 slice 里、不要被时序优化拆开，
    // 这样第一级的亚稳态有最长的解析时间。
    (* ASYNC_REG = "TRUE" *) reg [NUM_RO-1:0] sync1;
    (* ASYNC_REG = "TRUE" *) reg [NUM_RO-1:0] sync2;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            sync1 <= {NUM_RO{1'b0}};
            sync2 <= {NUM_RO{1'b0}};
        end else begin
            sync1 <= osc;
            sync2 <= sync1;
        end
    end

    wire raw_bit = ^sync2;

    // ---- 抽取 ----
    localparam integer DCW = (DECIM <= 1) ? 1 : $clog2(DECIM);

    reg [DCW-1:0] dcnt;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            dcnt         <= {DCW{1'b0}};
            sample_valid <= 1'b0;
            sample       <= 1'b0;
        end else if (!enable) begin
            // 停振期间不产样本：环没转，采到的是常量，喂给健康检测会立刻
            // 触发 RCT —— 那是个假告警，会掩盖真正的噪声源故障。
            dcnt         <= {DCW{1'b0}};
            sample_valid <= 1'b0;
        end else begin
            if (DECIM <= 1) begin
                sample_valid <= 1'b1;
                sample       <= raw_bit;
            end else if (dcnt == DECIM[DCW-1:0] - 1'b1) begin
                dcnt         <= {DCW{1'b0}};
                sample_valid <= 1'b1;
                sample       <= raw_bit;
            end else begin
                dcnt         <= dcnt + 1'b1;
                sample_valid <= 1'b0;
            end
        end
    end

endmodule

`default_nettype wire
