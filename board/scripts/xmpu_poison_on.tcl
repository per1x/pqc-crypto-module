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
# 【⚠️ 这是运行时的，重启就没了】
# ============================================================================
# 要让它开机就生效，得由 BL31 在启动时写（`boot/atf/patch_atf_secmmio.py` 里
# 已经写好那一行）。但**当前从这棵 ATF 树重建出来的 BL31 起不来网络**，
# 与毒化无关（去掉毒化那行的对照版同样起不来），诊断需要串口，而串口正处在
# 那个需要人手拔插的坏状态。详见 docs/STATUS-2026-08-17.zh-CN.md。
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
