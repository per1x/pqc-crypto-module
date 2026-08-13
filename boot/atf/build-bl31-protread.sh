#!/bin/bash
# 编带 PL_SECREAD + PROT_READ 两个 SiP 的 BL31，打成非 golden 槽的 BOOT0005.BIN
#
# 这一版比 BOOT0004 多的只有一个**只读**的保护单元读口（0x8200ff11）
# 与它需要的那段页表映射（0xFD00_0000 起 6 MB）。见 patch_atf_protread.py。
#
# ============================================================================
# 【为什么参数照抄 atf_build_v50，而不是"精简掉用不上的"】
# ============================================================================
# BOOT_OPTEE_FIXQ.BIN 是这块板上**已知 10/10 能启动**的镜像，它用的就是
# atf_build_v50 那份 BL31（SPD=opteed DEBUG=1 LOG_LEVEL=50）。
# 我要加的只有一个 SiP 分支，所以其余一切保持逐字相同 —— 这样万一起不来，
# 变量只有一个。看着"用不上就去掉 SPD=opteed"很合理，但那会同时改掉
# BL32 的交接路径，等于一次动两处。没有 JTAG 的板子上不这么干。
#
# 镜像结构也照 boot_fix_quiet.bif，只把 PL bitstream 换成验证过的
# 「厂家设计 + 风扇」那份（金丝雀在密码 bitstream 里，运行时再载 ——
# SiP 是按需调用的，调用那一刻 PL 里是什么就读什么）。
#
# ============================================================================
# 【为什么敢刷：掉电自动回黄金镜像】
# ============================================================================
# 产物写进 **BOOT0005.BIN**（新文件，不覆盖任何已有槽，也不动 BOOT0004），靠 CSU_MULTI_BOOT
# 指过去。而 multiboot **由 POR 清零** —— 也就是说这份镜像只在一次热重启内
# 有效，**任何一次断电都自动回到 BOOT.BIN（黄金）**。这是没有 JTAG 时能拿到
# 的最好兜底：最坏情况是"要断一次电"，而不是"砖"。
# golden BOOT.BIN / BOOT0001 / BOOT0002 / BOOT0003 一个字节都不动。
set -e
source /tools/Xilinx/Vitis/2020.1/settings64.sh
export PATH=/tools/Xilinx/Vitis/2020.1/gnu/aarch64/lin/aarch64-none/bin:$PATH

ATF=/home/build/wdt_patch/atf
OUT=/home/build/wdt_patch/atf_protread
IMG=/home/build/wdt_patch/images

echo "======== 编 BL31（参数与 atf_build_v50 一致）"
cd "$ATF"
make CROSS_COMPILE=aarch64-none-elf- PLAT=zynqmp SPD=opteed DEBUG=1 \
     LOG_LEVEL=50 BUILD_BASE="$OUT" bl31 -j4 2>&1 | tail -8

NEW="$OUT/zynqmp/debug/bl31/bl31.elf"
OLD=/home/build/wdt_patch/atf_build_v50/zynqmp/debug/bl31/bl31.elf
ls -l "$NEW" "$OLD"

echo "======== 装载布局对比（应当只差几十字节，段的起止要一致）"
aarch64-none-elf-readelf -l "$OLD" | grep -A1 LOAD | head -6
echo "  ---- 新 ----"
aarch64-none-elf-readelf -l "$NEW" | grep -A1 LOAD | head -6

echo "======== 确认两个 SiP 分支都进去了"
# ⚠️ **别 grep "ff11"，那个数永远是 0，而且看起来像是补丁没打上。**
# GCC 把 0x8200ff11 编成 "0x8200ff10 再加 1"（两个 case 相邻），
# 四个地址窗口的边界也编成"加一个负偏移再比大小"，于是
# ff11 / ff98 / fd5d 一个都不会以字面量出现在反汇编里。
# 真正能判的是那四个负偏移常量 —— 它们是这段代码独有的：
#   0x68 lsl 16  → a - 0xFF980000   XPPU
#   0x300 lsl 16 → a - 0xFD000000   XMPU_DDR0..5
#   0x2a3 lsl 16 → a - 0xFD5D0000   XMPU_FPD
#   0x59 lsl 16  → a - 0xFFA70000   XMPU_OCM
echo "  PL_SECREAD（ff10）出现次数（应 ≥1）："
aarch64-none-elf-objdump -d "$NEW" | grep -c "ff10" || true
echo "  四个保护单元窗口的边界常量（应当四个都是 1）："
for k in "0x68, lsl" "0x300, lsl" "0x2a3, lsl" "0x59, lsl"; do
  printf "    %-14s %s\n" "$k" "$(aarch64-none-elf-objdump -d "$NEW" | grep -c "$k" || true)"
done

echo "======== 打 BOOT_PROT.BIN（要放到 BOOT0005.BIN）"
cd "$IMG"
cat > boot_protread.bif <<BIF
the_ROM_image:
{
	[bootloader, destination_cpu=a53-0] /home/build/petalinux/images/linux/zynqmp_fsbl.elf
	[pmufw_image] /home/build/petalinux/images/linux/pmufw.elf
	[destination_device=pl] /home/build/wdt_patch/images/pl_fanquiet.bit
	[destination_cpu=a53-0, exception_level=el-3, trustzone] $NEW
	[destination_cpu=a53-0, exception_level=el-1, trustzone] /home/build/wdt_patch/images/tee_load_quiet.elf
	[destination_cpu=a53-0, load=0x00100000] /home/build/petalinux/images/linux/system.dtb
	[destination_cpu=a53-0, exception_level=el-2] /home/build/petalinux/images/linux/u-boot.elf
}
BIF
bootgen -image boot_protread.bif -arch zynqmp -w -o BOOT_PROT.BIN 2>&1 | grep -E "ERROR|WARNING" || true
ls -l BOOT_PROT.BIN BOOT_OPTEE_FIXQ.BIN
md5sum BOOT_PROT.BIN
echo "DONE（只落盘，没刷卡）"
