#!/usr/bin/env bash
# libFuzzer 版 fuzz
#
# Apple 的 clang **不带 libFuzzer**（缺 libclang_rt.fuzzer_osx.a），
# 所以这里用 brew 的 llvm。没装就 SKIP —— ctest 里跑的是不依赖 libFuzzer 的
# 独立驱动版（fuzz_standalone），CI 不能依赖 brew 装没装。
#
# 用法：tools/fuzz.sh [秒数] [语料目录]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SECS="${1:-60}"
CORPUS="${2:-$ROOT/.fuzz-corpus}"

LLVM="$(brew --prefix llvm 2>/dev/null || true)"
CC="$LLVM/bin/clang"
[ -x "$CC" ] || { echo "SKIP: 没装 brew 的 llvm（brew install llvm）"; exit 0; }

OSSL="$(brew --prefix openssl@3)"
OQS="$(brew --prefix liboqs)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

echo "编译 libFuzzer 靶子…"
"$CC" -g -O1 -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
  -std=c11 -I "$ROOT/include" -I "$ROOT/src" -I "$OSSL/include" -I "$OQS/include" \
  "$ROOT/tests/fuzz/fuzz_targets.c" \
  "$ROOT"/src/crypto/*.c "$ROOT"/src/slot/*.c "$ROOT"/src/store/*.c \
  "$ROOT"/src/backup/*.c "$ROOT"/src/audit/*.c "$ROOT"/src/proto/*.c "$ROOT"/src/util/*.c \
  -L "$OSSL/lib" -lcrypto "$OQS/lib/liboqs.a" \
  -o "$OUT/fuzz" || { echo "编译失败"; exit 1; }

mkdir -p "$CORPUS"
echo "跑 ${SECS}s（语料目录 ${CORPUS}，会累积）…"
"$OUT/fuzz" "$CORPUS" -max_total_time="$SECS" -print_final_stats=1 -rss_limit_mb=4096 2>&1 \
  | grep -E "^#|NEW|cov:|stat::|ERROR|SUMMARY|Done" | tail -25
rc=${PIPESTATUS[0]}
echo
if [ "$rc" -eq 0 ]; then echo "无崩溃"; else echo "⚠️ libFuzzer 退出码 $rc —— 检查上面的 SUMMARY 与 crash- 文件"; fi
exit $rc
