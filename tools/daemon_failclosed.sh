#!/usr/bin/env bash
# daemon_failclosed.sh —— 回归 P1④：pqchsmd 的密钥库必须 fail-closed
#
# 老行为：
#     if (ks && hsm_keystore_load(tok, ks) == HSM_OK) { 提示一句 }
#     ... 服务 ...
#     if (ks) hsm_keystore_save(tok, ks);      // ← 退出时无条件覆盖
#
# 于是装载失败（文件损坏 / 换了设备 / 被防回滚判为回放 / 权限不对）之后：
#   · daemon 照常起来，拿一个**空 token** 对外提供服务（fail-open）；
#   · 退出时把那个空 token 写回去，**真正的密钥库就没了**（数据销毁）。
#
# 这个脚本盯三件事：
#   ① 文件不存在 → 允许新建（正常路径不能被这次修复堵死）；
#   ② 文件存在但坏 → 拒绝启动，**且原文件一字节不改**；
#   ③ 文件不可读（权限）→ 同样拒绝启动，不当成"不存在"。
#
# 用法：tools/daemon_failclosed.sh [build 目录]
set -uo pipefail

BUILD="${1:-$(cd "$(dirname "$0")/.." && pwd)/build}"
D="$BUILD/pqchsmd"
C="$BUILD/pqchsm-cli"
for f in "$D" "$C"; do
	[ -x "$f" ] || { echo "找不到 $f —— 先 cmake --build build"; exit 2; }
done

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
KS="$WORK/ks.bin"
pass=0; fail=0
ok()   { printf '  ✓ %s\n' "$1"; pass=$((pass+1)); }
bad()  { printf '  ✗ %s\n' "$1"; fail=$((fail+1)); }

md5of() { if command -v md5 >/dev/null 2>&1; then md5 -q "$1"; else md5sum "$1" | cut -d' ' -f1; fi; }

# ---- ① 不存在 → 新建，并且安全状态变化会当场落盘 -------------------------
PORT=$(( 19000 + RANDOM % 20000 ))
"$D" -p "$PORT" -s 2 -k "$KS" >"$WORK/d1.log" 2>&1 &
DPID=$!
for _ in $(seq 1 60); do "$C" -p "$PORT" ping >/dev/null 2>&1 && break; sleep 0.1; done

if "$C" -p "$PORT" ping >/dev/null 2>&1; then
	ok "密钥库不存在时 daemon 正常启动"
else
	bad "密钥库不存在时 daemon 起不来"
fi
# 触发一次安全状态变化（init-token），它应当当场落盘
"$C" -p "$PORT" init-token 0 tok1 so-secret-0001 >/dev/null 2>&1
if [ -s "$KS" ]; then
	ok "init-token 之后密钥库已经在盘上（没等到退出才写）"
else
	bad "init-token 之后盘上还没有密钥库 —— persist 钩子没生效"
fi
kill "$DPID" 2>/dev/null; wait "$DPID" 2>/dev/null

[ -s "$KS" ] || { bad "密钥库为空，后面的用例没法做"; echo "$pass 通过 / $fail 失败"; exit 1; }
GOOD_MD5=$(md5of "$KS")
cp "$KS" "$WORK/ks.good"

# ---- ② 存在但坏 → 拒绝启动，且不写盘 --------------------------------------
# 打坏文件尾的全文件 MAC。改中间也行，这里挑一个必然被 MAC 抓到的位置。
python3 - "$KS" <<'PY'
import sys
p = sys.argv[1]
b = bytearray(open(p, 'rb').read())
b[-1] ^= 0xFF
open(p, 'wb').write(b)
PY
BROKEN_MD5=$(md5of "$KS")

PORT=$(( 19000 + RANDOM % 20000 ))
"$D" -p "$PORT" -s 2 -k "$KS" >"$WORK/d2.log" 2>&1
RC=$?
if [ "$RC" -ne 0 ]; then
	ok "密钥库损坏时 daemon 拒绝启动（退出码 ${RC}）"
else
	bad "密钥库损坏时 daemon 仍然起来了（fail-open）"
fi
if [ "$(md5of "$KS")" = "$BROKEN_MD5" ]; then
	ok "拒绝启动之后原文件一字节没改"
else
	bad "拒绝启动的路径上把密钥库写了 —— 这正是老实现销毁数据的地方"
fi
if grep -q '拒绝启动' "$WORK/d2.log"; then
	ok "日志说清了为什么拒绝"
else
	bad "日志里没有可操作的原因"
fi

# ---- ③ 不可读（权限）→ 同样拒绝，不当成"不存在" ---------------------------
cp "$WORK/ks.good" "$KS"
chmod 000 "$KS"
PORT=$(( 19000 + RANDOM % 20000 ))
"$D" -p "$PORT" -s 2 -k "$KS" >"$WORK/d3.log" 2>&1
RC=$?
chmod 600 "$KS"
if [ "$RC" -ne 0 ]; then
	ok "密钥库读不了时拒绝启动（退出码 ${RC}）"
else
	# root 跑测试时 chmod 000 挡不住读，这时这一条没有判据 —— 说清楚，不假装通过
	if [ "$(id -u)" = "0" ]; then
		printf '  · 以 root 运行，chmod 000 挡不住读，跳过这一条\n'
	else
		bad "密钥库读不了时 daemon 仍然起来了"
	fi
fi
if [ "$(md5of "$KS")" = "$GOOD_MD5" ]; then
	ok "这条路径上文件同样没被改"
else
	bad "文件被改了"
fi

echo "daemon_failclosed: $pass 通过 / $fail 失败"
[ "$fail" = 0 ]
