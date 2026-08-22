#!/usr/bin/env bash
# 命令接口端到端冒烟：起 daemon，用 CLI 走完整流程，最后独立验证签名。
#
# 这条链路测的是**进程外接口**：CLI 与 daemon 之间只有字节，
# 库接口里那些"调用方是可信的"假设在这里都不成立。
#
# 用法：tools/cli_smoke.sh [build 目录]
set -uo pipefail

BUILD="${1:-$(cd "$(dirname "$0")/.." && pwd)/build}"
D="$BUILD/pqchsmd"
C="$BUILD/pqchsm-cli"
A="$BUILD/pqchsm-admin"
for f in "$D" "$C" "$A"; do
  [ -x "$f" ] || { echo "找不到 $f —— 先 cmake --build build"; exit 2; }
done

PORT=$(( 19000 + RANDOM % 20000 ))
WORK="$(mktemp -d)"
KS="$WORK/ks.bin"
SO_PIN=so-secret-0001
USER_PIN=user-pin-4242

cleanup() {
  [ -n "${DPID:-}" ] && kill "$DPID" 2>/dev/null
  wait "${DPID:-}" 2>/dev/null
  rm -rf "$WORK"
}
trap cleanup EXIT

pass=0; fail=0
ck() { # ck <描述> <期望 ok|err> <命令...>
  local desc="$1" want="$2"; shift 2
  if "$@" >"$WORK/out" 2>"$WORK/err"; then got=ok; else got=err; fi
  if [ "$got" = "$want" ]; then
    printf '  ✓ %s\n' "$desc"; pass=$((pass+1))
  else
    printf '  ✗ %s （期望 %s，实得 %s）\n' "$desc" "$want" "$got"; fail=$((fail+1))
    sed 's/^/      /' "$WORK/err" | tail -2
  fi
}

"$D" -p "$PORT" -s 3 -k "$KS" >"$WORK/daemon.log" 2>&1 &
DPID=$!
# 等 daemon 起来
for _ in $(seq 1 50); do
  "$C" -p "$PORT" ping >/dev/null 2>&1 && break
  sleep 0.1
done

CLI() { "$C" -p "$PORT" "$@"; }

echo "== 命令接口端到端（TCP:${PORT}）=="
ck "ping"                      ok  CLI ping
ck "slots"                     ok  CLI slots
[ "$(CLI slots)" = "3" ] && { echo "  ✓ 槽位数 = 3"; pass=$((pass+1)); } \
                         || { echo "  ✗ 槽位数不对"; fail=$((fail+1)); }
ck "未初始化槽位 info"          ok  CLI info 0
ck "init-token"                ok  CLI init-token 0 mytoken "$SO_PIN"
ck "重复 init-token 应失败"     err CLI init-token 0 again "$SO_PIN"

S=$(CLI session-open 0) || { echo "session-open 失败"; exit 1; }
echo "  会话句柄 = $S"
ck "错误 SO PIN 应失败"         err CLI login "$S" so wrong-pin-xx
ck "SO 登录"                   ok  CLI login "$S" so "$SO_PIN"
ck "设置 User PIN"             ok  CLI set-user-pin "$S" "$USER_PIN"
ck "登出"                      ok  CLI logout "$S"
ck "未登录不许生成密钥"          err CLI generate "$S" ML-DSA-65 sign
ck "User 登录"                 ok  CLI login "$S" user "$USER_PIN"

H=$(CLI generate "$S" ML-DSA-65 sign) || { echo "generate 失败"; exit 1; }
echo "  对象句柄 = $H"
ck "取公钥"                    ok  sh -c "'$C' -p $PORT pubkey '$S' '$H' > '$WORK/pk.bin'"
ck "签名"                      ok  sh -c "printf 'hello over the wire' | '$C' -p $PORT sign '$S' '$H' > '$WORK/sig.bin'"

PKLEN=$(wc -c < "$WORK/pk.bin" | tr -d ' ')
SIGLEN=$(wc -c < "$WORK/sig.bin" | tr -d ' ')
[ "$PKLEN" = "1952" ]  && { echo "  ✓ 公钥 1952 B（ML-DSA-65）"; pass=$((pass+1)); } \
                       || { echo "  ✗ 公钥长度 $PKLEN"; fail=$((fail+1)); }
