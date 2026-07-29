# 纯组合子模块（mont_reduce / butterfly_ct / butterfly_gs / barrett_reduce）的 OOC 约束
#
# 这些模块没有 clk 端口，只能用虚拟时钟给一个参考周期，
# 让工具报出组合路径的延迟（in2out）。
create_clock -period 10.000 -name virt_clk

set_input_delay  -clock virt_clk 0.000 [all_inputs]
set_output_delay -clock virt_clk 0.000 [all_outputs]
