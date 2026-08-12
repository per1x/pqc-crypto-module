#!/bin/bash
# make_boot_scr.sh —— 在现有 boot.scr 里插一段「开机装风扇 bitstream」，重新打包
#
#   在构建机上跑：bash board/factory/make_boot_scr.sh <现有boot.scr> <输出boot.scr>
#
# ============================================================================
# 【为什么走 boot.scr，而不是打一个 BOOT.BIN 放非 golden 槽】
# ============================================================================
# 用户要的是「开机就静」。两条路：
#
#  (A) 改 BOOT.BIN，让 FSBL 启动时就装带风扇的 bitstream。
#      **这条路在这块板上既危险又无效：**
#       · 危险：板子没有 JTAG。BOOT.BIN 写坏了没有任何补救手段。
#       · 无效：multiboot 寄存器**由 POR 清零**（掉电即清），BootROM 冷启动
#         永远从偏移 0 也就是 BOOT.BIN 起。放进"非 golden 槽"的镜像
#         **根本轮不到**，除非有人先把 multiboot 写成非零 —— 而那本身要么
#         靠串口手动设，要么靠 FSBL 失败回退。也就是说：非 golden 槽既不会
#         在开机时生效，也就谈不上"先确认能启再依赖"。
#
#  (B) 改 boot.scr，让 U-Boot 在起 Linux 之前装那份 bitstream。
#      · boot.scr 是这块板上**当前真正在起作用**的机制（已经被改过：
#        拆 FSBL 看门狗、开 DT 的 watchdog 节点、三振退备份镜像）。
#      · 它就是 FAT 分区上的一个文件，换回旧的即可回退。
#      · 代价只是风扇从上电到 U-Boot 这几秒钟仍然满速 —— 对"太吵"这个
#        诉求没有任何影响。
#
# 所以走 (B)。golden BOOT.BIN 和所有备份镜像一个字节都不动。
#
# ============================================================================
# 【插进去的这段为什么是安全的】
# ============================================================================
#  · 位置在**拆看门狗那一段之后**：即使我这段整个失败，FSBL 那条 ~100 秒
#    的看门狗也已经被拆掉了，不会因为我而变成启动循环。
#  · 三层 if 嵌套，任何一步（文件不在、fatload 失败、fpga loadb 不支持）
#    都只是打一行 echo 然后继续往下走，PL 保持 FSBL 装的那一份（出厂设计，
#    风扇满速）—— 也就是**退化成现状**，不会退化成起不来。
#  · 装载地址 0x08000000：与内核 0x00200000、image.ub 0x10000000、
#    FDT 0x14000000 都不重叠。
set -euo pipefail

SRC=${1:?用法: make_boot_scr.sh <现有boot.scr> <输出boot.scr>}
OUT=${2:?用法: make_boot_scr.sh <现有boot.scr> <输出boot.scr>}
TXT=$(mktemp)
NEW=$(mktemp)
trap 'rm -f "$TXT" "$NEW"' EXIT

# mkimage 的头是 64 字节，去掉就是脚本正文
dd if="$SRC" bs=64 skip=1 of="$TXT" 2>/dev/null

if grep -q 'fanquiet.bit' "$TXT"; then
    echo "现有 boot.scr 里已经有这一段了，不重复插入"
    cp "$SRC" "$OUT"
    exit 0
fi

# 锚点：拆看门狗那段的结尾。插在它后面。
ANCHOR='^fi;$'
if ! grep -q "$ANCHOR" "$TXT"; then
    echo "错误：在 boot.scr 里找不到插入锚点（拆看门狗那段的 fi;）"
    exit 1
fi

python3 - "$TXT" "$NEW" <<'PY'
import sys
src, dst = sys.argv[1], sys.argv[2]
s = open(src, encoding='utf-8', errors='surrogateescape').read()

block = '''
# ============================================================================
# AXU3EGB：开机就把带温控的 bitstream 装进 PL（风扇不再满速）
# ============================================================================
# 出厂 bitstream 把风扇脚 AA11 钉死在低电平 = 常转满速。这里在起 Linux 之前
# 换成一份「厂家设计 + 风扇温控」的 bitstream —— 网卡/DDR4/MIPI/串口全都在，
# 唯一的差别就是那一个脚改成按结温调速的 PWM。
#
# ⚠️ 每一层都可能失败（文件不在、fatload 读不到、这版 U-Boot 没有 fpga
#    loadb），失败就只打一行 echo 继续往下走，PL 保持 FSBL 装的出厂设计。
#    也就是**退化回现状（风扇满速）**，不会把启动搞死。
#    位置在拆看门狗之后，所以即便这一整段炸了也不会变成启动循环。
#
# 要回退：把 fanquiet.bit 从 FAT 分区删掉即可，这一段自己就跳过了。
if test -e ${devtype} ${devnum}:${distro_bootpart} /fanquiet.bit; then
	echo AXU3EGB: loading fan-control bitstream;
	if fatload ${devtype} ${devnum}:${distro_bootpart} 0x08000000 fanquiet.bit; then
		if fpga loadb 0 0x08000000 ${filesize}; then
			echo AXU3EGB: PL configured, fan now under temperature control;
		else
			echo AXU3EGB: fpga loadb failed, PL left as FSBL loaded it;
		fi;
	else
		echo AXU3EGB: fatload fanquiet.bit failed, skipping;
	fi;
fi;
'''

# 插在"拆看门狗"那段的 fi; 之后 —— 找第一个独占一行的 fi;
lines = s.split('\n')
for i, ln in enumerate(lines):
    if ln.strip() == 'fi;':
        lines.insert(i + 1, block)
        break
else:
    raise SystemExit('找不到锚点 fi;')
open(dst, 'w', encoding='utf-8', errors='surrogateescape').write('\n'.join(lines))
PY

mkimage -A arm64 -T script -C none -n "AXU3EGB boot script (fan)" -d "$NEW" "$OUT"
echo "已生成 $OUT"
echo "插入的那一段："
grep -n -A3 'fanquiet' "$NEW" | head -12
