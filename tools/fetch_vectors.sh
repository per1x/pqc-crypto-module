#!/usr/bin/env bash
# 拉取 NIST ACVP 最终版 FIPS 203/204 向量，然后展平成 vectors/*.kat
#
# 为什么钉死 commit：ACVP-Server 的 master 会滚动更新，不钉死就无法解释
# "上周还全绿今天怎么挂了"。升级向量时改这里并重跑，把差异写进 docs/design/development-log.md。
set -euo pipefail

# ACVP-Server master @ 2026-07-28
ACVP_REF="${ACVP_REF:-ad33b3d9504491767f1aa76382464f3b3fa2359e}"
BASE="https://raw.githubusercontent.com/usnistgov/ACVP-Server/${ACVP_REF}/gen-val/json-files"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/vectors/acvp"

DIRS=(
  ML-KEM-keyGen-FIPS203
  ML-KEM-encapDecap-FIPS203
  ML-DSA-keyGen-FIPS204
  ML-DSA-sigGen-FIPS204
  ML-DSA-sigVer-FIPS204
)

echo "ACVP ref: $ACVP_REF"
for d in "${DIRS[@]}"; do
  mkdir -p "$OUT/$d"
  for f in prompt.json expectedResults.json; do
    printf '  %-34s %s ... ' "$d" "$f"
    curl -sSL --fail --max-time 300 -o "$OUT/$d/$f" "$BASE/$d/$f"
    echo "$(wc -c < "$OUT/$d/$f") B"
  done
done

PY="${PYTHON:-python3}"
"$PY" "$ROOT/tools/acvp_to_kat.py"