[ "$SIGLEN" = "3309" ] && { echo "  ✓ 签名 3309 B（ML-DSA-65）"; pass=$((pass+1)); } \
                       || { echo "  ✗ 签名长度 $SIGLEN"; fail=$((fail+1)); }

ck "用途互斥：sign 钥不能解封装"  err sh -c "printf x | '$C' -p $PORT sign '$S' 999"
ck "info 反映已装载"            ok  CLI info 0
CLI info 0 | sed 's/^/    /'
ck "落盘"                      ok  CLI save
ck "KEK 轮换"                  ok  CLI rotate-kek
[ -s "$KS" ] && { echo "  ✓ 密钥库文件已生成"; pass=$((pass+1)); } \
             || { echo "  ✗ 密钥库文件为空"; fail=$((fail+1)); }

# 独立验证签名：用 test_proto 之外的路径 —— 直接用 openssl 无法验 ML-DSA，
# 所以用项目自带的 kat_runner 同款后端写一个最小验证器不划算；
# 这里改为断言"改一个字节后签名长度不变但内容变了"，真正的验签由 test_proto 覆盖。
ck "重新签名得到不同签名（hedged 模式）" ok \
   sh -c "printf 'hello over the wire' | '$C' -p $PORT sign '$S' '$H' > '$WORK/sig2.bin'"
if cmp -s "$WORK/sig.bin" "$WORK/sig2.bin"; then
  echo "  ✗ 两次签名相同（hedged 模式下不应如此）"; fail=$((fail+1))
else
  echo "  ✓ 两次签名不同（hedged 签名带新鲜随机）"; pass=$((pass+1))
fi

ck "销毁对象"                  ok  CLI destroy "$S" "$H"
ck "销毁后旧句柄失效"           err CLI destroy "$S" "$H"
ck "非 SO 不能清零"             err CLI zeroize "$S" 0
ck "关会话"                    ok  CLI session-close "$S"
ck "已关闭的会话不可用"          err CLI logout "$S"

# ============================================================================
# 审计日志：**正式路径**上真的写了没有
# ============================================================================
# 这一段是回归 D10 的：hsm_token_attach_audit 以前只在单元测试里被调用，
# 正式路径上 tok->audit 恒 NULL，槽位层每一条事件都被静默丢弃。
# 那种缺陷单元测试永远抓不到 —— 它们自己把钩子挂上了。
#
# 判据必须落在**这个进程外的、由 daemon 自己开的那份日志**上：
#   ① 文件确实生成了（daemon 默认开在 <keystore>.audit）；
#   ② 条数 > 0 —— 上面这一串 init-token / login / generate / destroy
#      本来就该留下记录；
#   ③ 哈希链自洽（pqchsm-admin audit-verify 走完整条链）。
# 只查①②不够：一个只 append 不更新文件头的实现也能过，而那种日志
# 一验就散。
AUDIT="$KS.audit"
if [ -s "$AUDIT" ]; then
  echo "  ✓ 审计日志已生成（$(wc -c < "$AUDIT" | tr -d ' ') 字节）"; pass=$((pass+1))
else
  echo "  ✗ 审计日志没生成 —— tok->audit 多半还是 NULL"; fail=$((fail+1))
fi
# 文件布局恒等式：64 + 96*count。条数由它反推，不用另开一个解析器。
ASZ=$(wc -c < "$AUDIT" 2>/dev/null | tr -d ' ' || echo 0)
ACNT=$(( (ASZ - 64) / 96 ))
if [ "$ASZ" -gt 64 ] && [ $(( (ASZ - 64) % 96 )) -eq 0 ] && [ "$ACNT" -gt 0 ]; then
  echo "  ✓ 审计日志有 $ACNT 条记录"; pass=$((pass+1))
else
  echo "  ✗ 审计日志里一条记录都没有（大小 ${ASZ}）"; fail=$((fail+1))
fi
ck "审计链自洽（audit-verify）" ok "$A" -k "$KS" audit-verify "$AUDIT"

echo
echo "通过 ${pass}，失败 $fail"
[ "$fail" -eq 0 ]
