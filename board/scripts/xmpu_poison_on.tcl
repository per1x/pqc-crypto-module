# xmpu_poison_on.tcl —— 把 XMPU_DDR 的「按属性毒化」打开，让 OP-TEE 的安全内存
#                       真的挡住普通世界的读。
#
# ============================================================================
# 【为什么需要这一步 —— XMPU 本身不拦事务】
# ============================================================================
# XAPP1320《Isolation Methods in Zynq UltraScale+ MPSoCs》v3.0 第 10 页：
#
#   "If an illegal transaction is attempted, the XMPU asserts AxUser[10] but the
#    transaction **is passed to the memory controller** … **The transaction is
#    gated by the end point, not the XMPU itself.**"
#
# 所以只配区域 = 只有**检测**。BL31 配好的六个 XMPU_DDR 区域确实把 APU 的非安全
# 访问判成了 SECURTYVIO（ERR_MASTER 落在 0x80-0xBF = APU），但 `devmem 0x60000000`
# 照样读回 OP-TEE 的代码 —— 因为 POISON 寄存器是 0，没有东西去 gate。
#
# 把 POISON.ATTRIB（bit20）置上之后，2026-08-17 板上实测：
#     毒化前  devmem 0x60000000 → 0xAA0003F3   （OP-TEE 的一条 AArch64 指令）
#     毒化后  devmem 0x60000000 → Bus error，退出码 135
#     区域外  devmem 0x10000000 → 0xEDFE0DD0   （照常，没误伤）
#     0x70000000（OP-TEE 共享内存，故意不圈）→ 照常
#     sdf_demo 九节全绿，daemon/网络/PL 都不受影响
#
# ============================================================================
# 【为什么只动 POISON、不动 CTRL】
# ============================================================================
# CTRL 现在是 0x0b = DEFRDALWD | DEFWRALWD | ALIGNCFG。
#   · DEFRD/DEFWR 管的是"不匹配任何区域的访问"—— 清掉等于把整个 DDR 都拒了，
#     板子起不来。**绝不能动。**
#   · bit2 POISONCFG 在 Xilinx 的 QEMU 模型里走的是**按地址**重定向（把违规访问
#     打到 POISON.BASE）。BASE 现在是 0，一次违规的**写**就会打到物理地址 0 ——
#     在把 BASE 指到安全去处之前绝不能置它。
# 所以这里走 XAPP1320 明确推荐的「按属性」：CTRL 保持不动，只置 ATTRIB。
#
# ============================================================================
# 【⚠️ 2026-08-17 收尾：这个脚本已经不是必需步骤了】
# ============================================================================
# **默认镜像开机就把毒化打开了**（BL31 在 EL3 里写，见 patch_atf_secmmio.py）。
# 上电 35 秒后 `devmem 0x60000000` 直接就是 Bus error，不用跑这个脚本。
#
# 这个脚本现在只剩两个用途：
#   ① 应急：万一板子落到某个**不带**这段 BL31 的镜像（比如 BOOT.BIN 起不来、
#      FSBL 自增落到 BOOT0001.BIN），可以用它把毒化补上；
#   ② 演示对照：配合 xmpu_poison_off 那几行把 POISON 清零，让
#      `devmem 0x60000000` 读回 0xAA0003F3，**反证"没有 XMPU 时 root 读得到"**。
#      清零之后直接重启就恢复 —— BL31 会自己写回去。
#
# 曾经写在这里的「重建出来的 BL31 起不来网络」是**错的**：它起得来，只是被
# LOG_LEVEL=50 的页表转储拖过了 FSBL 那条 100 秒看门狗线，于是 multiboot++
# 落到了别的槽。降到 LOG_LEVEL=20 就好了。
# 详见 board/logs/RESULT_slot_boot_wdt.txt 与 RESULT_xmpu_persist.txt。
#
# 用法（在构建机上跑）：
#   sudo -H -u build bash -lc \
#     "source /tools/Xilinx/Vitis/2020.1/settings64.sh; xsct xmpu_poison_on.tcl"

connect -url tcp:127.0.0.1:3121
targets -set -filter {name =~ "PSU"}

puts "=== 打开前 ==="
for {set i 0} {$i < 6} {incr i} {
    set inst [expr 0xFD000000 + $i * 0x10000]
    puts "  DDR$i CTRL=0x[lindex [mrd -force $inst] 1] POISON=0x[lindex [mrd -force [expr $inst + 0x0C]] 1]"
}

# POISON: [31:20] ATTRIB，[19:0] BASE。置 ATTRIB=1，BASE 保持 0（不做地址重定向）。
for {set i 0} {$i < 6} {incr i} {
    set inst [expr 0xFD000000 + $i * 0x10000]
    mwr -force [expr $inst + 0x0C] 0x00100000
}

puts "=== 打开后 ==="
set ok 1
for {set i 0} {$i < 6} {incr i} {
    set inst [expr 0xFD000000 + $i * 0x10000]
    set p [lindex [mrd -force [expr $inst + 0x0C]] 1]
    puts "  DDR$i POISON=0x$p"
    if {$p ne "00100000"} { set ok 0 }
}
if {$ok} {
    puts "六个实例都已开启按属性毒化。"
    puts "判据：到板子上跑 `devmem 0x60000000 32` —— 应当是 Bus error，"
    puts "      而 `devmem 0x10000000 32` 应当照常返回。"
} else {
    puts "⚠️ 有实例没写进去，别当成已加固。"
}
