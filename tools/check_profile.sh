#!/usr/bin/env bash
# check_profile.sh —— 结构性回归：PRODUCTION 形态的二进制里搜不到桩根密钥。
#
# 【为什么要有这条】
# "固件里搜不到 KDR" 是验收项。靠 #if 写对一次是不够的 —— 有人加一行
# `#include` 或者把常量挪到别处，条件编译就悄悄失效了，而**没有任何编译错误**。
# 所以这里不看源码，直接把 kdr.c 用 PRODUCTION 的宏编出目标文件，
# 在字节里找那段字面量。找到就是回归。
#
# 顺带把 DEV 形态也编一遍并**要求能找到** —— 否则这条检查在"字面量被改名"
# 之后会静默地永远通过（空对照，比没有检查更糟）。
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-cc}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# 字面量 "PQC-HSM STUB KDR -- NOT SECRET!!"，在源码里是逐字节的十六进制，
# 所以要在**目标文件**里按字节找，不能 grep 源码。
NEEDLE='PQC-HSM STUB KDR -- NOT SECRET!!'

build() { # build <profile 值> <输出>
	"$CC" -c -O2 -I"$ROOT/include" -DPQC_PROFILE="$1" \
	      -o "$2" "$ROOT/src/crypto/kdr.c"
}

fail=0

build 0 "$TMP/dev.o"
if LC_ALL=C grep -q -- "$NEEDLE" "$TMP/dev.o"; then
	echo "  ✓ DEV 形态里找得到桩根密钥（空对照成立）"
else
	echo "  ✗ DEV 形态里也找不到桩根密钥 —— 这条检查已经失去意义"
	echo "    多半是字面量被改过；把本脚本的 NEEDLE 一起改掉。"
	fail=1
fi

build 1 "$TMP/prod.o"
if LC_ALL=C grep -q -- "$NEEDLE" "$TMP/prod.o"; then
	echo "  ✗ PRODUCTION 形态的目标文件里**仍然有**桩根密钥"
	fail=1
else
	echo "  ✓ PRODUCTION 形态里搜不到桩根密钥"
fi

[ "$fail" = 0 ] || exit 1
echo "check_profile: 通过"
