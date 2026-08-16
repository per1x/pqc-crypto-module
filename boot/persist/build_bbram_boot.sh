#!/bin/sh
# build_bbram_boot.sh —— 生成 **用 BBRAM 红钥加密的 BOOT.BIN**（只落盘，不烧板）
#
#   sh build_bbram_boot.sh
#   KEY=... OUT=... sh build_bbram_boot.sh
#
# ============================================================================
# 【这件事到底给了什么，没给什么 —— 先看这段再决定要不要用】
# ============================================================================
# 它给的是**机密性**：BOOT.BIN 里的 FSBL / PMUFW / 位流 / BL31 / OP-TEE / U-Boot
# 全部以 AES-256-GCM 密文形式躺在 SD 卡上。拔卡去读，拿不到位流，也拿不到
# BL31 里那张 SiP 白名单的具体内容。
#
# ⚠️ **它不是信任根，别当信任根用。** 关键在于：我们**不烧 eFUSE**（用户永久
#    排除了）。于是 `ENC_ONLY` 这一位是 0，BootROM 的判据只有**启动头本身**：
#
#      · 启动头说"我是加密的" → BootROM 用 BBRAM 里的钥匙解密；
#      · 启动头说"我是明文的" → BootROM **照样直接跑**。
#
#    换句话说，任何人只要把 SD 卡上的 BOOT.BIN 换成一份**自己做的明文镜像**，
#    板子就会老老实实地启动它。加密**挡不住替换**，只挡得住"读出来"。
#    真正的防替换要 `RSA_EN` + PPK hash 烧进 eFUSE —— 那是永久操作，排除了。
#    docs/SECURITY.md 里这句必须原样保留，别在对外材料里升级成"安全启动"。
#
# ⚠️ **BBRAM 是电池供电的 RAM，不是熔丝。** VCC_BATT 一断，钥匙就没了。
#    这块 AXU3EG 上 VCC_BATT 是否真的接了电池座**没有验证过**（要断电一次
#    再回读 CRC 才知道）。所以：
#
#      → **golden 槽（BOOT.BIN，slot 0）永远是明文的，本脚本拒绝覆盖它。**
#        加密镜像只进非 golden 槽。钥匙丢了，断电一次回到 golden，板子还活着。
#
# ============================================================================
# 【钥匙放哪 —— 这不是解决了密钥管理，是把问题挪了个地方】
# ============================================================================
# $KEY 指向的 .nky 里是明文的 AES-256 密钥。它现在躺在构建机的磁盘上
# （chmod 600 的一个文件）。谁能读构建机，谁就能解密 BOOT.BIN。
# 真做产品要么进 HSM 要么进 KMS —— 这里没有做，如实记在案。
set -eu

WS=${WS:-/home/build/bbram_ws}
KEY=${KEY:-$WS/bbram_boot.nky}
KEYHEX=${KEYHEX:-$WS/bbram_aes.key}
IMG=${IMG:-/home/build/petalinux/images/linux}
BL31=${BL31:-/home/build/wdt_patch/atf_secmmio/zynqmp/debug/bl31/bl31.elf}
TEE=${TEE:-/home/build/wdt_patch/images/tee_load_quiet.elf}
SRCTREE=${SRCTREE:-/home/build/pqc-hsm-fpga}
# ⚠️ 这里**故意**用厂家那份静音位流，而不是密码位流：本镜像是拿来验"加密启动
#    这条路通不通"的，**和 golden 的唯一区别应该只有'加不加密'**。换成密码位流
#    就同时动了两个变量（加密 + PL 内容），一旦起不来分不清是谁的锅。
#    演示形态本来就是开机后由 fpgautil 在运行时装密码位流的，不靠这里。
BIT=${BIT:-/home/build/wdt_patch/images/pl_fanquiet.bit}
DTB=${DTB:-$IMG/system.dtb}
OUT=${OUT:-$WS/BOOT_BBRAM.BIN}
BIF=${BIF:-$WS/bbram_boot.bif}
# ENC=0 生成**明文孪生镜像**：组件、顺序、bif 结构与加密版逐字相同，唯一差别
# 就是那几个 encryption 属性。它不是备用产物，是**对照组** —— 加密镜像起不来时，
# 先把它塞进同一个槽、走同一条重启路径。它也起不来，说明锅在"warm reboot 进
# 非 0 槽"，不在加密；它起得来，才轮到怀疑解密。
ENC=${ENC:-1}
if [ "$ENC" = "0" ]; then
    OUT=${OUT_PLAIN:-$WS/BOOT_BBRAM_PLAIN.BIN}
    BIF=$WS/bbram_boot_plain.bif
fi

case "$(basename "$OUT")" in
    BOOT.BIN)
        echo "拒绝：$OUT 会覆盖 golden 槽。加密镜像只能进非 golden 槽 —— 见文件头。"
        exit 1 ;;
esac

