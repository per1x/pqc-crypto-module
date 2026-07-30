# build_bitstream.tcl —— 综合、实现、生成比特流与硬件交接文件
#
# 【需要在装有 Vivado 的机器上执行】本仓库的开发机上没有 Vivado，因此这个脚本
# 从未运行过。脚本本身是完整的，但"综合能过、时序能收敛"这两件事没有任何证据
# 支撑，第一次跑必须逐条看报告。
#
# 用法：
#     vivado -mode batch -source board/xc7z020/vivado/build_bitstream.tcl \
#            -tclargs -proj <path>/pqc_accel_pynq_z2.xpr [-jobs 8]
#
# 产物（放在工程目录下的 outputs/）：
#     <bd>_wrapper.bit    比特流
#     <bd>.hwh            硬件交接文件，PYNQ 的 Overlay 要它
#     <bd>_wrapper.xsa    硬件平台描述，PetaLinux 要它
#     utilization.rpt     资源占用
#     timing.rpt          时序汇总
#
# 三份报告都要留档：资源占用用来核对 board/xc7z020/docs/resource-budget.md 里的
# 估算，时序报告用来确认 100 MHz 是否真的收敛。

set opt_proj ""
set opt_jobs 4

for {set i 0} {$i < [llength $argv]} {incr i} {
    set a [lindex $argv $i]
    switch -- $a {
        -proj { incr i; set opt_proj [lindex $argv $i] }
        -jobs { incr i; set opt_jobs [lindex $argv $i] }
        default { puts "忽略未知参数：$a" }
    }
}

if {$opt_proj eq "" || ![file exists $opt_proj]} {
    puts "用法：vivado -mode batch -source build_bitstream.tcl -tclargs -proj <path>.xpr"
    exit 1
}

open_project $opt_proj
set proj_dir [get_property directory [current_project]]
set out_dir  $proj_dir/outputs
file mkdir $out_dir

update_compile_order -fileset sources_1

# ---- 综合 ----
launch_runs synth_1 -jobs $opt_jobs
wait_on_run synth_1
if {[get_property PROGRESS [get_runs synth_1]] != "100%"} {
    puts "综合失败，见 [get_property DIRECTORY [get_runs synth_1]]"
    exit 1
}

open_run synth_1 -name synth_1
report_utilization -file $out_dir/utilization_synth.rpt
report_timing_summary -file $out_dir/timing_synth.rpt

# ---- 实现 ----
launch_runs impl_1 -to_step write_bitstream -jobs $opt_jobs
wait_on_run impl_1
if {[get_property PROGRESS [get_runs impl_1]] != "100%"} {
    puts "实现失败，见 [get_property DIRECTORY [get_runs impl_1]]"
    exit 1
}

open_run impl_1
report_utilization -file $out_dir/utilization.rpt
report_timing_summary -file $out_dir/timing.rpt

# 时序不收敛不能当成"只是个警告"：加速器的结果会随机出错，而且错得没有规律。
set wns [get_property SLACK [get_timing_paths -delay_type max]]
set whs [get_property SLACK [get_timing_paths -delay_type min]]
puts "建立时间余量 WNS = $wns ns，保持时间余量 WHS = $whs ns"
if {$wns < 0 || $whs < 0} {
    puts "时序未收敛。降低 PCW_FPGA0_PERIPHERAL_FREQMHZ 或给关键路径加流水级，"
    puts "不要带着负余量往下走。"
    exit 1
}

# ---- 产物 ----
set bd_file [get_files -filter {FILE_TYPE == "Block Designs"}]
set bd_name [file rootname [file tail $bd_file]]

# glob 找不到匹配时会抛错。这里显式判断，给出"缺什么"而不是一行 Tcl 报错 ——
# 这一步跑在整条流程的最后，报错信息含糊会让人从头再排一遍。
set bit_files [glob -nocomplain $proj_dir/*.runs/impl_1/*.bit]
if {[llength $bit_files] == 0} {
    puts "实现目录里没有 .bit：检查 impl_1 是否真的跑到了 write_bitstream"
    exit 1
}
foreach f $bit_files { file copy -force $f $out_dir/ }
# .hwh 由 block design 生成，PYNQ 的 Overlay 靠它解析地址与 IP 列表
set hwh [glob -nocomplain $proj_dir/*.gen/sources_1/bd/$bd_name/hw_handoff/${bd_name}.hwh]
if {$hwh ne ""} {
    file copy -force $hwh $out_dir/
} else {
    puts "没找到 .hwh —— PYNQ 流程需要它，检查 block design 是否已生成输出产物"
}

write_hw_platform -fixed -include_bit -force $out_dir/${bd_name}_wrapper.xsa

puts ""
puts "产物已放到 $out_dir："
foreach f [lsort [glob -nocomplain $out_dir/*]] { puts "  [file tail $f]" }
puts ""
puts "核对清单："
puts "  1. utilization.rpt 与 docs/resource-budget.md 的估算是否吻合"
puts "  2. timing.rpt 的 WNS/WHS 是否为正"
puts "  3. .bit 与 .hwh 同名同版本 —— PYNQ 靠文件名配对，错配会读到旧地址表"
