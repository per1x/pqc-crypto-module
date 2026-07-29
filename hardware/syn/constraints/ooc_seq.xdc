# 时序模块（ntt_core）的 OOC 约束
#
# ⚠️ 这个文件的存在理由：原来只有一个**未绑定端口**的虚拟时钟，
# 用它综合 ntt_core 会让 clk 端口上没有时钟约束 → 所有时序路径不被计时
# → report_timing_summary 给出失真/空的结果，反而让人误判"时序很好"。
# 那与 hardware/syn/ 存在的全部意义（买板前拿到可信时序）直接冲突。
#
# 100 MHz = 10 ns。选它而不是 150 MHz 的理由见 hardware/syn/README.md：
# 单周期蝶形里有**两级串行乘法**（zeta·b 与 mont 内的 m·Q）加模约减，
# 6.667 ns 偏紧；先在 100 MHz 拿到可信的正 WNS，再决定要不要把蝶形打一拍。

create_clock -period 10.000 -name clk [get_ports clk]

# OOC 综合没有真实的板级时序，给一个保守的 I/O 预算：
# 输入按 1 ns 到达后延迟、输出按 1 ns 建立要求。
# 这样 report_timing_summary 里 in2reg / reg2out 路径才会被计时，
# 而不是只剩 reg2reg。
set_input_delay  -clock clk 1.000 [remove_from_collection [all_inputs] [get_ports clk]]
set_output_delay -clock clk 1.000 [all_outputs]

# 复位是异步断言、同步释放的低有效信号：不参与时序分析，
# 但要告诉工具别把它当数据路径去优化时序。
set_false_path -from [get_ports rst_n]
