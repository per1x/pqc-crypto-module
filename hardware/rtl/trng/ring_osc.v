// ring_osc —— 环形振荡器（TRNG 的物理噪声源）
//
// 【这是整条 TRNG 链上唯一"不是数字逻辑"的一环】
// 反相器环的振荡周期由器件内部的传播延迟决定，而传播延迟受热噪声、电源噪声、
// 器件失配影响，逐周期抖动。用一个与之异步的时钟去采样这个环，采到的比特就
// 带有物理不可预测性。TRNG 的熵**全部来自这里**，后面的健康检测与调理都只是
// 在保护和整形这份熵，不会凭空产生熵。
//
// ============================================================================
// ⚠️ 关于仿真模型，说在最前面
// ============================================================================
// 反相器环在 RTL 仿真里是一个零延迟组合环：Icarus 会陷入死循环不推进时间，
// 而 Verilator 直接报 UNOPTFLAT/组合环错误。所以本模块有两套实现：
// （⚠️ 上面这行开头的"而"字不是语气词：Verilator 把**以 verilator 开头的
//   注释行**当成元注释指令，`// Verilator 直接报…` 会让整仓 lint 全部报
//   BADVLTPRAGMA 而挂掉。注释里提到它时前面要有别的字。）
//
//   `TRNG_SIM_MODEL 未定义（默认）→ 真正的反相器环，这一套才综合上板
//   `TRNG_SIM_MODEL 已定义        → 带抖动的行为模型，只用于仿真
//
// **仿真模型里的抖动量是编的，不是测出来的。** 它比真实器件的相位抖动大一到
// 两个数量级 —— 这样做是为了让下游的采样、健康检测、调理、FIFO、AXI 这些
// 数字逻辑能在合理的仿真时长里被跑到。
//
// 因此：**任何最小熵数字都不能从仿真里得出。** 真实的每样本最小熵必须在硅上
// 用 SP 800-90B 的 EntropyAssessment 工具跑原始比特实测，实测之前
// trng_health 的阈值参数（按 H = 0.5 算的）只是一个待验证的假设。
// 这一条写进 docs/fpga-进展.md，不要在报告里把仿真结果说成熵评估结果。
//
// ============================================================================
// 【综合路径的三个必要条件】
// 1. DONT_TOUCH：否则 Vivado 会把 ~~x 成对约掉，整条环塌成一根线。
// 2. XDC 里 set_property ALLOW_COMBINATORIAL_LOOPS TRUE，否则布线器拒绝组合环。
// 3. XDC 里把 LUTLP-1 这条 DRC 降级为 Warning，否则 write_bitstream 失败。
//    三条都在 hardware/syn/constraints/trng_ro.xdc 里。
//
// 【为什么 STAGES 必须是奇数】
// 偶数级反相器环是一个双稳态锁存器，不振荡。参数在下面有断言。
`default_nettype none

// 仿真模型里的延迟是 ps 量级（环振周期约 2 ns），1 ns 的时间单位分辨不出来。
// 综合路径不看 timescale，所以这条指令只在仿真时出现。
// 配套地，hardware/tb/cocotb/Makefile.trng 把 cocotb 的时间单位也设成 1ps。
`ifdef TRNG_SIM_MODEL
`timescale 1ps/1ps
`endif

module ring_osc #(
    parameter integer STAGES   = 13,        // 反相器级数，必须为奇数
    parameter integer SEED     = 32'h1,     // 仅仿真模型用：每个实例的抖动种子
    parameter integer STAGE_PS = 90,        // 仅仿真模型用：单级标称延迟 (ps)
    parameter integer JITTER_PS = 60        // 仅仿真模型用：每半周期抖动幅度 (ps)
) (
    input  wire enable,
    output wire osc
);

    // 偶数级不振荡（那是个双稳态锁存器）。与其上板才发现输出恒定，不如在
    // 仿真一开始就停下。`SYNTHESIS 由 Vivado 自动定义，综合时不看这段。
`ifndef SYNTHESIS
    initial begin
        if (STAGES % 2 == 0) begin
            $display("ring_osc: STAGES=%0d 是偶数，环不会振荡", STAGES);
            $finish;
        end
    end
`endif

`ifdef TRNG_SIM_MODEL
    // ------------------------------------------------------------------
    // 仿真行为模型 —— 不综合
    // ------------------------------------------------------------------
    // 每个半周期重新抽一次延迟，于是相位误差按随机游走累积，这一点与真实
    // 振荡器的累积抖动是同一个机理；只是幅度被人为放大了（见文件头警告）。
    reg        osc_r;
    reg [31:0] lfsr;
    integer    half_ps;

    initial begin
        osc_r = 1'b0;
        lfsr  = (SEED[31:0] == 32'd0) ? 32'hACE1_2345 : SEED[31:0];
    end

    always begin
        if (!enable) begin
            // 环被 enable 关停。仿真时间仍要推进，否则这个 always 变成死循环。
            osc_r   = 1'b0;
            #(STAGES * STAGE_PS);
        end else begin
            lfsr    = {lfsr[30:0], lfsr[31] ^ lfsr[21] ^ lfsr[1] ^ lfsr[0]};
            half_ps = STAGES * STAGE_PS
                    + (lfsr % (2 * JITTER_PS + 1)) - JITTER_PS;
            #(half_ps) osc_r = ~osc_r;
        end
    end

    assign osc = osc_r;

`else
    // ------------------------------------------------------------------
    // 综合实现 —— 真正的反相器环
    // ------------------------------------------------------------------
    // 第 0 级是与非门，兼作 enable 门控：enable=0 时 chain[0] 恒 1，环停振。
    // 停振比让它一直转好：不用时不产生电磁辐射，也省功耗。
    (* DONT_TOUCH = "TRUE" *) wire [STAGES-1:0] chain;

    assign chain[0] = ~(chain[STAGES-1] & enable);

    genvar i;
    generate
        for (i = 1; i < STAGES; i = i + 1) begin : g_inv
            assign chain[i] = ~chain[i-1];
        end
    endgenerate

    assign osc = chain[STAGES-1];
`endif

endmodule

`default_nettype wire
