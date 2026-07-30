# create_project.tcl —— 生成 XC7Z020 的 Vivado 工程与 block design
#
# 工程不入库，入库的是这个脚本：Vivado 的 .xpr 与 .bd 是带绝对路径、带工具版本
# 的二进制/半二进制产物，改一个设置就会产生大片无法审阅的 diff。用 Tcl 生成，
# 工程随时可以删掉重建，评审的对象是脚本本身。
#
# 用法（需要 Vivado，本仓库的开发机上没有）：
#     vivado -mode batch -source board/xc7z020/vivado/create_project.tcl
#     vivado -mode batch -source board/xc7z020/vivado/create_project.tcl -tclargs \
#            -board ax7020 -outdir /tmp/pqc_vivado
#
# 参数：
#     -board <name>   板级引脚与 PS7 预设，见 board/xc7z020/constraints/boards/
#                     默认 pynq_z2
#     -outdir <path>  工程输出目录，默认 <repo>/build-vivado
#     -part <part>    器件型号，默认 xc7z020clg400-1
#
# 【结构】
#     PS7 ── M_AXI_GP0 ──┬── pqc_accel_zynq  S_AXI      (0x43C0_0000)
#                        └── axi_dma_0       S_AXI_LITE (0x4040_0000)
#     PS7 ── S_AXI_HP0 ──── axi_dma_0 的两个 MM 主口
#     axi_dma_0 M_AXIS_MM2S ──> pqc_accel_zynq S_AXIS   （PS 送数据进 PL）
#     axi_dma_0 S_AXIS_S2MM <── pqc_accel_zynq M_AXIS   （PL 送结果回 PS）
#
# 控制面走 AXI4-Lite、数据面走 AXI-DMA，与 docs/register-map.md 的分工一一对应：
# 软件配寄存器、DMA 搬数据、轮询 STATUS.DONE、再把结果搬回来。

set script_dir [file normalize [file dirname [info script]]]
set repo_root  [file normalize $script_dir/../../..]

# ---- 参数 ----
set opt_board  "pynq_z2"
set opt_outdir "$repo_root/build-vivado"
set opt_part   "xc7z020clg400-1"
# XC7Z020 上默认不含 NTT 核：资源实测与取舍见
# board/xc7z020/docs/resource-budget.md。器件更大时用 -with-ntt 打开。
set opt_with_ntt 0

for {set i 0} {$i < [llength $argv]} {incr i} {
    set a [lindex $argv $i]
    switch -- $a {
        -board  { incr i; set opt_board  [lindex $argv $i] }
        -outdir { incr i; set opt_outdir [lindex $argv $i] }
        -part   { incr i; set opt_part   [lindex $argv $i] }
        -with-ntt { set opt_with_ntt 1 }
        default { puts "忽略未知参数：$a" }
    }
}

set proj_name "pqc_accel_${opt_board}"
set bd_name   "pqc_accel_bd"

puts "工程名   : $proj_name"
puts "器件     : $opt_part"
puts "目标板   : $opt_board"
puts "输出目录 : $opt_outdir"
puts "NTT 核   : [expr {$opt_with_ntt ? {包含} : {不包含（面积所限）}}]"

file mkdir $opt_outdir
create_project $proj_name $opt_outdir/$proj_name -part $opt_part -force

# 板级文件（board part）不是必需的，但装了之后 PS7 的 DDR / MIO 预设可以直接套用。
# 【重要】DDR 时序、MIO 分配、时钟源频率都是**板级参数**，不同板子完全不同，
# 无法在这个脚本里凭空写出来。装了板级文件就用官方预设；没装则只配置与 PL 相关的
# 部分，DDR 保持 Vivado 默认值 —— 那样出来的比特流能在 PL 侧仿真与综合，
# 但**不能拿去启动真实板子**。脚本会明确提示处于哪种情况。
set board_part_id ""
switch -- $opt_board {
    pynq_z2 { set board_part_id "tul.com.tw:pynq-z2:part0:1.0" }
    ax7020  { set board_part_id "alinx.com:ax7020:part0:1.0" }
    default { set board_part_id "" }
}

