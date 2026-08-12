# ooc_synth.tcl —— out-of-context 综合，出资源与时序报告
#
# ⚠️ **需要 AMD Vivado**（免费版即可）。本仓库的开发机上没有装，所以这个脚本
#    **未经实机验证** —— 它是照 Vivado 的标准 non-project 流程写的，
#    第一次跑可能需要按你的 Vivado 版本微调。装了 Vivado 之后：
#
#      vivado -mode batch -source hardware/syn/ooc_synth.tcl -tclargs <part> <top>
#      # 例：vivado -mode batch -source hardware/syn/ooc_synth.tcl -tclargs xck26-sfvc784-2LV-c butterfly_ct
#
# 为什么值得先跑：明确说资源占用、时序收敛与 Fmax 估算**不需要板子**，
# 只需指定 part。在下单开发板**之前**跑一遍，才知道选的器件够不够用 ——
# 那句"没有综合报告就买板子，板子只会吃灰"说的就是这件事。

set part [lindex $argv 0]
set top  [lindex $argv 1]
if {$part eq "" || $top eq ""} {
    puts "用法: vivado -mode batch -source ooc_synth.tcl -tclargs <part> <top_module>"
    exit 1
}

set root [file normalize [file dirname [info script]]/..]
set rpt  $root/syn/rpt
file mkdir $rpt

# ---- 读入源码 ----------------------------------------------------------------
# 只读自研纯 RTL：算法核刻意不依赖任何厂商 IP，
# 所以这里不需要 read_ip，也因此能在 Verilator/Icarus 里跑 cocotb。
read_verilog -sv [glob $root/rtl/common/*.v]
read_verilog -sv [glob $root/rtl/mlkem/*.v]
read_verilog -sv [glob $root/rtl/mldsa/*.v]
read_verilog -sv [glob $root/rtl/keccak/*.v]
read_verilog -sv [glob $root/rtl/bus/*.v]

# ---- 约束 --------------------------------------------------------------------
# 时序模块与纯组合模块需要不同的约束：
#   ntt_core 有真实 clk 端口 → 必须 create_clock 到该端口，否则时序报告失真；
#   mont_reduce / butterfly_* 是纯组合 → 只能用虚拟时钟报 in2out 延迟。
if {$top eq "ntt_core" || $top eq "mldsa_ntt_core" || $top eq "keccak_f1600"
    || $top eq "sha3_core" || $top eq "pqc_accel_axi"
    || $top eq "mlkem_keygen" || $top eq "mlkem_cbd_stream"} {
    read_xdc $root/syn/constraints/ooc_seq.xdc
    puts "约束：ooc_seq.xdc（clk 端口，100 MHz）"
} else {
    read_xdc $root/syn/constraints/ooc_comb.xdc
    puts "约束：ooc_comb.xdc（虚拟时钟，纯组合）"
}

# ---- 综合 --------------------------------------------------------------------
synth_design -top $top -part $part -mode out_of_context -flatten_hierarchy rebuilt

report_utilization      -file $rpt/${top}_utilization_synth.rpt
report_timing_summary   -file $rpt/${top}_timing_synth.rpt
write_checkpoint -force $rpt/${top}_post_synth.dcp

# ---- 布局布线 ----------------------------------------------------------------
# Fmax 要用 post-route 的 WNS 反推才作数；post-synth 的数字通常偏乐观。
opt_design
place_design
route_design

report_utilization    -file $rpt/${top}_utilization_route.rpt
report_timing_summary -file $rpt/${top}_timing_route.rpt
report_power          -file $rpt/${top}_power.rpt

# ---- 把 Fmax 直接算出来 ------------------------------------------------------
# Fmax = 1 / (T_target - WNS)。这一步是整个脚本的重点：
# 报告里只有 WNS，人工换算容易出错，直接打印出来。
set period [get_property PERIOD [get_clocks -quiet]]
if {$period eq ""} { set period 10.000 }
set wns [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -setup]]
if {$wns ne ""} {
    set fmax [expr {1000.0 / ($period - $wns)}]
    puts "=========================================================="
    puts " $top @ $part"
    puts "   目标周期 : $period ns"
    puts "   WNS      : $wns ns"
    puts "   估算 Fmax: [format %.1f $fmax] MHz"
    puts "=========================================================="
    set fh [open $rpt/${top}_fmax.txt w]
    puts $fh "part=$part top=$top period=$period wns=$wns fmax_mhz=[format %.1f $fmax]"
    close $fh
}

puts "报告已写入 $rpt"
exit 0
