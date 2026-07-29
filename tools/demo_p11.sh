#!/usr/bin/env bash
# 跑 Python 与 Java 的 PKCS#11 provider demo（缺环境时自动 SKIP）
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODULE="${1:-$ROOT/build/pqchsm-pkcs11.dylib}"

# ASan 编出来的 .dylib 没法被普通进程 dlopen：macOS 会以
# "Sanitizer load violates platform policy" 拒绝加载 asan 运行时。
# 这不是本项目的问题，也修不了 —— 如实 SKIP，别让它伪装成失败。
# （C 侧的 test_p11 是**整个进程**都带 ASan 编的，所以它照跑不误。）
if command -v otool >/dev/null 2>&1 && otool -L "$MODULE" 2>/dev/null | grep -q asan; then
  echo "SKIP: $MODULE 带 ASan，外部进程无法 dlopen（macOS 平台策略）"
  exit 0
fi

[ -f "$MODULE" ] || { echo "SKIP: 没有 $MODULE"; exit 0; }

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
fail=0

echo "== Python (PyKCS11) =="
PY="$ROOT/.venv-p11/bin/python"
if [ -x "$PY" ] && "$PY" -c "import PyKCS11" 2>/dev/null; then
  PQCHSM_KEYSTORE="$WORK/py.ks" PQCHSM_SLOTS=3 "$PY" "$ROOT/demo/python/pqchsm_demo.py" "$MODULE" \
    | tail -3
  [ "${PIPESTATUS[0]}" -eq 0 ] || fail=1
else
  echo "  SKIP: 没有 PyKCS11 环境（python3 -m venv .venv-p11 && .venv-p11/bin/pip install PyKCS11）"
fi

echo
echo "== Java (JDK FFM) =="
JH=""
for c in "$(brew --prefix openjdk 2>/dev/null)/libexec/openjdk.jdk/Contents/Home" "${JAVA_HOME:-}"; do
  [ -n "$c" ] && [ -x "$c/bin/java" ] && JH="$c" && break
done
if [ -n "$JH" ]; then
  V=$("$JH/bin/java" -version 2>&1 | head -1 | grep -oE '"[0-9]+' | tr -d '"')
  if [ "${V:-0}" -ge 22 ]; then
    PQCHSM_KEYSTORE="$WORK/java.ks" PQCHSM_SLOTS=3 \
      "$JH/bin/java" --enable-native-access=ALL-UNNAMED \
      "$ROOT/demo/java/PqcHsmDemo.java" "$MODULE" | tail -3
    [ "${PIPESTATUS[0]}" -eq 0 ] || fail=1
  else
    echo "  SKIP: JDK $V < 22，没有 FFM（java.lang.foreign）"
  fi
else
  echo "  SKIP: 找不到 JDK"
fi

echo
[ "$fail" -eq 0 ] && echo "demo 全部通过" || echo "有 demo 失败"
exit $fail
