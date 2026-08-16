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
# ============================================================================
# 【设备树必须与位流配套 —— 一次 SError panic 换来的】
# ============================================================================
# 第一版直接用了 golden 的 system.dtb，板子起来立刻 panic：
#     xgpio_of_probe → gpiochip_add_data_with_key
#       → el1_error → do_serror → arm64_serror_panic → panic
# 那份设备树描述的是**厂家 PL**（AXI Ethernet、四个 GPIO、两个串口、
# VDMA、MIPI…共十几个节点，全在 0x8000_0000 段）。J1 换成密码位流之后
# 这些外设一个都不存在，驱动一探测就撞上 AXI 防火墙 —— 而写是 posted 的，
# DECERR 以 SError 回来，内核只能 panic。**这正是 docs/REGISTERS.md 首屏
# 那条 hazard 的另一种触发方式**：这次发起访问的不是我们的测试程序，是内核
# 自己的驱动。
#
# 所以 J1 用一份**配套的设备树** j1_nopl.dtb：整个 amba_pl@0 子树连同指向它
# 的别名一起删掉。密码核不需要设备树节点 —— 它们由 /dev/secmmio 经 EL3 访问。
#
# 【后来的补充：这一类 panic 已经在硬件那一层根治了】
# 防火墙与译码器改成了 RAZ/WI（读回 0、写丢弃、不产生总线错误），所以现在
# 即使设备树里留着不存在的 PL 外设，驱动去探测也只会读到 0 然后放弃，
# **不会再有 SError**。
#
# 但 j1_nopl.dtb 仍然要用，理由变了：不是"防止 panic"，而是"别在设备树里
# 描述一堆不存在的东西" —— 那会让 probe 失败的日志刷屏，也会让任何看设备树
# 的人对这块板上到底有什么产生错误印象。根治了崩溃，不等于该留着错误的描述。
#
# ⚠️ **"J1 形态没有网络"这句话是错的，已纠正（2026-08-17）。**
#    eth0（AXI Ethernet）确实在厂家 PL 里、装了密码位流就没了 —— 这半句对。
#    但板上还有 **eth1 = `ff0e0000.ethernet`，PS 里的 Cadence GEM 硬核**，
#    **PL 怎么换都动不了它**。演示形态一直走的就是它（192.168.50.175）。
#    所以 J1 形态**有网络**，hsm-boot.sh 照常把 eth1 配起来。
#    写错这一条的代价是真的：它让 J1 被当成"验不了的形态"搁置了很久。
#
# ⚠️ 仍然成立的是：J1 形态里 PL 由**受控镜像**加载，root 换不掉密码边界 ——
#    那才是 J1 的价值所在。而**日常开发仍然建议用 golden + 运行时装位流**，
#    因为换 J1 镜像要重打整份 BOOT.BIN，改一次位流的成本高得多。
set -e
source /tools/Xilinx/Vitis/2020.1/settings64.sh

# ============================================================================
# 【bif 由本脚本生成，不直接用仓库里那份】
# ============================================================================
# 仓库里的 boot_j1_pl.bif 写的是**绝对路径**，指向 /home/build/pqc-hsm-fpga ——
# 而构建机上同时有好几棵 checkout（pqc-hsm-fpga / pqc-hsm-merged / pqc-hsm-tip…）。
# 于是"这份 BOOT.BIN 里的位流到底来自哪棵树"要靠猜，而猜错的后果是
# **把一份旧位流固化进启动镜像**，还看不出来。
#
# 所以路径全部变量化、bif 现生成；仓库里那份留作**布局的参考**（分区顺序、
# destination 属性），不再被直接使用。要换树只改 SRCTREE。
SRCTREE=${SRCTREE:-/home/build/pqc-hsm-merged}
IMG=${IMG:-/home/build/petalinux/images/linux}
BL31=${BL31:-/home/build/wdt_patch/atf_secmmio/zynqmp/debug/bl31/bl31.elf}
TEE=${TEE:-/home/build/wdt_patch/images/tee_load_quiet.elf}
BIT=${BIT:-$SRCTREE/hardware/syn/impl/zu3eg_hsm.bit}
BIF=/tmp/boot_j1_pl.gen.bif
OUT=${OUT:-/home/build/wdt_patch/images/BOOT_J1_PL.BIN}

# ⚠️ **只认送检形态的位流。** J1 的全部价值就是"密码边界由受控镜像加载"，
#    把一份 SECURE_ONLY=0 的开发位流固化进去，等于固化了一个普通世界全可达的
#    边界 —— 比不做还糟，因为它看起来更安全了。名字是唯一的判据，而
#    impl_bitstream.tcl 里那条断言保证了名字与形态一致。
case "$(basename "$BIT")" in
    zu3eg_hsm.bit) ;;
    *) echo "拒绝：$BIT 不是送检形态位流（必须叫 zu3eg_hsm.bit）"; exit 1 ;;
esac

# ---- 设备树：**就用 petalinux 的 system.dtb，不要另做一份"删掉 PL"的** ----
#
# ⚠️ 这里原来生成一份 j1_nopl.dtb（把 amba_pl@0 子树删掉/禁用），理由是
#    "别在设备树里描述一堆不存在的外设"。**那个做法从两个方向都错了，
#    实测记下来免得有人再走一遍：**
#
#  ① **它够不着内核。** BOOT.BIN 里 load=0x00100000 那份 dtb 是 **U-Boot 自己的
#     control DTB**；**内核的设备树来自 SD 上的 image.ub**（串口日志里那句
#     `Booting using the fdt blob at 0x14000000` 就是证据）。所以改它对
#     "内核别去探测不存在的 PL 外设"**一点作用都没有** —— 实测：用了 nopl 之后
#     `xilinx-video amba_pl@0:vcap_mipi ...` 那串探测失败原样出现。
#     要达到那个目的，得改 image.ub 里的设备树，那是另一件事。
#
#  ② **它还把热重启弄坏了。** 同一份镜像：
#       system.dtb 版  —— sysrq 重启能起来、JTAG 复位也能起来
#       nopl 版        —— JTAG 复位能起来，**sysrq 重启起不来**
#                          （串口上连 FSBL 的 banner 都没有，两次复现）
#     根因没有继续追 —— 因为按 ① 它本来就没有存在的理由。
#
# 结论：J1 用 system.dtb。**少一份自制产物，就少一处能坏的地方。**
DTB_OUT=${DTB_OUT:-/home/build/petalinux/images/linux/system.dtb}

cat > "$BIF" <<BIFEOF
the_ROM_image:
{
	[bootloader, destination_cpu=a53-0] $IMG/zynqmp_fsbl.elf
	[pmufw_image] $IMG/pmufw.elf
	[destination_device=pl] $BIT
	[destination_cpu=a53-0, exception_level=el-3, trustzone] $BL31
	[destination_cpu=a53-0, exception_level=el-1, trustzone] $TEE
	[destination_cpu=a53-0, load=0x00100000] $DTB_OUT
	[destination_cpu=a53-0, exception_level=el-2] $IMG/u-boot.elf
}
BIFEOF
echo "== 生成的 bif =="
cat "$BIF"

echo "== 核对 bif 引用的文件都在 =="
grep -oE "/home/[^ ]*\.(elf|bit|dtb)" $BIF | while read f; do
    [ -f "$f" ] && echo "  OK  $f" || { echo "  缺  $f"; exit 1; }
done

echo "== bootgen =="
bootgen -image $BIF -arch zynqmp -w -o $OUT 2>&1 | grep -E "ERROR|WARNING" || true
ls -l $OUT
md5sum $OUT
echo "DONE（只落盘，没刷卡）"
