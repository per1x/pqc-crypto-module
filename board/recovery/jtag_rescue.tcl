# jtag_rescue.tcl —— 用 JTAG 把一块 BOOT.BIN 已损坏的板子救回来
#
#   xsct board/recovery/jtag_rescue.tcl
#
# ============================================================================
# 【这个脚本存在的意义：它是所有启动链改动的许可证】
# ============================================================================
# 在有 JTAG 之前，这块板上**任何**动 BOOT.BIN 的实验都被同一件事卡死：
# multiboot 由 POR 清零，所以坏镜像断电也救不回来 —— golden 一旦写坏就是砖。
# 后果不是"少一个功能"，而是整条启动链的加固都不敢做，于是密码边界只能退到
# "运行时 fpgautil 装 bitstream"这种任何 root 都能替换的形态。
#
# 所以：**先证明能救，再动任何一处。**
#
# ============================================================================
# 【三件必须做对的事 —— 每一件都是一次失败换来的】
# ============================================================================
#
# **① 必须自己把 PL 也装上。** `rst -system` 会清空 PL，而**这块板的 eth0
#    （AXI Ethernet）在厂家 PL 里**。不装 PL 的话，就算 Linux 起来了也没有
#    网络 —— 等于救了一块看不见的板子，而且你会误以为没救活。
#
# **② FSBL 要 JTAG 启动模式，之后必须改回 SD。** 两者读同一个寄存器
#    BOOT_MODE_USER(0xFF5E0200)。不强制 JTAG，FSBL 会去读那份坏掉的
#    BOOT.BIN；改不回来，后面的东西又会认为"没有设备可加载"。
#
# **③ 不要经过 U-Boot。** 实测 U-Boot 起来之后**停在提示符不引导**：
#    这块板的串口在 USB 层是坏的，RX 上的噪声把 autoboot 倒计时打断了，
#    而我们又看不到、敲不了。表现是"U-Boot 明明在跑、Linux 就是不起来"，
#    PC 停在同一个地址不动。
#    所以正式路径是 **JTAG 直接把 kernel + 设备树 + initramfs 装进 DDR
#    并跳过去**，一个交互都不需要。
#
# ============================================================================
# 【JTAG 救不了什么 —— 说清楚，别让人以为有了它就万无一失】
# ============================================================================
# **JTAG 做不到 POR。** 三条都实测过：
#   · `rst -system`  发出去板子不会重新走引导；
#   · `rst -por`     ZynqMP 不支持（那是 Versal 的选项）；
#   · `rst -srst`    回 "Jtag scan chain does not support SRST pin" ——
#                    这条 Digilent 线的 SRST 引脚没接到板子复位。
# 也就是说：**JTAG 能把代码跑起来，但换不回一次干净的上电。**
# 需要 POR 的场合（比如清 CSU_MULTI_BOOT）仍然只能物理断电。
set IMG /home/build/petalinux/images/linux
set PLBIT /home/build/wdt_patch/images/pl_fanquiet.bit
set RESCUE_DTB /tmp/rescue.dtb
set SHIM /tmp/shim.bin
set RESCUE_INITRAMFS /tmp/rescue_initramfs.gz
set PSU_INIT /home/build/factory_vivado/board_test.srcs/sources_1/bd/design_1/ip/design_1_zynq_ultra_ps_e_0_1/psu_init.tcl

connect -url tcp:127.0.0.1:3121
after 1500

# ---- 1. 强制 JTAG 启动模式，复位 ----
targets -set -filter {name =~ "PSU"}
mwr 0xFF5E0200 0x0100
rst -system
after 3000
puts "1/6 已强制 JTAG 启动模式并系统复位"

# ---- 关于 PMUFW：这条路走不通，也不需要走 ----
# 曾经在这里插过一步"JTAG 灌 PMUFW"，因为 BL31 要和 PMU 固件握手。
# 两件事让它出局：
#   · PMU 目标不接受 JTAG 下载，回 "Invalid context"；
#   · 而且**根本不需要** —— BL31 已经从救砖路径上拿掉了（见 bl33_shim.S）。
# 留这几行是为了下一个看到 ipi_mb_notify 自旋的人不用再走一遍这条死路。

# ---- 3. FSBL：它自己做 psu_init，建时钟与 DDR ----
targets -set -filter {name =~ "Cortex-A53 #0"}
rst -processor
dow $IMG/zynqmp_fsbl.elf
con
after 8000
stop
puts "3/6 FSBL 跑完"

# ---- 4. 验 DDR 真的活了 —— 不验就会把内核装进一片死内存 ----
targets -set -filter {name =~ "PSU"}
mwr -force 0x10000000 0xA5A5A5A5
set rb [lindex [split [mrd -force -value 0x10000000 1]] 0]
# ⚠️ mrd -value 返回**十进制**。按字符串和 "A5A5A5A5" 比会误报 DDR 坏。
if {$rb != 0xA5A5A5A5} {
    puts "错误：DDR 回读 $rb（十进制），DDR 没起来，停"
    exit 1
}
puts "4/6 DDR 回读正确"

# ---- 4.5 装 PL —— **必须在 FSBL 之后** ----
# 这块板的 eth0（AXI Ethernet）在 PL 里，不装 PL 就没有网，
# 而"能 SSH 进去"正是救砖成功的判据 —— 没网等于救了一块看不见的板子。
#
# ⚠️ 顺序踩过一次：最早把这一步放在 FSBL **之前**，结果内核确实起来了
#    （PC 落在内核空闲循环里、CPSR 显示 EL1h）却始终不通网。原因是 FSBL 的
#    psu_init 会重置 PS-PL 接口，把先装好的 PL 又推翻了。
#    所以必须 FSBL 先跑、PL 后装。
targets -set -filter {name =~ "PL"}
fpga -file $PLBIT
puts "4.5/6 PL 已装（厂家设计，含 eth0）"

