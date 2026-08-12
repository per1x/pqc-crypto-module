# TRNG 的时序约束
#
# ⚠️ **XDC 不是完整的 Tcl**。Vivado 只接受一个受限的命令子集，`if`、`for`、
#    `remove_from_collection` 这类都会报 [Designutils 20-1307] 并被整条忽略 ——
#    注意是**静默忽略那一条约束**，脚本还会继续跑完。所以带条件判断的部分
#    （环振放行组合环、DRC 降级、以及"环有没有被优化掉"的检查）全部放在
#    hardware/syn/trng_ooc.tcl 里，那边是真正的 Tcl 上下文。
#    本文件只留纯声明式约束。

# ---- 时钟 -------------------------------------------------------------------
create_clock -period 10.000 -name clk [get_ports clk]

# OOC 综合没有板级时序，给一个保守的 I/O 预算，这样 in2reg / reg2out 路径
# 才会被计时，而不是只剩 reg2reg。
# 注意用 get_ports -filter 而不是 remove_from_collection —— 后者在 XDC 里不支持。
set_input_delay  -clock clk 1.000 [get_ports -filter {DIRECTION == IN && NAME != clk}]
set_output_delay -clock clk 1.000 [get_ports -filter {DIRECTION == OUT}]

# 复位异步断言、同步释放，不参与时序分析
set_false_path -from [get_ports rst_n]

# tamper 是板级异步信号，进来先被 trng_top 的同步逻辑吃掉
set_false_path -from [get_ports tamper]
