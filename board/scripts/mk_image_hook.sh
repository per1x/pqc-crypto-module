#!/usr/bin/env bash
# mk_image_hook.sh —— 往 image.ub 里塞一个开机钩子（不重跑 petalinux）
#
#   在构建机上跑：bash mk_image_hook.sh <源 image.ub> <输出 image.ub>
#
# ============================================================================
# 【为什么不用 petalinux 重建】
# ============================================================================
# 要加进去的只有三个东西：一个 20 行的 init 脚本 + 两条 rc?.d 软链。
# 为这个跑一遍 petalinux-build 要几十分钟，而且那条路本身有坑
# （rm_work 会造出「空 ramdisk 但全程报成功」的 image.ub，
#  见 pqc-hsm 仓库 boot/zynqmp/README.zh-CN.md）。
#
# 拆包改包这条路每一步都能校验，跑完还能把产物反向拆开逐项核对 ——
# 而"能验证"正是上面那个坑真正缺的东西。
#
# ============================================================================
# 【钩子里什么策略都不放】
# ============================================================================
# 烤进 initramfs 的东西改一次要重出镜像，所以 /etc/init.d/hsm-boot 只做一件事：
# 「SD 上那个脚本在就跑它」。真正的开机逻辑在
# /media/sd-mmcblk1p2/hsm/hsm-boot.sh —— 普通文件，随便改，删了就回退。
#
# ============================================================================
# 【⚠️ 公钥：漏一把就把自己关在门外】
# ============================================================================
# initramfs 里的 /home/root/.ssh/authorized_keys 是**唯一**能进板子的钥匙串
# （rootfs 每次启动重新展开，运行时加的一重启就没）。
#
# 原来烤进去的只有 jinling-mac 和 webtop-tunnel 两把，**没有构建机那把** ——
# 而构建机是够到板子的唯一跳板（Mac 直连 ARP 是坏的）。这个脚本因此强制
# 把调用者指定的公钥并进去，并在最后把钥匙串逐条打印出来核对。
#
# 同样重要：**image_backup.ub 必须和 image.ub 带一样的钥匙。**
# boot.scr 的三振兜底会退到它，退过去发现没钥匙一样是失联。
set -euo pipefail

SRC="${1:?用法: mk_image_hook.sh <源 image.ub> <输出 image.ub> [额外公钥文件...]}"
OUT="${2:?缺输出路径}"
shift 2
EXTRA_KEYS=("$@")

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

echo "== 拆 FIT =="
dumpimage -T flat_dt -p 0 "$SRC" -o kernel.gz   >/dev/null
dumpimage -T flat_dt -p 1 "$SRC" -o system.dtb  >/dev/null
dumpimage -T flat_dt -p 2 "$SRC" -o ramdisk.gz  >/dev/null
ls -l kernel.gz system.dtb ramdisk.gz

echo "== 解 initramfs =="
mkdir rootfs && (cd rootfs && gzip -dc ../ramdisk.gz | cpio -idm --quiet)
N0=$(find rootfs | wc -l)
echo "原始条目 $N0"

echo "== 装钩子 =="
cat > rootfs/etc/init.d/hsm-boot <<'HOOK'
#!/bin/sh
# hsm-boot —— 通用钩子：把开机逻辑交给 SD 卡上的脚本
#
# 这个文件烤在 initramfs 里，改它要重出 image.ub —— 正因如此它必须
# **什么策略都不含**：只负责"SD 上那个脚本在就跑它"。真正的逻辑在
# /media/sd-mmcblk1p2/hsm/hsm-boot.sh，普通文件，随便改、删了就回退。
#
# 后台跑：它要等 PHY 协商（最多 20 秒），不该把 boot 卡在这里。
### BEGIN INIT INFO
# Provides:          hsm-boot
# Required-Start:    $network $remote_fs
# Default-Start:     3 5
### END INIT INFO
S=/media/sd-mmcblk1p2/hsm/hsm-boot.sh
case "$1" in
  start) [ -x "$S" ] && setsid "$S" >/dev/null 2>&1 </dev/null & ;;
