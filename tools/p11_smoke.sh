#!/usr/bin/env bash
# pkcs11-tool 冒烟 + 与 SoftHSMv2 的行为对比
#
# 为什么要跟 SoftHSMv2 比：本模块自身不报错并不说明行为正确。SoftHSMv2 是
# 事实上的 PKCS#11 参考实现，同一条 pkcs11-tool 命令在两边应当得到**同类**的
# 结果（成功/失败、以及失败时的错误类别）。不同之处必须能解释得出原因。
#
# 已知的、可解释的差异（不是 bug）：
#   1. 机制列表：本模块只有 ML-KEM/ML-DSA，SoftHSM 只有 RSA/EC/AES 等传统算法。
#      OpenSC 0.27 的 CLI 还不认 ML-DSA 密钥类型，所以 --keypairgen 在本模块上
#      走不通 —— PQC 密钥生成由 tests/unit/test_p11.c 直接调 C_GenerateKeyPair 驱动。
#   2. 重复 --init-token：SoftHSM 允许（重新初始化），本模块拒绝（CKR_ACTION_PROHIBITED），
#      因为那等于无声销毁全部密钥，本实现要求先显式 zeroize。
#
# 用法：tools/p11_smoke.sh [模块路径]
set -uo pipefail

MODULE="${1:-$(cd "$(dirname "$0")/.." && pwd)/build/pqchsm-pkcs11.dylib}"

# ASan 编出来的 .dylib 没法被普通进程 dlopen：macOS 会以
# "Sanitizer load violates platform policy" 拒绝加载 asan 运行时。
# 这不是本项目的问题，也修不了 —— 如实 SKIP，别让它伪装成失败。
# （C 侧的 test_p11 是**整个进程**都带 ASan 编的，所以它照跑不误。）
if command -v otool >/dev/null 2>&1 && otool -L "$MODULE" 2>/dev/null | grep -q asan; then
  echo "SKIP: $MODULE 带 ASan，外部进程无法 dlopen（macOS 平台策略）"
  exit 0
fi

SO_PIN=12345678
USER_PIN=1234abcd

if [ ! -f "$MODULE" ]; then
  echo "找不到模块 $MODULE —— 先 cmake --build build --target pqchsm-p11"
  exit 2
fi
if ! command -v pkcs11-tool >/dev/null 2>&1; then
  echo "SKIP: 没装 pkcs11-tool（brew install opensc）"
  exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
export PQCHSM_KEYSTORE="$WORK/ks.bin"
export PQCHSM_SLOTS=2

pass=0; fail=0
ck() { # ck <描述> <期望 ok|err> <命令...>
  local desc="$1" want="$2"; shift 2
  if "$@" >"$WORK/out" 2>&1; then got=ok; else got=err; fi
  if [ "$got" = "$want" ]; then
    printf '  ✓ %s\n' "$desc"; pass=$((pass+1))
  else
    printf '  ✗ %s （期望 %s，实得 %s）\n' "$desc" "$want" "$got"; fail=$((fail+1))
    sed 's/^/      /' "$WORK/out" | tail -3
  fi
}

P11() { pkcs11-tool --module "$MODULE" "$@"; }

echo "== pqc-hsm PKCS#11 模块 =="
ck "C_GetInfo（--show-info）"          ok  P11 --show-info
ck "C_GetSlotList（--list-slots）"     ok  P11 --list-slots
ck "机制列表"                          ok  P11 --list-mechanisms
ck "未初始化时登录应失败"               err P11 --slot 0 --login --pin "$USER_PIN" --list-objects
ck "C_InitToken"                       ok  P11 --slot 0 --init-token --label smoke --so-pin "$SO_PIN"
ck "错误 SO PIN 应失败"                 err P11 --slot 0 --init-pin --so-pin 99999999 --pin "$USER_PIN"
ck "C_InitPIN"                         ok  P11 --slot 0 --init-pin --so-pin "$SO_PIN" --pin "$USER_PIN"
ck "正确 User PIN 登录"                 ok  P11 --slot 0 --login --pin "$USER_PIN" --list-objects
ck "错误 User PIN 应失败"               err P11 --slot 0 --login --pin wrongpin1 --list-objects
ck "重复 InitToken 被拒（有意与 SoftHSM 不同）" err \
                                        P11 --slot 0 --init-token --label again --so-pin "$SO_PIN"

echo "  -- token 标志 --"
P11 --list-slots 2>/dev/null | grep -E "token (label|flags)" | sed 's/^/    /'

echo "  -- 机制（应当只有 ML-KEM / ML-DSA）--"
P11 --list-mechanisms 2>/dev/null | sed -n '2,8p' | sed 's/^/    /'

# ---- SoftHSMv2 对照 ----
echo
if command -v softhsm2-util >/dev/null 2>&1; then
  SOFTHSM_LIB=""
  for c in /opt/homebrew/lib/softhsm/libsofthsm2.so /usr/local/lib/softhsm/libsofthsm2.so \
           /opt/homebrew/lib/softhsm/libsofthsm2.dylib; do
    [ -f "$c" ] && SOFTHSM_LIB="$c" && break
  done
  if [ -z "$SOFTHSM_LIB" ]; then
    echo "== SoftHSMv2 对照：装了 softhsm2-util 但找不到 libsofthsm2，跳过 =="
  else
    mkdir -p "$WORK/softhsm/tokens"
    printf 'directories.tokendir = %s/softhsm/tokens\nobjectstore.backend = file\n' "$WORK" \
      > "$WORK/softhsm2.conf"
    export SOFTHSM2_CONF="$WORK/softhsm2.conf"
    S11() { pkcs11-tool --module "$SOFTHSM_LIB" "$@"; }
    echo "== SoftHSMv2 参考实现（同样的命令序列）=="
    ck "C_GetInfo"                  ok  S11 --show-info
    ck "C_GetSlotList"              ok  S11 --list-slots
    ck "未初始化时登录应失败"        err S11 --slot 0 --login --pin "$USER_PIN" --list-objects
    ck "C_InitToken"                ok  S11 --slot 0 --init-token --label smoke --so-pin "$SO_PIN"
    ck "错误 SO PIN 应失败"          err S11 --token-label smoke --init-pin --so-pin 99999999 --pin "$USER_PIN"
    ck "C_InitPIN"                  ok  S11 --token-label smoke --init-pin --so-pin "$SO_PIN" --pin "$USER_PIN"
    ck "正确 User PIN 登录"          ok  S11 --token-label smoke --login --pin "$USER_PIN" --list-objects
    ck "错误 User PIN 应失败"        err S11 --token-label smoke --login --pin wrongpin1 --list-objects
    echo "  注：SoftHSM 允许重复 InitToken，本模块有意拒绝 —— 见脚本顶部说明"
  fi
else
  echo "== SoftHSMv2 对照：未安装（brew install softhsm），跳过 =="
fi

echo
echo "通过 ${pass}，失败 $fail"
[ "$fail" -eq 0 ]
