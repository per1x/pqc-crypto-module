# board_pins.xdc —— AXU3EGB 板级管脚约束
#
# 本设计与外界的往来几乎全部经过 PS（AXI、时钟、复位），所以这里**只有一根线**。
#
# ============================================================================
# 【AA11 = 风扇 PWM，出处】
# ============================================================================
# 抄自厂家工程的约束文件，不是猜的：
#   /home/build/factory_vivado/board_test.srcs/constrs_1/new/system.xdc
#     set_property PACKAGE_PIN AA11 [get_ports {fan}]
#     set_property IOSTANDARD LVCMOS33 [get_ports {fan}]
# AXU3EGB 用户手册 §十九：该脚接风扇 MOSFET 栅极，**输出低 = 导通 = 风扇转**。
#
# 管脚号写错的代价不是"风扇不转"，而是**把一根不知道接了什么的线驱起来**，
# 所以这一条必须有出处。上面两行就是出处。
set_property PACKAGE_PIN AA11    [get_ports {fan}]
set_property IOSTANDARD  LVCMOS33 [get_ports {fan}]

# 25 kHz 的 PWM 对时序没有任何要求（一个周期 3000 拍），
# 不设 output delay 是有意的：给它编一个约束反而是在假装知道负载的时序。
# 它是寄存器直出，与外面没有同步关系。
set_false_path -to [get_ports {fan}]