set have_board 0
if {$board_part_id ne ""} {
    if {[llength [get_board_parts -quiet $board_part_id]] > 0} {
        set_property board_part $board_part_id [current_project]
        set have_board 1
        puts "板级文件已找到：$board_part_id —— PS7 套用官方预设"
    } else {
        puts "板级文件未安装：$board_part_id"
        puts "  PS7 的 DDR/MIO 将保持 Vivado 默认值，出来的比特流不能用于启动真实板子。"
        puts "  安装方法见 board/xc7z020/docs/vivado.md。"
    }
}

# ---- 源文件 ----
# 通用 RTL 直接引用主干目录，板级分支不复制一份 —— 复制出来的副本迟早与主干漂移。
set rtl_files [list \
    $repo_root/hardware/rtl/mlkem/mont_reduce.v \
    $repo_root/hardware/rtl/mlkem/butterfly.v \
    $repo_root/hardware/rtl/mlkem/ntt_core.v \
    $repo_root/hardware/rtl/keccak/keccak_f1600.v \
    $repo_root/hardware/rtl/bus/axi4lite_regs.v \
    $repo_root/hardware/rtl/bus/pqc_accel_axi.v \
    $repo_root/board/xc7z020/rtl/pqc_accel_zynq.v \
]
add_files -norecurse -fileset sources_1 $rtl_files
set_property file_type {Verilog} [get_files $rtl_files]

# ---- 约束 ----
# 与板无关的时序约束、与板相关的引脚分配分开两个文件，换板只需要替换后者。
set xdc_common $repo_root/board/xc7z020/constraints/timing.xdc
set xdc_board  $repo_root/board/xc7z020/constraints/boards/${opt_board}.xdc
if {![file exists $xdc_board]} {
    puts "找不到板级约束 $xdc_board —— 请照 boards/pynq_z2.xdc 的格式补一份"
    exit 1
}
add_files -fileset constrs_1 -norecurse [list $xdc_common $xdc_board]

# ---- Block design ----
create_bd_design $bd_name

# PS7
set ps7 [create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7 ps7_0]
if {$have_board} {
    apply_bd_automation -rule xilinx.com:bd_rule:processing_system7 \
        -config {make_external "FIXED_IO, DDR" apply_board_preset "1" \
                 Master "Disable" Slave "Disable"} $ps7
} else {
    apply_bd_automation -rule xilinx.com:bd_rule:processing_system7 \
        -config {make_external "FIXED_IO, DDR" apply_board_preset "0" \
                 Master "Disable" Slave "Disable"} $ps7
}

# 与 PL 相关的 PS7 配置：一个 GP 主口给控制面，一个 HP 从口给 DMA 搬数据，
# FCLK_CLK0 定在 100 MHz —— 时序余量见 board/xc7z020/docs/resource-budget.md。
set_property -dict [list \
    CONFIG.PCW_USE_M_AXI_GP0 {1} \
    CONFIG.PCW_USE_S_AXI_HP0 {1} \
    CONFIG.PCW_EN_CLK0_PORT  {1} \
    CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {100} \
    CONFIG.PCW_IRQ_F2P_INTR {1} \
] $ps7

# 复位控制器
set rst [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset rst_ps7_100M]

# 加速器：作为 RTL 模块直接加进 BD。端口名按 AXI 命名约定写好，
# Vivado 据此推断出 S_AXI / S_AXIS / M_AXIS 三个接口并关联到 aclk。
set accel [create_bd_cell -type module -reference pqc_accel_zynq pqc_accel_0]
set_property CONFIG.INCLUDE_NTT $opt_with_ntt $accel

# AXI-DMA：simple mode（不开 scatter-gather），数据宽度 32 位，与流接口一致
set dma [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_dma axi_dma_0]
set_property -dict [list \
    CONFIG.c_include_sg {0} \
    CONFIG.c_sg_include_stscntrl_strm {0} \
    CONFIG.c_include_mm2s {1} \
    CONFIG.c_include_s2mm {1} \
    CONFIG.c_m_axi_mm2s_data_width {32} \
    CONFIG.c_m_axis_mm2s_tdata_width {32} \
    CONFIG.c_m_axi_s2mm_data_width {32} \
    CONFIG.c_s_axis_s2mm_tdata_width {32} \
    CONFIG.c_mm2s_burst_size {16} \
    CONFIG.c_s2mm_burst_size {16} \
] $dma