# ---- 密钥文件（.nky）---------------------------------------------------------
# ZynqMP 的 .nky 只需要 Key 0（AES-256，64 个十六进制字符）和 IV 0（96 位，
# 24 个字符）。IV 由本脚本随机生成并**连同密钥一起留在 $KEY 里** —— 重建同一份
# 镜像必须用同一个 IV，丢了就只能重烧 BBRAM。
if [ ! -f "$KEY" ]; then
    [ -f "$KEYHEX" ] || { echo "没有 $KEYHEX —— 先跑 bbram_build.sh 生成密钥"; exit 1; }
    K=$(cat "$KEYHEX")
    IV=$(openssl rand -hex 12)
    cat > "$KEY" <<NKY
Device       zu3eg;

Key 0        $K;
IV  0        $IV;
NKY
    chmod 600 "$KEY"
    echo "生成 $KEY（IV=$IV）"
fi

# PMUFW 不写 encryption：bootgen 明确拒绝这个属性组合（"pmufw will be encrypted
# if encryption is enabled for bootloader"）—— 它跟着 bootloader 一起被加密。
if [ "$ENC" = "0" ]; then
cat > "$BIF" <<BIFEOF
the_ROM_image:
{
	[bootloader, destination_cpu=a53-0] $IMG/zynqmp_fsbl.elf
	[pmufw_image] $IMG/pmufw.elf
	[destination_device=pl] $BIT
	[destination_cpu=a53-0, exception_level=el-3, trustzone] $BL31
	[destination_cpu=a53-0, exception_level=el-1, trustzone] $TEE
	[destination_cpu=a53-0, load=0x00100000] $DTB
	[destination_cpu=a53-0, exception_level=el-2] $IMG/u-boot.elf
}
BIFEOF
else
cat > "$BIF" <<BIFEOF
the_ROM_image:
{
	[keysrc_encryption] bbram_red_key
	[bootloader, destination_cpu=a53-0, encryption=aes, aeskeyfile=$KEY] $IMG/zynqmp_fsbl.elf
	[pmufw_image] $IMG/pmufw.elf
	[destination_device=pl, encryption=aes] $BIT
	[destination_cpu=a53-0, exception_level=el-3, trustzone, encryption=aes] $BL31
	[destination_cpu=a53-0, exception_level=el-1, trustzone, encryption=aes] $TEE
	[destination_cpu=a53-0, load=0x00100000] $DTB
	[destination_cpu=a53-0, exception_level=el-2, encryption=aes] $IMG/u-boot.elf
}
BIFEOF
fi

echo "=== 组件 ==="
grep -oE "/home/[^ ]*\.(elf|bit|dtb)" "$BIF" | while read -r f; do
    if [ -f "$f" ]; then echo "  OK  $f"; else echo "  缺  $f"; exit 1; fi
done

. /tools/Xilinx/Vitis/2020.1/settings64.sh >/dev/null 2>&1 || true
bootgen -image "$BIF" -arch zynqmp -w -o "$OUT" 2>&1 | grep -E "ERROR|WARNING" || true
[ -f "$OUT" ] || { echo "bootgen 没出产物"; exit 1; }
ls -l "$OUT"

# ---- 自检：镜像**必须**真的是加密的 ------------------------------------------
# bootgen 对拼错的属性经常只是忽略，不报错。所以别信它的退出码，去数据里找证据：
# 明文 FSBL 的头几百 KB 里必有 ELF 段里的可打印串（"Xilinx"、"zynqmp_fsbl"）；
# 加密之后一个都不该有。
if [ "$ENC" = "0" ]; then
    echo "ENC=0：明文孪生镜像 $OUT（对照组，不做加密自检）"
    exit 0
fi
if strings -n 6 "$OUT" | head -4000 | grep -qiE "zynqmp_fsbl|xilinx"; then
    echo "!!! 自检不过：$OUT 里还能读到明文串 —— 它没有被真正加密"
    exit 1
fi
echo "自检通过：镜像前段读不到明文串"
# ---- 自检二：启动头里的**密钥来源位**必须是 BBRAM ----------------------------
# ⚠️ 这两个常数长得几乎一样，极容易记反（本项目就记反过一次）：
#       0x3A5C3C5A = BBRAM 红钥      ← 我们要的
#       0xA5C3C5A3 = eFUSE 红钥      ← 烧熔丝才用，本项目**永久排除**
# 判断依据不是文档记忆，是对照编译出来的：同一份 bif 只改 [keysrc_encryption]，
# bbram_red_key 出 0x3A5C3C5A，efuse_red_key 出 0xA5C3C5A3。
KS=$(python3 -c "import struct,sys;print('%08x'%struct.unpack('<I',open(sys.argv[1],'rb').read(0x2c)[0x28:0x2c])[0])" "$OUT")
if [ "$KS" != "3a5c3c5a" ]; then
    echo "!!! 启动头密钥来源位是 0x$KS，不是 BBRAM 的 0x3a5c3c5a —— 不要用这份镜像"
    exit 1
fi
echo "自检通过：启动头密钥来源位 = 0x3a5c3c5a（BBRAM 红钥）"
