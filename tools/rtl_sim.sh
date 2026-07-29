#!/usr/bin/env bash
# 跑全部 cocotb 对拍
#
# 前置：iverilog（brew install icarus-verilog）
#       cocotb（本仓库用 .venv-rtl 虚拟环境 —— PEP 668 禁止往系统 Python 装包）
#       黄金向量（python3 hardware/model/export_vectors.py，脚本会自动生成）
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VENV="$ROOT/.venv-rtl"

if [ ! -x "$VENV/bin/cocotb-config" ]; then
  echo "SKIP: 没有 cocotb 环境。建：python3 -m venv .venv-rtl && .venv-rtl/bin/pip install cocotb"
  exit 0
fi
command -v iverilog >/dev/null 2>&1 || { echo "SKIP: 没装 iverilog（brew install icarus-verilog）"; exit 0; }
[ -f "$ROOT/vectors/rtl/mont_reduce.hex" ] || python3 "$ROOT/hardware/model/export_vectors.py" >/dev/null

export PATH="$VENV/bin:$PATH"
cd "$ROOT/hardware/tb/cocotb"
LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT
fail=0
total=0

run() { # run <MODULE> <TOPLEVEL> [PARAM_D]
  local label="$2"
  [ -n "${3:-}" ] && label="$2 D=$3"
  make -s clean >/dev/null 2>&1          # 必须清，否则 make 认为没变化直接跳过
  if ! make -s MODULE="$1" TOPLEVEL="$2" PARAM_D="${3:-}" >"$LOG" 2>&1; then
    printf '  ✗ %-18s %-24s （make 失败）\n' "$1" "$label"; fail=1; return
  fi
  local line
  line="$(grep -oE 'TESTS=[0-9]+ PASS=[0-9]+ FAIL=[0-9]+ SKIP=[0-9]+' "$LOG" | tail -1)"
  if [ -z "$line" ]; then
    printf '  ✗ %-18s %-24s （没拿到结果行）\n' "$1" "$label"; fail=1; return
  fi
  total=$((total + $(printf '%s' "$line" | sed -E 's/TESTS=([0-9]+).*/\1/')))
  case "$line" in
    *"FAIL=0"*) printf '  ✓ %-18s %-24s %s\n' "$1" "$label" "$line" ;;
    *)          printf '  ✗ %-18s %-24s %s\n' "$1" "$label" "$line"; fail=1 ;;
  esac
}

echo "cocotb 对拍（Icarus Verilog）"
echo
echo "  ML-KEM 算子与数据通路"
run test_ops         mont_reduce
run test_butterfly   butterfly_ct
run test_butterfly   butterfly_gs
run test_ntt_core    ntt_core
run test_basemul     mlkem_basemul
run test_mlkem_units tb_mlkem_units
for d in 1 4 5 10 11; do
  run test_compress   mlkem_compress   "$d"
done
for d in 1 4 5 10 11; do
  run test_decompress mlkem_decompress "$d"
done
echo
echo "  Keccak"
run test_keccak      keccak_f1600

make -s clean >/dev/null 2>&1
echo
if [ "$fail" -eq 0 ]; then
  echo "全部通过（$total 个测试）"
else
  echo "有失败"
fi
exit $fail
