# ooc_mldsa.tcl —— 按**参数集**做 ML-DSA 的 out-of-context 综合
#
# 为什么单独一份而不用 ooc_synth.tcl：那份不接受参数覆盖，直接综合
# mldsa_sign 量到的是默认值（ML-DSA-44）的面积。而决定"能不能塞进 ZU3EG"
# 的是**最大的 87**，所以必须能把参数打进去。这里用 synth_design -generic。
#
#   vivado -mode batch -source hardware/syn/ooc_mldsa.tcl \
#          -tclargs <part> <keygen|sign|verify|engine> <44|65|87>
#
# 例：
#   vivado -mode batch -source ooc_mldsa.tcl \
#          -tclargs xazu3eg-sfvc784-1-i sign 87
#
# 产物：syn/rpt/mldsa_<op>_<pset>_{utilization,timing}_{synth,route}.rpt
#
# ⚠️ 报告里的 Fmax 以 **post-route** 的 WNS 反推为准；post-synth 偏乐观。

set part  [lindex $argv 0]
set op    [lindex $argv 1]
set pset  [lindex $argv 2]
if {$part eq "" || $op eq "" || $pset eq ""} {
    puts "用法: vivado -mode batch -source ooc_mldsa.tcl -tclargs <part> <keygen|sign|verify|engine> <44|65|87>"
    exit 1
}

# FIPS 204 Table 1。keygen 只有 K/L/ETA；verify 没有 ETA（见各模块端口）。
switch $pset {
    44 { set K 4; set L 4; set ETA 2; set TAU 39; set G1 17; set MODE 0; set OMG 80; set BETA 78;  set CTB 32; set PS 0 }
    65 { set K 6; set L 5; set ETA 4; set TAU 49; set G1 19; set MODE 1; set OMG 55; set BETA 196; set CTB 48; set PS 1 }
    87 { set K 8; set L 7; set ETA 2; set TAU 60; set G1 19; set MODE 1; set OMG 75; set BETA 120; set CTB 64; set PS 2 }
    default { puts "未知参数集 $pset"; exit 1 }
}

set root [file normalize [file dirname [info script]]/..]
set rpt  $root/syn/rpt
file mkdir $rpt

read_verilog -sv [glob $root/rtl/common/*.v]
read_verilog -sv [glob $root/rtl/mlkem/*.v]
read_verilog -sv [glob $root/rtl/mldsa/*.v]
read_verilog -sv [glob $root/rtl/keccak/*.v]
read_verilog -sv [glob $root/rtl/bus/*.v]
read_verilog -sv [glob $root/rtl/sym/*.v]
read_verilog -sv [glob $root/rtl/trng/*.v]

# 三个模块都有真实 clk 端口 → 用时序约束那份
read_xdc $root/syn/constraints/ooc_seq.xdc

set top "mldsa_$op"
switch $op {
    keygen { set gen [list K=$K L=$L ETA=$ETA] }
    sign   { set gen [list K=$K L=$L ETA=$ETA TAU=$TAU G1LOG=$G1 MODE=$MODE OMG=$OMG BETA=$BETA CTB=$CTB] }
    verify { set gen [list K=$K L=$L TAU=$TAU G1LOG=$G1 MODE=$MODE OMG=$OMG BETA=$BETA CTB=$CTB] }
    engine { set gen [list K=$K L=$L ETA=$ETA TAU=$TAU G1LOG=$G1 MODE=$MODE OMG=$OMG BETA=$BETA CTB=$CTB PSET=$PS] }
    default { puts "未知操作 $op"; exit 1 }
}

set gargs {}
foreach g $gen { lappend gargs -generic $g }

puts "=== OOC: $top @ ML-DSA-$pset  ($gen) ==="
eval synth_design -top $top -part $part -mode out_of_context \
     -flatten_hierarchy rebuilt $gargs

set tag "mldsa_${op}_${pset}"
report_utilization    -file $rpt/${tag}_utilization_synth.rpt
report_timing_summary -file $rpt/${tag}_timing_synth.rpt

opt_design
place_design
route_design
report_utilization    -file $rpt/${tag}_utilization_route.rpt
report_timing_summary -file $rpt/${tag}_timing_route.rpt
puts "=== 完成: $tag ==="