# ---- 4.6 解除 PS-PL 隔离并配 fabric 复位 ----
# ⚠️ 装了 PL 不等于 PS 够得到它。正常启动里，FSBL 在**交接**那一刻解除
#    PS-PL 隔离；而我们的 FSBL 是 JTAG 跑起来的、没有交接对象，这一步没发生。
#    于是 PL 装上了、时钟也有，但 AXI 主口被隔离挡着，AXI Ethernet 形同不存在
#    —— 症状正是"内核起来了（PC 在空闲循环、CPSR=EL1h）却始终没有网"。
#
# 寄存器序列抄自厂家工程 psu_init.tcl 的 psu_ps_pl_isolation_removal /
# psu_ps_pl_reset_config（与这份 PL 位流同源）。**不 source 那个文件**：
# 它的 init_ps/mask_write 依赖一堆在 xsct 里跑不起来的辅助过程，
# 而这里需要的只是九次读改写，自己实现更短也更清楚。
proc mw {addr mask val} {
    set cur [lindex [split [mrd -force -value $addr 1]] 0]
    set new [expr {($cur & ~$mask) | ($val & $mask)}]
    mwr -force $addr $new
}
targets -set -filter {name =~ "PSU"}
mw 0xFFD80118 0x00800000 0x00800000   ;# PL 上电请求中断使能
mw 0xFFD80120 0x00800000 0x00800000   ;# 触发上电请求
mw 0xFD1A0100 0x00001F80 0x00000000   ;# 撤销 PL 复位
mw 0xFF5E023C 0x00080000 0x00000000   ;# 开 PL 参考时钟
mw 0xFF419000 0x00000300 0x00000000
mw 0xFD380000 0x00000003 0x00000000   ;# AFI FM 隔离
mw 0xFD390000 0x00000003 0x00000000
mw 0xFD380014 0x00000003 0x00000000
mw 0xFD390014 0x00000003 0x00000000
# fabric 复位（经 EMIO GPIO 打一个脉冲）
mw 0xFF0A002C 0xFFFF0000 0x80000000
mw 0xFF0A0344 0xFFFFFFFF 0x80000000
mw 0xFF0A0348 0xFFFFFFFF 0x80000000
mw 0xFF0A0054 0xFFFFFFFF 0x80000000
mw 0xFF0A0054 0xFFFFFFFF 0x00000000
mw 0xFF0A0054 0xFFFFFFFF 0x80000000
puts "4.6/6 PS-PL 隔离已解除、fabric 复位已打"

# ---- 5. 启动模式改回 SD —— 见 ② ----
targets -set -filter {name =~ "PSU"}
mwr 0xFF5E0200 0x0000
puts "5/6 启动模式已还原成 SD"

# ---- 6. 装内核、设备树、initramfs、引导垫片 ----
# ⚠️ **地址不能撞。** 早先把 dtb 放在 0x0010_0000，而内核 17 MB 装在
#    0x0008_0000 会一直盖到约 0x010D_8000 —— dtb 正落在内核镜像里。
#    症状是"什么都装好了就是不起来"，看不出是被覆盖。
#    现在的布局（互不重叠，留足余量）：
#       0x0008_0000  Image       17.4 MB → 到 ~0x010D_8000
#       0x0200_0000  设备树      32 MB 处（救砖专用那份，补了 initrd 指针）
#       0x0400_0000  initramfs   64 MB 处，10 MB → 到 ~0x0498_C000
#       0x0800_0000  引导垫片    128 MB 处，80 字节
#    垫片里编进去的 DTB_ADDR / KERNEL_ADDR 必须与这里逐字一致（bl33_shim.S）。
targets -set -filter {name =~ "Cortex-A53 #0"}
dow -data $IMG/Image           0x00080000
dow -data $RESCUE_DTB          0x02000000
dow -data $RESCUE_INITRAMFS    0x04000000
dow -data $SHIM                0x08000000
puts "6/7 kernel + 设备树 + initramfs + 引导垫片 已装进 DDR"

# ---- 关于 GIC：这一步在垫片里做，不在这里 ----
# 曾经在这里写 GICD_IGROUPR，失败：
#     Memory write error at 0xF9010080, DAP status 0x30000021
# JTAG 的内存访问是非安全的，而那组寄存器只有安全世界能写。
# 已经挪进 bl33_shim.S —— 垫片跑在 EL3，写得了。

# ---- 7. 把 PC 指到垫片，放手 ----
# **不经 BL31。** 理由写在 bl33_shim.S 的文件头：BL31 会卡在等 PMU 固件应答
# 的 IPI 自旋里，而 PMUFW 正常是从 BOOT.BIN 里来的 —— 救砖时那个文件恰恰是
# 坏的。垫片自己做 EL3→EL2 降级，把 BL31 这个依赖整个去掉。
targets -set -filter {name =~ "Cortex-A53 #0"}
rwr pc 0x08000000
con
puts "7/7 已跳进引导垫片 —— 它会降到 EL2 并进入内核"
puts ""
puts "救砖 init 会自动把 BOOT.BIN 从备份恢复，然后停住等断电。"
puts "**不依赖网络、不依赖串口、不需要人工干预** —— 这是有意的："
puts "救砖环境里能少一个依赖就少一个，而网络恰恰依赖 PL、依赖隔离解除、"
puts "依赖 PHY 起来，任何一环出问题都会让人误以为没救活。"
puts ""
puts "断电后正常启动，去 /media/sd-mmcblk1p2/hsm/RESCUE_DONE.txt 看结果。"
