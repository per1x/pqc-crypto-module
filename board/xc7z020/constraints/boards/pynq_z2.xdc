# pynq_z2.xdc —— PYNQ-Z2（TUL，XC7Z020-1CLG400C）的板级约束
#
# 这个文件里只有**引脚分配与 I/O 电平**，也只有这些内容与板子有关。
# 换到黑金 AX7020、正点原子等同样用 XC7Z020-clg400 的板子时，
# 复制一份改引脚即可，其余文件一律不动 —— 见 boards/ax7020.xdc 的模板。
#
# 【必须核对】下面的引脚号取自 PYNQ-Z2 公开的 master XDC。第一次上板前请与
# 手上这块板的官方 master 约束文件逐条对照：同一型号的不同批次、不同厂商的
# "兼容板"都可能改动 LED 与按键的引脚。引脚接错最好的情况是灯不亮，
# 最坏的情况是把输出接到了另一个输出上。
#
# 【本设计用到的板级资源】只有 3 个 LED。加速器全部通过 PS 的 AXI 访问，
# 不占用任何 PL 侧的外部引脚，因此换板的代价才这么小。

# ---- 配置 bank 电平 ----
set_property CFGBVS VCCO        [current_design]
set_property CONFIG_VOLTAGE 3.3 [current_design]

# ---- 状态指示灯 ----
# status_led[0] 复位已释放     LD0
# status_led[1] 数据面有活动   LD1
# status_led[2] 上次 STATUS 带 ERR  LD2
set_property -dict {PACKAGE_PIN R14 IOSTANDARD LVCMOS33} [get_ports {status_led[0]}]
set_property -dict {PACKAGE_PIN P14 IOSTANDARD LVCMOS33} [get_ports {status_led[1]}]
set_property -dict {PACKAGE_PIN N16 IOSTANDARD LVCMOS33} [get_ports {status_led[2]}]
