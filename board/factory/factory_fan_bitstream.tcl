# factory_fan_bitstream.tcl —— 在**厂家 Vivado 工程**里换掉风扇逻辑，重出 bitstream
#
#   vivado -mode batch -source board/factory/factory_fan_bitstream.tcl
#
# ============================================================================
# 【为什么要动厂家工程，而不是把风扇塞进密码 bitstream 就算完】
# ============================================================================
# 板上的 `eth0` 是 `80000000.ethernet` —— **AXI Ethernet IP 在厂家 PL 里**。
# 密码 bitstream 里没有网卡，运行时载进去网络就断（所以才有 plharness.sh 那套
# 「跑完必定恢复出厂 bit + 重新绑驱动」的兜底）。
#
# 而用户要的是「**开机就静**」，也就是要让 FSBL 在启动时就装上带风扇的
# bitstream。如果那份 bitstream 是密码版的，**那次启动就没有网络**，
# 只能靠串口 —— 不可接受。
#
# 所以持久化那一份必须建立在**厂家设计**之上：网卡、DDR4、MIPI、串口全都在，
# 唯一的改动是把出厂那句 `assign fan = 1'b1;`（钉死满速）换成按结温调速。
#
# 密码核不进这一份。它们仍然按需运行时载入（已在真硅上 24/24 验证过）。
# 把两者合成一份是下一步的事，需要先把密码从 0x8000_0000 挪开 ——
# 那个地址正是厂家 AXI Ethernet 占着的。
#
# ============================================================================
# 【安全边界】
# ============================================================================
# · 本脚本**只写** /home/build/factory_vivado 这个工程目录。
# · 板子 SD 卡上的 golden BOOT.BIN 与所有备份镜像，本脚本一概不碰。
# · 出厂原始 bitstream 另有两份没被动过的副本：
#     /home/build/petalinux/project-spec/hw-description/top.bit
#     板子 SD 卡上的 factory.bit
#   所以即使这个工程被改坏，回退路径仍在。
# · 每次跑之前把上一版 top.bit 另存一份，见下面的 备份 段落。

set proj  /home/build/factory_vivado/board_test.xpr
set src   /home/build/factory_vivado/board_test.srcs/sources_1/new
set fanrtl /home/build/pqc-hsm-fpga/fpga/fan_ctrl

# ---- 备份上一版产物 ----------------------------------------------------------
set prev /home/build/factory_vivado/board_test.runs/impl_1/top.bit
if {[file exists $prev]} {
    set stamp [clock format [clock seconds] -format %Y%m%d-%H%M%S]
    file copy -force $prev /home/build/factory_vivado/top_prev_$stamp.bit
    puts "已备份上一版 bitstream：top_prev_$stamp.bit"
}

open_project $proj

# ---- 换掉风扇 RTL ------------------------------------------------------------
# 工程里原有一个 fan_ctrl.v（另一版实现），模块名与我这份**重名**，必须先摘掉，
# 否则两个 fan_ctrl 撞在一起，综合选哪个都不确定。
set old [get_files -quiet */sources_1/new/fan_ctrl.v]
if {[llength $old] > 0} {
    remove_files $old
    file delete -force $src/fan_ctrl.v
    puts "已移除工程里原有的 fan_ctrl.v（模块重名）"
}

# 我这份三个文件：fan_ctrl（温度→占空比→PWM）、sysmon_drp（DRP 轮询，纯 RTL，
# 进 cocotb 回归）、fan_sysmon（SYSMONE4 原语 + 上面那个）。
# fan_ctrl_axi 不进来 —— 这一份不加观测口，理由见 top.v 末尾那段注释。
file mkdir $src/fan
foreach f {fan_ctrl.v sysmon_drp.v fan_sysmon.v} {
    file copy -force $fanrtl/$f $src/fan/$f
}
add_files -norecurse [glob $src/fan/*.v]

# ---- 换掉顶层 ----------------------------------------------------------------
# board/factory/top.v 是出厂 top.v 加了风扇例化的版本，改动只有末尾那一段。
file copy -force /home/build/pqc-hsm-fpga/board/factory/top.v $src/top.v
update_compile_order -fileset sources_1
set_property top top [current_fileset]

# 管脚约束沿用出厂的 system.xdc（里面本来就有 AA11/LVCMOS33 的 fan）——
# **不新增约束文件**，少一处可能和出厂冲突的地方。

# ---- 跑实现 ------------------------------------------------------------------
reset_run synth_1
launch_runs impl_1 -to_step write_bitstream -jobs 8
wait_on_run impl_1

set st [get_property STATUS [get_runs impl_1]]
set pr [get_property PROGRESS [get_runs impl_1]]
puts "=========================================================="
puts "  impl_1 状态：$st（$pr）"

set wns [get_property STATS.WNS [get_runs impl_1]]
set whs [get_property STATS.WHS [get_runs impl_1]]
puts "  建立时间 WNS = $wns ns"
puts "  保持时间 WHS = $whs ns"
puts "=========================================================="

set bit /home/build/factory_vivado/board_test.runs/impl_1/top.bit
if {![file exists $bit]} {
    puts "错误：没有产出 top.bit"
    exit 1
}
if {$wns < 0 || $whs < 0} {
    puts "错误：时序不收敛 —— 这份 bitstream 是要进 BOOT.BIN 开机跑的，不能将就"
    exit 1
}
puts "产物：$bit"
exit 0
