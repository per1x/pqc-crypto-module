// trng_health —— 噪声源的连续健康检测（NIST SP 800-90B §4.4）
//
// 两项检测都是标准要求的"连续检测"（continuous health tests）：噪声源一旦出现
// 卡死或严重偏置，必须在很短的样本数内被发现，而不是等到事后统计。
//
// 【重复计数检测 RCT】
// 连续出现 C 个相同的样本即告警。C = 1 + ceil(−log2(α) / H)，其中 H 是每样本
// 最小熵、α 是误报率。取 α = 2⁻⁴⁰、实测 H = 0.871234 时 C = 47（见下）。
// 这一项抓的是"噪声源卡死"。
//
// 【自适应比例检测 APT】
// 取窗口内第一个样本为参考值，统计整个窗口里等于它的样本个数；超过阈值 C 即
// 告警。窗口 W 对二元源取 1024，其它取 512；C 取满足 P(X ≥ C) ≤ α 的最小整数，
// X ~ B(W, 2⁻ᴴ)。实测 H = 0.871234、W = 1024、α = 2⁻⁴⁰ 时 C = 672（见下）。
// 这一项抓的是"噪声源还在动，但分布已经严重偏了"。
//
// 阈值由参数给出而不是写死：不同噪声源的最小熵不同，阈值必须跟着变。
// hardware/model/trng_health_model.py 按上面的定义现算阈值，测试台用它交叉验证
// 参数取值 —— 这样"阈值表抄错一格"这类问题不会被放过。
//
// 【告警是锁存的】
// 置位后保持，直到 clear 显式清除。健康检测的告警必须能被软件在任意时刻读到，
// 与 accel.h 里 STATUS.DONE 用电平而不用脉冲是同一个理由。
//
// 本模块只做检测，不做"出了告警之后怎么办"的策略 —— 那属于上层：
// SP 800-90B 要求的是停止输出并上报，具体动作由使用方决定。
`default_nettype none

// ============================================================================
// 【阈值来自**实测**最小熵，不再是 H=0.5 的假设】（2026-08-13 更新）
// ============================================================================
// 板上导出 1,048,576 个调理前的原始噪声比特，按 SP 800-90B 非 IID 十项估计器
// 算出 **H = 0.871234 比特/样本**（碰撞估计器卡住的那一项）。工具与逐项数值见
// tools/sp800_90b.py 与 docs/fpga-进展.md。
//
// 拿实测 H 重算之后，旧的两个值都站不住 —— 但坏的方向不一样，得分开说：
//
//   · RCT = 41 在 H=0.871 下相当于 α = 2⁻³⁴·⁸。这个源的样本率约 9.4 M/s，
//     于是**平均 55 分钟就会误报一次**。对一个常开的熵源太频繁。
//   · APT = 793 在 H=0.871 下的触发概率是 5.5×10⁻⁵²。
//     **也就是说这道检测从来不会触发，等于不存在。**
//     它当初是按 H=0.5（p=0.707）算的，而真实的 p 只有 0.547，
//     793 这个门槛离真实分布的中心有二十多个标准差。
//
// 现在按 α = 2⁻⁴⁰ 重算（平均约 33 小时一次虚警，对这个样本率是合理预算）：
//     RCT_CUTOFF = 47   APT_CUTOFF = 672（W 仍为 1024）
// α 的选择依据是样本率，不是照抄规范里那个 2⁻²⁰ —— 那个值在这个速率下是
// 每 0.11 秒误报一次，没法用。
//
// ⚠️ 这两个数**绑在这块芯片这一次的实测熵上**。换器件、换工艺角、改 DECIM
//    或环振级数，都要重新导原始比特重算，不能沿用。
module trng_health #(
    parameter integer SAMPLE_W   = 1,      // 每样本位宽
    parameter integer RCT_CUTOFF = 47,     // 重复计数阈值 C
    parameter integer APT_WINDOW = 1024,   // 自适应比例检测窗口 W
    parameter integer APT_CUTOFF = 672     // 自适应比例检测阈值 C
) (
    input  wire                clk,
    input  wire                rst_n,

    input  wire                clear,      // 清除已锁存的告警
    input  wire                sample_valid,
    input  wire [SAMPLE_W-1:0] sample,

    output reg                 rct_alarm,
    output reg                 apt_alarm,
    output wire                alarm,

    // 观测口：便于软件读取当前状态，也便于测试台直接比对
    output reg  [15:0]         rct_run,    // 当前连续相同样本数
    output reg  [15:0]         apt_count,  // 本窗口内等于参考值的样本数
    output reg  [15:0]         apt_index   // 本窗口已处理的样本数
);
    localparam [15:0] RCT_C = RCT_CUTOFF[15:0];
    localparam [15:0] APT_W = APT_WINDOW[15:0];
    localparam [15:0] APT_C = APT_CUTOFF[15:0];

    reg [SAMPLE_W-1:0] prev;               // RCT 的上一个样本
    reg [SAMPLE_W-1:0] ref_val;            // APT 的窗口参考值
    reg                started;            // 是否已经收到过样本

    assign alarm = rct_alarm | apt_alarm;

    wire same_as_prev = started && (sample == prev);
    wire same_as_ref  = (sample == ref_val);
    wire new_window   = !started || (apt_index == APT_W);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rct_alarm <= 1'b0;
            apt_alarm <= 1'b0;
            rct_run   <= 16'd0;
            apt_count <= 16'd0;
            apt_index <= 16'd0;
            prev      <= {SAMPLE_W{1'b0}};
            ref_val   <= {SAMPLE_W{1'b0}};
            started   <= 1'b0;
        end else begin
            if (clear) begin
                rct_alarm <= 1'b0;
                apt_alarm <= 1'b0;
            end

            if (sample_valid) begin
                started <= 1'b1;

                // ---- 重复计数检测 ----
                if (!same_as_prev) begin
                    prev    <= sample;
                    rct_run <= 16'd1;
                end else begin
                    rct_run <= rct_run + 16'd1;
                    if ((rct_run + 16'd1) >= RCT_C) begin
                        rct_alarm <= 1'b1;
                    end
                end

                // ---- 自适应比例检测 ----
                if (new_window) begin
                    // 窗口的第一个样本既是参考值，也计入统计
                    ref_val   <= sample;
                    apt_count <= 16'd1;
                    apt_index <= 16'd1;
                end else begin
                    apt_index <= apt_index + 16'd1;
                    if (same_as_ref) begin
                        apt_count <= apt_count + 16'd1;
                        if ((apt_count + 16'd1) > APT_C) begin
                            apt_alarm <= 1'b1;
                        end
                    end
                end
            end
        end
    end
endmodule

`default_nettype wire
