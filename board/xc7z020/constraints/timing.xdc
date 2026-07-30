# timing.xdc —— 与具体板子无关的约束
#
# 换板子时**不需要动这个文件**；要换的是 constraints/boards/<板名>.xdc 里的
# 引脚分配与电平。两者分开正是为了这一点。
#
# 【为什么这里没有 create_clock】
# PL 的时钟来自 PS7 的 FCLK_CLK0，它由 block design 自动约束，
# 在这里重复定义只会得到两条互相冲突的约束。频率在 create_project.tcl 里由
# PCW_FPGA0_PERIPHERAL_FREQMHZ 决定，当前取 100 MHz。

# 状态 LED 与任何时钟都没有关系：它只是给人看的指示灯，采样时刻无所谓。
# 不设成伪路径的话，工具会拿默认的输出延迟去约束它，在报告里制造一批
# 无意义的时序违例，把真正需要关注的路径淹掉。
set_false_path -to [get_ports {status_led[*]}]

# 复位释放是异步的：proc_sys_reset 内部已经做了同步，跨到各时钟域的那一段
# 不需要再按单周期路径约束。
set_false_path -from [get_pins -hierarchical -filter {NAME =~ *proc_sys_reset*/EXT_LPF/*}] -quiet
