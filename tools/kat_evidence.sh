#!/usr/bin/env bash
# 生成 KAT / ACVP 测试证据表
#
# 输出是一张 Markdown 表：每类向量的来源、条数、通过/失败/跳过。
# 表由实际运行 kat_runner 得到，不是手写的 —— 合规材料里的数字必须能被重跑复现，
# 手抄的表在向量更新之后会悄悄变成假的。
#
# 用法：./tools/kat_evidence.sh [> docs/kat-evidence.md]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUNNER="$ROOT/build/kat_runner"
VEC="$ROOT/vectors"

if [ ! -x "$RUNNER" ]; then
  echo "SKIP: 找不到 ${RUNNER}，先 cmake --build build" >&2
  exit 0
fi
if [ ! -d "$VEC" ]; then
  echo "SKIP: 找不到 ${VEC}，先跑 ./tools/fetch_vectors.sh" >&2
  exit 0
fi

backend="$("$RUNNER" "$VEC"/mlkem_keygen.kat 2>/dev/null | sed -n 's/^后端: //p' | head -1)"

printf '# KAT / ACVP 测试证据\n\n'
printf '由 `tools/kat_evidence.sh` 运行 `build/kat_runner` 生成，可重跑复现。\n\n'
printf '算法后端：`%s`\n\n' "${backend:-未知}"
printf '| 向量文件 | ACVP 来源 | 记录数 | 通过 | 失败 | 跳过 |\n'
printf '|---|---|---|---|---|---|\n'

total_pass=0
total_fail=0
total_skip=0
rc=0

for f in "$VEC"/*.kat; do
  [ -e "$f" ] || continue
  name="$(basename "$f")"
  src="$(sed -n 's/^# 来源: //p' "$f" | head -1)"
  recs="$(sed -n 's/^# 记录数: //p' "$f" | head -1)"
  line="$("$RUNNER" "$f" 2>/dev/null | sed -n 's/^总计: //p' | head -1)"
  pass="$(printf '%s' "$line" | sed -E 's/.*pass=([0-9]+).*/\1/')"
  fail="$(printf '%s' "$line" | sed -E 's/.*fail=([0-9]+).*/\1/')"
  skip="$(printf '%s' "$line" | sed -E 's/.*skip=([0-9]+).*/\1/')"
  [ -n "$pass" ] || { pass=0; fail=0; skip=0; rc=1; }
  printf '| `%s` | %s | %s | %s | %s | %s |\n' \
    "$name" "${src:-—}" "${recs:-—}" "$pass" "$fail" "$skip"
  total_pass=$((total_pass + pass))
  total_fail=$((total_fail + fail))
  total_skip=$((total_skip + skip))
  [ "$fail" -eq 0 ] || rc=1
done

printf '| **合计** | | | **%d** | **%d** | **%d** |\n' \
  "$total_pass" "$total_fail" "$total_skip"
printf '\n'
printf '跳过的条目是向量里本项目未实现的可选功能，`kat_runner` 会如实计入 skip\n'
printf '而不是当作通过。\n'
exit $rc
