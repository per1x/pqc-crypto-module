# trng_ooc.tcl —— TRNG 的 out-of-context 综合 + 布局布线
#
# 与 ooc_synth.tcl 分开，两条理由：
#   · 源码集不同（TRNG 要 trng/ + common/ + keccak/，不要 mlkem/mldsa/）；
#   · 约束不同（环振要放行组合环、降级 LUTLP-1 DRC，见 constraints/trng_ro.xdc）。
#
# 目标器件默认 xazu3eg-sfvc784-1-i —— 这是 AXU3EGB 上真正那颗，从出厂
# Vivado 工程里挖出来的（车规 XA 版，不是 XC）。资源与 XCZU3EG 相同：
# 71K LUT / 141K FF / 360 DSP / 216 个 BRAM36。
#
# ⚠️ **绝不能定义 TRNG_SIM_MODEL**。定义了，ring_osc 就会被换成仿真用的
#    伪随机行为模型 —— 那东西综合不出来（带 # 延迟），就算能出来也不是
#    噪声源。这个脚本不传任何 -verilog_define，就是为了确保这一点。
#
# 用法：
#   vivado -mode batch -source hardware/syn/trng_ooc.tcl
#   vivado -mode batch -source hardware/syn/trng_ooc.tcl -tclargs <part> <top>

set part [lindex $argv 0]
set top  [lindex $argv 1]
if {$part eq ""} { set part "xazu3eg-sfvc784-1-i" }
if {$top  eq ""} { set top  "trng_axi" }

set root [file normalize [file dirname [info script]]/..]
set rpt  $root/syn/rpt
file mkdir $rpt

puts "=========================================================="
puts " TRNG OOC 综合：top=$top part=$part"
puts "=========================================================="

read_verilog [glob $root/rtl/trng/*.v]
read_verilog [glob $root/rtl/common/*.v]
read_verilog [glob $root/rtl/keccak/*.v]

read_xdc $root/syn/constraints/trng_ro.xdc

synth_design -top $top -part $part -mode out_of_context -flatten_hierarchy none

report_utilization    -file $rpt/${top}_utilization_synth.rpt
report_timing_summary -file $rpt/${top}_timing_synth.rpt
write_checkpoint -force $rpt/${top}_post_synth.dcp

# ---- 环振：放行组合环，降级 DRC ---------------------------------------------
# 这几条带条件判断，XDC 里写不了（XDC 不支持 if），所以放在这里。
# ring_osc 里的 chain 带了 DONT_TOUCH，网络名会被保留，可以按名字抓。
set ro_nets [get_nets -hier -quiet -filter {NAME =~ *u_ro*chain*}]
if {[llength $ro_nets] > 0} {
    set_property ALLOW_COMBINATORIAL_LOOPS TRUE $ro_nets
    puts "TRNG：已对 [llength $ro_nets] 条环振网络放行组合环"
}
# LUTLP-1 = "Combinatorial Loop Alert"。不降级则 write_bitstream 失败。
set_property SEVERITY {Warning} [get_drc_checks LUTLP-1]

# 环振输出没有时钟域，到采样触发器是不可时序分析的异步路径。
# 亚稳态由 sync1→sync2 两级同步器吸收（RTL 里带 ASYNC_REG）。
set sync1 [get_cells -hier -quiet -filter {NAME =~ *sync1_reg*}]
if {[llength $sync1] > 0} {
    set_false_path -to [get_pins -quiet -of_objects $sync1 -filter {REF_PIN_NAME =~ D*}]
    puts "TRNG：已对 [llength $sync1] 个采样触发器的 D 口设 false path"
}

# ---- 环振有没有活下来 --------------------------------------------------------
# 本脚本最重要的一步。环被优化掉之后 TRNG 会静默退化成常数发生器，
# 综合日志里**不会有任何报错** —— 所以必须在这里主动数，数不对就当场失败。
#
# 期望值：NUM_RO 条环，第 g 条 RO_STAGES_0+2g 级，默认 8 条 13/15/…/27 级，
# 合计 160 个查找表单元（每条环 1 个与非门 + STAGES-1 个反相器）。
# 按 REF_NAME 抓 LUT* 而不是 PRIMITIVE_GROUP —— 后者在 post-synth 网表上
# 并不总是填好，第一版就是栽在这里（数出 0 个，误判成环被优化掉了）。
set ro_cells [get_cells -hier -quiet -filter {NAME =~ *u_ro*}]
set ro_luts  [llength [get_cells -hier -quiet -filter {NAME =~ *u_ro* && REF_NAME =~ LUT*}]]
puts "----------------------------------------------------------"
puts " 环振检查：ring_osc 层次下 [llength $ro_cells] 个单元，其中 LUT $ro_luts 个"
if {$ro_luts < 120} {
    puts " ❌ LUT 数远低于预期（8 条环共 160 级）。"
    puts "    极可能是 DONT_TOUCH 没生效、反相器链被约掉了。"
    puts "    这种情况下 TRNG 会静默变成常数发生器 —— 不要继续。"
    error "ring_osc 被优化掉了"
}
puts " ✅ 环振保住了：$ro_luts 个 LUT"
puts "----------------------------------------------------------"

opt_design
place_design
route_design

report_utilization    -file $rpt/${top}_utilization_route.rpt
report_timing_summary -file $rpt/${top}_timing_route.rpt
report_power          -file $rpt/${top}_power.rpt
write_checkpoint -force $rpt/${top}_post_route.dcp

# 布线之后再数一次：opt_design / place_design 也可能把环吃掉。
# 综合时活着不等于布线之后还活着，这一步不能省。
set ro_luts_r [llength [get_cells -hier -quiet -filter {NAME =~ *u_ro* && REF_NAME =~ LUT*}]]
puts "----------------------------------------------------------"
puts " 布线后环振复查：$ro_luts_r 个 LUT（综合后是 $ro_luts 个）"
if {$ro_luts_r < 120} {
    error "环振在 opt/place/route 阶段被优化掉了"
}
puts " ✅ 布线后环振仍在"
puts "----------------------------------------------------------"

# ---- Fmax ------------------------------------------------------------------
set period [get_property PERIOD [get_clocks -quiet clk]]
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
    puts $fh "part=$part top=$top period=$period wns=$wns fmax_mhz=[format %.1f $fmax] ro_luts=$ro_luts"
    close $fh
}

puts "报告已写入 $rpt"
exit 0
