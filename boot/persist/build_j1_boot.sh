#!/bin/bash
# build_j1_boot.sh —— J1：把密码边界固化进 BOOT.BIN 的 PL 段
#
# 在构建机上跑。产物 BOOT_J1_PL.BIN，只落盘，不刷卡。
#
# ============================================================================
# 【J1 改了什么，为什么这是最高性价比的加固】
# ============================================================================
# golden 的 bif 里 [destination_device=pl] 指向厂家的 top.bit（那份"开机满速吵"
# 的位流）。运行时再由 fpgautil 从 SD 上一个 root 可替换的 .bit 装密码边界 ——
# 也就是说**任何 root 都能换掉整个密码边界**，而 SECURITY.md 把这一条列为"不防"。
#
# J1 把 [destination_device=pl] 换成密码位流 zu3eg_hsm.bit：
#   · 密码边界（firewall / key_vault / 四核 SECURE_ONLY=1）由 FSBL 从**受控
#     镜像**加载，不再是 SD 上一个文件；换掉它得重打整个 BOOT.BIN；
#   · 密码位流**自带风扇温控**（SYSMONE4 → fan_ctrl → AA11，完全不经 AXI），
#     于是"开机就静"顺带解决 —— 不用再跑 fanquiet-init.sh。
#
# BL31 用 atf_secmmio 那份（带受限安全 MMIO 读写 SiP），服务层要靠它从
# 安全世界驱动 SECURE_ONLY=1 的核。
#
# ============================================================================
# 【为什么刷非 golden 槽 + golden 兜底】
# ============================================================================
# 产物写进 **BOOT0007.BIN**（新槽，不覆盖任何已有槽），CSU_MULTI_BOOT 指过去。
# multiboot 由 POR 清零 → **任何一次断电都自动回到 golden BOOT.BIN**。
# 这是没有真 POR 级 JTAG 恢复时能拿到的最好兜底：最坏"要断一次电"，不是砖。
# 而现在 J0 又给了 JTAG 引导 + cp 恢复，兜底更厚了一层。
# golden / BOOT0001..0006 一个字节都不动。
#
# ⚠️ J1 镜像装了密码 PL → **eth0 没了**（eth0 在厂家 PL 里）。所以它起来之后
#    **不能 SSH**，验证走 JTAG console + /dev/secmmio。这是有意的：送检形态
#    本就该是安全世界驱动、普通世界零可达。
set -e
source /tools/Xilinx/Vitis/2020.1/settings64.sh
BIF=/home/build/pqc-hsm-fpga/boot/persist/boot_j1_pl.bif
OUT=/home/build/wdt_patch/images/BOOT_J1_PL.BIN

echo "== 核对 bif 引用的文件都在 =="
grep -oE "/home/[^ ]*\.(elf|bit|dtb)" $BIF | while read f; do
    [ -f "$f" ] && echo "  OK  $f" || { echo "  缺  $f"; exit 1; }
done

echo "== bootgen =="
bootgen -image $BIF -arch zynqmp -w -o $OUT 2>&1 | grep -E "ERROR|WARNING" || true
ls -l $OUT
md5sum $OUT
echo "DONE（只落盘，没刷卡）"
