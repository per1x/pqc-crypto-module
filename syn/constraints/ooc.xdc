# OOC 综合的最小约束：只给一个虚拟时钟周期。
# 6.667 ns = 150 MHz —— 路线图 §5.8.1 算例用的就是这个频率，
# 换目标频率时改这里，然后重跑 ooc_synth.tcl。
create_clock -period 6.667 -name virt_clk

# 当前 rtl/mlkem 下的模块都是纯组合逻辑（mont_reduce / butterfly / barrett），
# 综合出来是一条组合路径。等 NTT 核带上时序逻辑后，这里要改成对真实时钟端口约束：
#   create_clock -period 6.667 -name clk [get_ports clk]
# 并补 set_input_delay / set_output_delay。
