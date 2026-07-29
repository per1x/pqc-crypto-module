#!/usr/bin/env bash
# 跑全部 cocotb 对拍（路线图 §5.3）
#
# 前置：iverilog（brew install icarus-verilog）
#       cocotb（本仓库用 .venv-rtl 虚拟环境 —— PEP 668 禁止往系统 Python 装包）
#       黄金向量（python3 model/export_vectors.py，脚本会自动生成）
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VENV="$ROOT/.venv-rtl"

if [ ! -x "$VENV/bin/cocotb-config" ]; then
  echo "SKIP: 没有 cocotb 环境。建：python3 -m venv .venv-rtl && .venv-rtl/bin/pip install cocotb"
  exit 0
fi
command -v iverilog >/dev/null 2>&1 || { echo "SKIP: 没装 iverilog（brew install icarus-verilog）"; exit 0; }
[ -f "$ROOT/vectors/rtl/mont_reduce.hex" ] || python3 "$ROOT/model/export_vectors.py" >/dev/null

export PATH="$VENV/bin:$PATH"
cd "$ROOT/tb/cocotb"
LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT
fail=0

run() { # run <MODULE> <TOPLEVEL>
  make -s clean >/dev/null 2>&1          # 必须清，否则 make 认为没变化直接跳过
  if ! make -s MODULE="$1" TOPLEVEL="$2" >"$LOG" 2>&1; then
    printf '  ✗ %-14s %-14s （make 失败）\n' "$1" "$2"; fail=1; return
  fi
  local line
  line="$(grep -oE 'TESTS=[0-9]+ PASS=[0-9]+ FAIL=[0-9]+ SKIP=[0-9]+' "$LOG" | tail -1)"
  if [ -z "$line" ]; then
    printf '  ✗ %-14s %-14s （没拿到结果行）\n' "$1" "$2"; fail=1; return
  fi
  case "$line" in
    *"FAIL=0"*) printf '  ✓ %-14s %-14s %s\n' "$1" "$2" "$line" ;;
    *)          printf '  ✗ %-14s %-14s %s\n' "$1" "$2" "$line"; fail=1 ;;
  esac
  grep -oE '[a-z_]+: [0-9]+ 条三方一致|定义式与输出范围均成立' "$LOG" | sed 's/^/      /'
}

echo "cocotb 对拍（Icarus Verilog）"
run test_ops       mont_reduce
run test_butterfly butterfly_ct
run test_butterfly butterfly_gs
run test_ntt_core   ntt_core
make -s clean >/dev/null 2>&1
echo
[ "$fail" -eq 0 ] && echo "全部通过" || echo "有失败"
exit $fail
