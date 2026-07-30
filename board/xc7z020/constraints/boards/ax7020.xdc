# ax7020.xdc —— 黑金 AX7020（XC7Z020-CLG400）的板级约束【模板，引脚待填】
#
# 这个文件存在的意义是说明"换板要改什么"：只有引脚号与 I/O 电平。
# 其余部分（timing.xdc、RTL、block design、软件）一律不动。
#
# 【引脚号必须从手上这块板的资料填入】下面留空的三行需要按 AX7020 的原理图或
# 厂商提供的 master XDC 补齐。这里不写猜测值：LED 引脚在不同厂商的 XC7Z020
# 板子之间没有任何通用性，填错的后果是把输出脚接到别的输出脚上。
#
# 填法：在厂商资料里找到三个可用 LED 的 PACKAGE_PIN 与所在 bank 的电平，
# 按下面的格式写进去，然后
#     vivado -mode batch -source board/xc7z020/vivado/create_project.tcl \
#            -tclargs -board ax7020
#
# 正点原子等其它 XC7Z020 板子同理：复制本文件改名，填引脚即可。

# ---- 配置 bank 电平 ----
# 多数 XC7Z020 板子的 PL 侧 bank 34/35 用 3.3 V，但仍以手上这块板的资料为准。
set_property CFGBVS VCCO        [current_design]
set_property CONFIG_VOLTAGE 3.3 [current_design]

# ---- 状态指示灯 ----
# status_led[0] 复位已释放
# status_led[1] 数据面有活动
# status_led[2] 上次 STATUS 带 ERR
#
# set_property -dict {PACKAGE_PIN <引脚> IOSTANDARD <电平>} [get_ports {status_led[0]}]
# set_property -dict {PACKAGE_PIN <引脚> IOSTANDARD <电平>} [get_ports {status_led[1]}]
# set_property -dict {PACKAGE_PIN <引脚> IOSTANDARD <电平>} [get_ports {status_led[2]}]