# 时钟与复位
connect_bd_net [get_bd_pins ps7_0/FCLK_CLK0] [get_bd_pins rst_ps7_100M/slowest_sync_clk]
connect_bd_net [get_bd_pins ps7_0/FCLK_RESET0_N] [get_bd_pins rst_ps7_100M/ext_reset_in]
connect_bd_net [get_bd_pins ps7_0/FCLK_CLK0] [get_bd_pins pqc_accel_0/aclk]
connect_bd_net [get_bd_pins rst_ps7_100M/peripheral_aresetn] [get_bd_pins pqc_accel_0/aresetn]

# 控制面：PS7 的 GP0 接到加速器与 DMA 的从口
apply_bd_automation -rule xilinx.com:bd_rule:axi4 \
    -config [list Master "/ps7_0/M_AXI_GP0" Clk "Auto"] \
    [get_bd_intf_pins pqc_accel_0/s_axi]
apply_bd_automation -rule xilinx.com:bd_rule:axi4 \
    -config [list Master "/ps7_0/M_AXI_GP0" Clk "Auto"] \
    [get_bd_intf_pins axi_dma_0/S_AXI_LITE]

# 数据面：DMA 的两个存储映射主口接到 PS7 的 HP0
apply_bd_automation -rule xilinx.com:bd_rule:axi4 \
    -config [list Master "/axi_dma_0/M_AXI_MM2S" Slave "/ps7_0/S_AXI_HP0" \
                  intc_ip "Auto" Clk_master "Auto" Clk_slave "Auto"] \
    [get_bd_intf_pins ps7_0/S_AXI_HP0]
apply_bd_automation -rule xilinx.com:bd_rule:axi4 \
    -config [list Master "/axi_dma_0/M_AXI_S2MM" Slave "/ps7_0/S_AXI_HP0" \
                  intc_ip "Auto" Clk_master "Auto" Clk_slave "Auto"] \
    [get_bd_intf_pins axi_dma_0/M_AXI_S2MM]

# 流接口：DMA 与加速器直连
connect_bd_intf_net [get_bd_intf_pins axi_dma_0/M_AXIS_MM2S] \
                    [get_bd_intf_pins pqc_accel_0/s_axis]
connect_bd_intf_net [get_bd_intf_pins pqc_accel_0/m_axis] \
                    [get_bd_intf_pins axi_dma_0/S_AXIS_S2MM]

# 状态 LED 引出到顶层，由板级 XDC 分配引脚
create_bd_port -dir O -from 2 -to 0 status_led
connect_bd_net [get_bd_pins pqc_accel_0/status_led] [get_bd_ports status_led]

# ---- 地址映射 ----
# 与 board/xc7z020/include/pqc_accel_zynq.h 里的地址表必须一致，
# 两边对不上时软件读到的是别的外设。
assign_bd_address
set_property offset 0x43C00000 [get_bd_addr_segs {ps7_0/Data/SEG_pqc_accel_0_reg0}]
set_property range  64K        [get_bd_addr_segs {ps7_0/Data/SEG_pqc_accel_0_reg0}]
set_property offset 0x40400000 [get_bd_addr_segs {ps7_0/Data/SEG_axi_dma_0_Reg}]
set_property range  64K        [get_bd_addr_segs {ps7_0/Data/SEG_axi_dma_0_Reg}]

validate_bd_design
save_bd_design

# ---- 顶层封装 ----
make_wrapper -files [get_files $opt_outdir/$proj_name/$proj_name.srcs/sources_1/bd/$bd_name/$bd_name.bd] -top
add_files -norecurse $opt_outdir/$proj_name/$proj_name.gen/sources_1/bd/$bd_name/hdl/${bd_name}_wrapper.v
set_property top ${bd_name}_wrapper [current_fileset]
update_compile_order -fileset sources_1

puts ""
puts "工程已生成：$opt_outdir/$proj_name/$proj_name.xpr"
if {!$have_board} {
    puts "提醒：未安装板级文件，PS7 的 DDR/MIO 是 Vivado 默认值，"
    puts "      这个工程可以综合与实现，但不能用于启动真实板子。"
}
puts "下一步：vivado -mode batch -source board/xc7z020/vivado/build_bitstream.tcl \\"
puts "               -tclargs -proj $opt_outdir/$proj_name/$proj_name.xpr"
