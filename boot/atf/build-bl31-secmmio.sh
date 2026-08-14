#!/bin/bash
# 编带受限安全 MMIO **读写**服务的 BL31，打成非 golden 槽的 BOOT0005.BIN
#
# 提供两个 SiP（见 patch_atf_secmmio.py）：
#   0x8200ff12  读：x1 = 物理地址          → x0 = 0/~0，x1 = 值
#   0x8200ff13  写：x1 = 物理地址，x2 = 值 → x0 = 0/~0
#
# ⚠️ **这是读写，不是只读。** 这个文件头一度是从 build-bl31-protread.sh
#    整段拷来的，于是把"只读的保护单元读口"这句话一起拷了过来 ——
#    而那描述的是另一个 SiP（0x8200ff11，只读、只放行 XPPU/XMPU 寄存器）。
#    一份安全敏感的构建脚本，头部把自己的能力说小了，比没有注释更糟：
#    审这份脚本的人会以为攻击面只有读。独立评审的 M3 点的就是这一条。
#
#    真正的边界因此是**白名单本身**：只放行 PL 的 0x8000_0000 密码核段，
#    且只到合法寄存器偏移。写口意味着普通世界能改 key_vault 的槽 ——
#    这一条在 docs/SECURITY.md 里显式列为"当前仍具备的残余能力"。
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
OUT=/home/build/wdt_patch/atf_secmmio
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

echo "======== 确认 SiP 真的进去了"
# ⚠️ 别 grep FID 字面量：GCC 会把相邻的 case 值编成"某个值再加 N"，
#    数出来永远是 0，看着像补丁没打上（这个坑栽过一次）。
#    查符号最实在：三张白名单表是这段代码独有的。
for sym in pl_off_max pl_rd_ok pl_wr_ok; do
  printf "  %-12s %s\n" "$sym" "$(aarch64-none-elf-nm "$NEW" | grep -c " $sym\$" || true)"
done
echo "  已删除的旧口子（都应为 0）："
for sym in PROT_READ PL_SECREAD; do
  printf "  %-12s %s\n" "$sym" "$(aarch64-none-elf-objdump -t "$NEW" | grep -ci "$sym" || true)"
done

echo "======== 打 BOOT_SECMMIO.BIN（要放到 BOOT0005.BIN）"
cd "$IMG"
cat > boot_secmmio.bif <<BIF
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
bootgen -image boot_secmmio.bif -arch zynqmp -w -o BOOT_SECMMIO.BIN 2>&1 | grep -E "ERROR|WARNING" || true
ls -l BOOT_SECMMIO.BIN BOOT_OPTEE_FIXQ.BIN
md5sum BOOT_SECMMIO.BIN
echo "DONE（只落盘，没刷卡）"
