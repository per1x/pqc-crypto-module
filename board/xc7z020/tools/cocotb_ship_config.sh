#!/usr/bin/env bash
# 交付配置的 cocotb 回归（INCLUDE_NTT=0）
#
# 主干的 tools/rtl_sim.sh 跑的是默认配置（含 NTT 核）。XC7Z020 上交付的是
# 不含 NTT 的配置，它有一条只在这个配置下才成立的行为需要验证：
# **操作码 7/8 与其它未实现的模式一样返回 ERRCODE=3**。
# 没有这一遍，"裁掉 NTT"就只是改了个参数，没人确认过契约还成立。
#
# 前置：.venv-rtl 里的 cocotb、iverilog
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"

if [ ! -x "$ROOT/.venv-rtl/bin/cocotb-config" ]; then
  echo "SKIP: 没有 cocotb 环境（python3 -m venv .venv-rtl && .venv-rtl/bin/pip install cocotb）"
  exit 0
fi
command -v iverilog >/dev/null 2>&1 || { echo "SKIP: 没装 iverilog"; exit 0; }

[ -f "$ROOT/vectors/rtl/mont_reduce.hex" ] || \
  python3 "$ROOT/hardware/model/export_vectors.py" >/dev/null

export PATH="$ROOT/.venv-rtl/bin:$PATH"
cd "$ROOT/hardware/tb/cocotb"

LOG="$(mktemp)"
trap 'rm -f "$LOG"; rm -rf "$ROOT/hardware/tb/cocotb/sim_build" "$ROOT/hardware/tb/cocotb/results.xml"' EXIT
rm -rf sim_build results.xml

if ! make -s MODULE=test_axi TOPLEVEL=pqc_accel_axi PARAM_INCLUDE_NTT=0 >"$LOG" 2>&1; then
  echo "  ✗ test_axi（INCLUDE_NTT=0）：make 失败"
  grep -iE "error" "$LOG" | head -8 | sed 's/^/      /'
  exit 1
fi

line="$(grep -oE 'TESTS=[0-9]+ PASS=[0-9]+ FAIL=[0-9]+ SKIP=[0-9]+' "$LOG" | tail -1)"
if [ -z "$line" ]; then
  echo "  ✗ test_axi（INCLUDE_NTT=0）：没拿到结果行"
  tail -10 "$LOG" | sed 's/^/      /'
  exit 1
fi
case "$line" in
  *"FAIL=0"*) echo "  ✓ test_axi（INCLUDE_NTT=0）  $line" ;;
  *)          echo "  ✗ test_axi（INCLUDE_NTT=0）  $line"; exit 1 ;;
esac

# 只在这个配置下成立的那条行为，单独确认它真的被跑到了
if grep -q "未实现的操作码如实报错" "$LOG"; then
  echo "  ✓ 操作码 7/8 在此配置下返回 ERRCODE=3"
else
  echo "  ✗ 没有观察到未实现操作码那一项通过"
  exit 1
fi
