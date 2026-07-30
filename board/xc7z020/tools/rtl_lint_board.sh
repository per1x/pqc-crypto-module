#!/usr/bin/env bash
# 板级 RTL 的静态检查与可综合性检查
#
# 通用 RTL 由主干的 tools/rtl_lint.sh 与 tools/rtl_synth_check.sh 覆盖；
# 这里只补上板级新增的那一层包装 pqc_accel_zynq，判据完全相同：
# Verilator `-Wall` 一条告警都不留，Yosys 能综合。
#
# 【注意豁免表的匹配方式】hardware/rtl/lint_waivers.vlt 里的条目按
# `-file "*/hardware/rtl/*"` 匹配，模式要求路径里 hardware 之前还有内容，
# 因此传给 verilator 的必须是**绝对路径**。用相对路径调用时豁免不会生效，
# 会看到一堆本已审阅过的告警。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"

WAIVERS="$ROOT/hardware/rtl/lint_waivers.vlt"
BOARD_RTL="$ROOT/board/xc7z020/rtl/pqc_accel_zynq.v"
DEPS="
$ROOT/hardware/rtl/mlkem/mont_reduce.v
$ROOT/hardware/rtl/mlkem/butterfly.v
$ROOT/hardware/rtl/mlkem/ntt_core.v
$ROOT/hardware/rtl/keccak/keccak_f1600.v
$ROOT/hardware/rtl/bus/axi4lite_regs.v
$ROOT/hardware/rtl/bus/pqc_accel_axi.v
"
DEPS=$(printf '%s' "$DEPS" | tr '\n' ' ')

fail=0

if command -v verilator >/dev/null 2>&1; then
  echo "Verilator lint（-Wall，含主干豁免表）"
  out=$(verilator --lint-only -Wall --top-module pqc_accel_zynq \
        "$WAIVERS" $DEPS "$BOARD_RTL" 2>&1)
  if printf '%s' "$out" | grep -qE '^%(Warning|Error)'; then
    printf '  ✗ pqc_accel_zynq\n'
    printf '%s\n' "$out" | grep -E '^%(Warning|Error)' | sed 's/^/      /'
    fail=1
  else
    printf '  ✓ pqc_accel_zynq\n'
  fi
else
  echo "Verilator lint：SKIP（没装 verilator）"
fi

echo
if command -v yosys >/dev/null 2>&1; then
  echo "Yosys 可综合性检查"
  if yosys -q -p "read_verilog $DEPS $BOARD_RTL
      hierarchy -top pqc_accel_zynq -check
      proc
      opt
      memory
      opt
      check -assert
    " >/dev/null 2>&1; then
    printf '  ✓ pqc_accel_zynq\n'
  else
    printf '  ✗ pqc_accel_zynq —— 无法综合\n'
    fail=1
  fi
else
  echo "Yosys 可综合性检查：SKIP（没装 yosys）"
fi

echo
if [ "$fail" -eq 0 ]; then
  echo "板级 RTL 检查全部通过"
else
  echo "板级 RTL 检查有失败"
fi
exit $fail
