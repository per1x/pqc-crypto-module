#!/bin/bash
# 编带受限安全 MMIO **读写**服务的 BL31，打成一份放非 golden 槽验证用的镜像
# （产物名见下面的 ${BIN}；验证通过后由人决定要不要提升成 BOOT.BIN）
#
# 提供两个 SiP（见 patch_atf_secmmio.py）：
#   0x8200ff12  读：x1 = 物理地址          → x0 = 0/~0，x1 = 值
#   0x8200ff13  写：x1 = 物理地址，x2 = 值 → x0 = 0/~0
#
# ⚠️ **这是读写，不是只读。** 这个文件头一度是从 build-bl31-protread.sh
#    整段拷来的，于是把"只读的保护单元读口"这句话一起拷了过来 ——
#    而那描述的是另一个 SiP（0x8200ff11，只读、只放行 XPPU/XMPU 寄存器）。
#    一份安全敏感的构建脚本，头部把自己的能力说小了，比没有注释更糟：
#    审这份脚本的人会以为攻击面只有读。
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
# ============================================================================
# 【★ LOG_LEVEL=20，不是 50 —— 50 会把镜像撞死在一条 100 秒的看门狗上】
# ============================================================================
# 这是 2026-08-17 收尾时查清的一条，值得完整记下来，因为它一度被误诊成
# 三件完全不同的事：「重建的 BL31 起不来网络」「重建镜像起不来」「槽 6 时灵时不灵」。
# **三条都是同一个原因。**
#
# 链条是这样的：
#   ① 本项目的 FSBL 打了看门狗补丁（make_wdt_patch.py）：交接给 ATF 之前武装
#      PS SWDT0，**XFSBL_WDT_EXPIRE_TIME = 100 秒**；
#   ② 而且**故意不在 handoff 时停掉它**（xfsbl_handoff.c 那段补丁的原话：
#      "the WDT is intentionally NOT stopped at handoff … It keeps guarding the
#      ATF/OP-TEE/U-Boot phase"）；
#   ③ 停狗的动作在 **U-Boot 的 boot.scr** 里（带 0xABC 钥匙写 ZMR，WDEN=0）；
#   ④ 所以「FSBL 交接」到「U-Boot 跑到 boot.scr」之间有一条**硬的 100 秒线**；
#   ⑤ 超了就整机复位，而下一次的 FSBL 一看是 WDT 引起的复位就 **multiboot++**
#      （make_wdt_patch.py 第 4 行原话："fall back (multiboot++) on any
#      WDT-induced reset"，实现在 xfsbl_initialization.c 那段补丁里）。
#
# `LOG_LEVEL=50`（VERBOSE）会让 BL31 把整张页表逐条打到 115200 的串口 ——
# 实测 2888 行。**它把 ATF 那一段推到了 100 秒线附近**，于是：
#   · 有时候赶在 100 秒内跑完 → 槽 6 起得来（"上次能启动"）
#   · 有时候没赶上 → SWDT 复位 → multiboot 6→7 → 落到槽 7（"这次落到 7"）
# 那个"不稳定"不是玄学，是**在跟一条 100 秒的死线赛跑**。
#
# 板上量到的数：槽 6 放已知能启动的镜像 → 20 秒回来、MULTI_BOOT 停在 6；
# 同一个槽放 LOG_LEVEL=50 的重建镜像 → **138 秒**才回来、MULTI_BOOT 变成 7
# （≈ 100 秒看门狗 + 一次落到槽 7 的正常启动）。而 BootROM 自己
# `CSU_BR_ERROR = 0` —— **BootROM 根本没报错，它压根不是 BootROM 拒的**。
# 两份镜像的启动头与六个分区头**逐字节相同**，差异全部落在 BL31 那个分区内部，
# 这也印证了「不是镜像结构问题」。
#
# ⚠️ 所以：**这份脚本里 LOG_LEVEL 不要再调回 50。** 真要看 VERBOSE，
#    得先把 FSBL 的 100 秒放宽或让 U-Boot 早点停狗，否则就是在自找 multiboot++。
#
# 镜像结构也照 boot_fix_quiet.bif，只把 PL bitstream 换成验证过的
# 「厂家设计 + 风扇」那份（金丝雀在密码 bitstream 里，运行时再载 ——
# SiP 是按需调用的，调用那一刻 PL 里是什么就读什么）。
#
# ============================================================================
# 【为什么敢刷：掉电自动回黄金镜像】
# ============================================================================
# 产物先写进一个**非 golden 槽**（本轮用槽 6 = BOOT0006.BIN），靠 CSU_MULTI_BOOT
# 指过去。而 multiboot **由 POR 清零** —— 也就是说这份镜像只在一次热重启内
# 有效，**任何一次断电都自动回到 BOOT.BIN**。这是能拿到的最好兜底：
# 最坏情况是"要断一次电"，而不是"砖"。
# 本脚本**只落盘，不刷卡** —— 传到板子、指 multiboot、提升成 BOOT.BIN
# 这三步都由人另外做。golden BOOT0002.BIN 一个字节都不动。
#
# ⚠️ 做非 golden 槽实验之前，**先把下一个槽换成已知能启动的镜像当安全网**：
#    槽 N 失败时 FSBL 会自增到 N+1，安全网在那里板子就不会失联。做完记得还原。
set -e
source /tools/Xilinx/Vitis/2020.1/settings64.sh
export PATH=/tools/Xilinx/Vitis/2020.1/gnu/aarch64/lin/aarch64-none/bin:$PATH

ATF=/home/build/wdt_patch/atf
# ⚠️ **新的 BUILD_BASE 和新的输出名，不覆盖 atf_secmmio / BOOT_SECMMIO.BIN。**
# 上一轮就是就地重建，把当前 BOOT.BIN 所用的那份 bl31.elf 覆盖掉且没有备份，
# 到现在也没找回来。一次教训够了：产物换名字比事后找回便宜得多。
OUT=/home/build/wdt_patch/atf_secmmio_xmpu
IMG=/home/build/wdt_patch/images
BIN=BOOT_SECMMIO_XMPU.BIN

echo "======== 编 BL31（LOG_LEVEL=20，其余与 atf_build_v50 一致）"
cd "$ATF"
make CROSS_COMPILE=aarch64-none-elf- PLAT=zynqmp SPD=opteed DEBUG=1 \
     LOG_LEVEL=20 BUILD_BASE="$OUT" bl31 -j4 2>&1 | tail -8

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

echo "======== 打 ${BIN}（放非 golden 槽，当前用槽 6）"
cd "$IMG"
cat > boot_secmmio_xmpu.bif <<BIF
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
bootgen -image boot_secmmio_xmpu.bif -arch zynqmp -w -o "$BIN" 2>&1 | grep -E "ERROR|WARNING" || true
ls -l "$BIN" BOOT_OPTEE_FIXQ.BIN
md5sum "$BIN"
echo "DONE（只落盘，没刷卡）"