esac
exit 0
HOOK
chmod 755 rootfs/etc/init.d/hsm-boot
# S40：在 networking(S01) 与 dropbear(S10) 之后 —— dropbear 先起来，
# 万一这个脚本出问题，至少 SSH 已经在听了。
ln -sf ../init.d/hsm-boot rootfs/etc/rc5.d/S40hsm-boot
ln -sf ../init.d/hsm-boot rootfs/etc/rc3.d/S40hsm-boot

if [ ${#EXTRA_KEYS[@]} -gt 0 ]; then
    echo "== 并入额外公钥 =="
    for k in "${EXTRA_KEYS[@]}"; do cat "$k" >> rootfs/home/root/.ssh/authorized_keys; done
    # 去重，免得重复跑这个脚本时越攒越多
    sort -u rootfs/home/root/.ssh/authorized_keys -o rootfs/home/root/.ssh/authorized_keys
    chmod 600 rootfs/home/root/.ssh/authorized_keys
fi

echo "== 重打包 =="
(cd rootfs && find . | cpio -o -H newc --quiet | gzip -9) > ramdisk_new.gz

cat > fit.its <<'ITS'
/dts-v1/;
/ {
	description = "U-Boot fitImage for PetaLinux/5.4+gitAUTOINC+22b71b4162/zynqmp-generic";
	#address-cells = <1>;
	images {
		kernel@1 {
			description = "Linux kernel";
			data = /incbin/("kernel.gz");
			type = "kernel"; arch = "arm64"; os = "linux";
			compression = "gzip";
			load = <0x00080000>; entry = <0x00080000>;
			hash@1 { algo = "sha256"; };
		};
		fdt@system-top.dtb {
			description = "Flattened Device Tree blob";
			data = /incbin/("system.dtb");
			type = "flat_dt"; arch = "arm64"; compression = "none";
			hash@1 { algo = "sha256"; };
		};
		ramdisk@1 {
			description = "petalinux-image-minimal";
			data = /incbin/("ramdisk_new.gz");
			type = "ramdisk"; arch = "arm64"; os = "linux";
			compression = "none";
			hash@1 { algo = "sha256"; };
		};
	};
	configurations {
		default = "conf@system-top.dtb";
		conf@system-top.dtb {
			description = "1 Linux kernel, FDT blob, ramdisk";
			kernel = "kernel@1"; fdt = "fdt@system-top.dtb"; ramdisk = "ramdisk@1";
			hash@1 { algo = "sha256"; };
		};
	};
};
ITS
mkimage -f fit.its out.ub >/dev/null

# ============================================================================
# 反向校验：把刚做出来的镜像**重新拆开**，逐项核对。
# 不做这一步的话，"mkimage 没报错"是唯一的证据 —— 而那条 rm_work 的坑
# 恰恰证明了"工具报成功"什么都不说明。
# ============================================================================
echo "== 反向校验 =="
dumpimage -T flat_dt -p 2 out.ub -o rd_chk.gz >/dev/null
mkdir chk && (cd chk && gzip -dc ../rd_chk.gz | cpio -idm --quiet)
N1=$(find chk | wc -l)
[ "$N1" -ge "$N0" ] || { echo "错误：新包条目 $N1 少于原始 $N0"; exit 1; }
[ -x chk/etc/init.d/hsm-boot ] || { echo "错误：钩子不在新包里"; exit 1; }
[ -L chk/etc/rc5.d/S40hsm-boot ] || { echo "错误：rc5.d 软链不在"; exit 1; }
for f in usr/bin/fpgautil sbin/insmod usr/sbin/tee-supplicant; do
    [ -e "chk/$f" ] || { echo "错误：关键运行时 $f 丢了"; exit 1; }
done
echo "条目 $N0 → $N1，钩子在，关键运行时在"
echo "钥匙串："
while read -r l; do
    [ -n "$l" ] || continue
    echo "$l" > k.pub; ssh-keygen -lf k.pub 2>/dev/null || echo "  ?? 无法解析的一行"
done < chk/home/root/.ssh/authorized_keys

cp out.ub "$OUT"
echo "== 产物：$OUT  md5=$(md5sum "$OUT" | cut -d' ' -f1) =="
echo "⚠️ 别忘了 image_backup.ub 也要换成同一份，否则三振退过去就失联。"
