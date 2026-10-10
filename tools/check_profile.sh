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

# ============================================================================
# PS-07：PRODUCTION 形态**拒绝** SLOT_POLICY_SEED_STORAGE
# ============================================================================
# 为什么这条是**源码结构检查**而不是运行时用例：那道闸门只在 PRODUCTION 形态
# 下生效，而整个 ctest 跑的是 DEV 构建 —— 要真跑到它，得把整个库再按
# PRODUCTION 编一遍并链一个专门的测试目标。那条路值得做，但它属于
# "PRODUCTION 形态的完整回归"，不是这一条检查该背的。
#
# **所以这里如实降低断言强度**：只证明那道闸门还在源码里、且形状没被改坏
# （既查 profile 判定，也查策略位）。它挡不住"闸门写错了"，只挡得住
# "闸门被删了 / 被改成只查一半"。带空对照，免得检查本身悄悄失效。
SLOT_C="$ROOT/src/slot/slot.c"
gate_ok=1
grep -q 'pqc_profile_is_production()' "$SLOT_C" || gate_ok=0
grep -q 'SLOT_POLICY_SEED_STORAGE' "$SLOT_C"    || gate_ok=0
grep -q 'policy_denied_by_profile' "$SLOT_C"    || gate_ok=0
# 闸门必须真的被 create_object 用上 —— 只定义不调用是最容易漏的一种失效
grep -q 'if (policy_denied_by_profile(policy))' "$SLOT_C" || gate_ok=0
if [ "$gate_ok" = 1 ]; then
	echo "  ✓ PS-07 闸门在位（PRODUCTION 下拒绝 SEED_STORAGE）"
else
	echo "  ✗ PS-07 闸门不见了或形状变了 —— PRODUCTION 形态下种子会落盘进 keystore"
	fail=1
fi
# 空对照：同一套 grep 对着一个**没有**闸门的样本必须报失败
CTRL_SRC="$TMP/nogate.c"
cat > "$CTRL_SRC" <<'EOF'
/* 空对照样本：故意不含闸门 */
static int create_object(unsigned policy) { return (int)policy; }
EOF
ctrl_ok=1
grep -q 'policy_denied_by_profile' "$CTRL_SRC" && ctrl_ok=0
if [ "$ctrl_ok" = 1 ]; then
	echo "  ✓ 空对照成立（同一套判据在没有闸门的样本上会报失败）"
else
	echo "  ✗ 空对照失败 —— 这条检查在任何源码上都会通过，等于没有"
	fail=1
fi

[ "$fail" = 0 ] || exit 1
echo "check_profile: 通过"
