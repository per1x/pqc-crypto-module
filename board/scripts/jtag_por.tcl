# jtag_por.tcl —— 用 JTAG 远程触发一次**真 POR**，不用人去拔电源。
#
# ============================================================================
# 【为什么这条以前被当成"做不到"】
# ============================================================================
# 本项目此前反复记录"这颗芯片没有软件 POR 通路，只能物理断电"，依据是 xsct 的
# 三种复位在这块板上都不支持：
#     rst -por   → por not supported for target      （JTAG 线没引 POR 脚）
#     rst -srst  → Jtag scan chain does not support SRST pin
#     rst -ps    → ps reset not suported for target
# 直写 CRL_APB.RESET_CTRL 也只是系统复位。
#
# **但那只说明"JTAG 线没法拉 POR 引脚"，不等于"芯片没有软件 POR 通路"。**
# UG1085 第 6 章（PMU）写着，每个错误源都可以配置成触发下列动作之一：
#     · 拉 PS_ERROR_OUT
#     · 给 PMU 发中断
#     · 产生系统复位（SRST）
#     · **产生上电复位（POR）**
# 掩码由 PMU_GLOBAL.ERROR_POR_EN_{1,2} 写 1 解除。于是只要有一个错误源是**已经
# 置起来的**，给它解掩码，PMU 就会立刻发一次 POR。
#
# ============================================================================
# 【本脚本用的错误源：CSU_ROM（ERROR_STATUS_2 bit26）】
# ============================================================================
# 只要上一次启动 BootROM 报过错（CSU_BR_ERROR 的 bit31 BR_ERROR = 1），
# 这一位就是置着的 —— 试加密启动失败之后必然满足。
# 如果这一位是 0（上次启动完全干净），本脚本会直接告诉你，不会瞎写。
#
# ⚠️ 与物理断电的差别：这是 **PS 的 POR**，不掉电源轨。
#    对 multiboot、安全态、CSU/XMPU 的 POR-only 闩锁而言等价；
#    对**掉电才会丢的东西（例如没有电池的 BBRAM）不一定等价** —— 那条要单独验。
#
# 2026-08-17 实测：往 PERS_GLOB_GEN_STORAGE7（文档明写"只有 POR 会复位"）写
# 0xDEADBEEF，触发后读回 0x00000000 —— **确认是真 POR**，板子 35 秒回演示形态。
#
# 用法（构建机上）：
#   sudo -H -u build bash -lc \
#     "source /tools/Xilinx/Vitis/2020.1/settings64.sh; xsct jtag_por.tcl"

connect -url tcp:127.0.0.1:3121
targets -set -filter {name =~ "PSU"}

set st [lindex [mrd -force 0xFFD80540] 1]
scan $st %x stn
puts "ERROR_STATUS_2 = 0x$st   (bit26 = CSU_ROM)"

if {($stn & 0x04000000) == 0} {
    puts "⚠️ CSU_ROM 错误位没有置起来，这个触发源现在用不了。"
    puts "   办法：先让 BootROM 失败一次（例如把一个起不来的镜像放非 golden 槽热重启），"
    puts "   或者换一个当前已置位的错误源（读 ERROR_STATUS_1/2 挑一个）。"
    exit 1
}

# POR-only 的探针，用来事后证明真的发生了 POR
mwr -force 0xFFD8006C 0xDEADBEEF
puts "探针 PERS_GLOB_GEN_STORAGE7 = 0x[lindex [mrd -force 0xFFD8006C] 1]"

puts "触发 POR（给 CSU_ROM 错误解掩码）…"
catch {mwr -force 0xFFD80560 0x04000000}
puts "已写 ERROR_POR_EN_2 = 0x04000000 —— 板子应当立刻 POR，约 35 秒后回到演示形态。"
puts ""
puts "事后核验：再连一次读 0xFFD8006C，读到 0x00000000 就是真 POR 了。"
