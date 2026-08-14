#!/usr/bin/env bash
# RTL 静态检查
#
# 判据：hardware/rtl/ 下的每个模块都要以 Verilator `-Wall` 过 lint，一条告警都
# 不留。确属设计意图的现象写进 hardware/rtl/lint_waivers.vlt，逐条给理由，
# 并按文件与信号名精确匹配 —— 新出现的同类告警不会被顺带遮住。
#
# 每个模块都单独作为顶层跑一遍，而不是只跑加速器顶层：否则没有被例化的模块
# （组合算子、采样器等）根本不会被展开，也就不会被检查。
#
# 第二道用 Icarus Verilog 的语法检查，它与 Verilator 的实现不同，
# 能捕到另一批问题；两个仿真器在位宽截断上的语义差异本身也是一道交叉验证。
#
# 前置：verilator（brew install verilator）、iverilog（brew install icarus-verilog）
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# 风扇温控在 hardware/rtl/ 之外（hardware/platform/fan_ctrl/，与密码逻辑分开），但它一样
# 要过 lint —— 风扇写错不会崩，只会安静地让芯片变热，更需要静态检查兜着。
RTL_FILES=$(find "$ROOT/hardware/rtl" "$ROOT/hardware/platform/fan_ctrl" -name '*.v' 2>/dev/null | sort)
# 厂商原语的空壳（BUFGCE_DIV / SYSMONE4 / zynq_ultra_ps_e_0）。
# 少了它，Verilator 对**每一个**模块都报 MODMISSING —— 它是把命令行上所有
# 文件一起看的，一个缺失模块就能让整仓 lint 全红，等于没有 lint。
# 桩只进 lint，不进 impl_bitstream.tcl，理由见文件头。
STUBS="$ROOT/hardware/tb/lint/vendor_stubs.v"
WAIVERS="$ROOT/hardware/rtl/lint_waivers.vlt"

command -v verilator >/dev/null 2>&1 || { echo "SKIP: 没装 verilator（brew install verilator）"; exit 0; }

fail=0
n=0

echo "Verilator lint（-Wall，豁免见 hardware/rtl/lint_waivers.vlt）"
while read -r m; do
  n=$((n + 1))
  out=$(verilator --lint-only -Wall --top-module "$m" "$WAIVERS" $STUBS $RTL_FILES 2>&1)
  if printf '%s' "$out" | grep -qE '^%(Warning|Error)'; then
    printf '  ✗ %s\n' "$m"
    printf '%s\n' "$out" | grep -E '^%(Warning|Error)' | sed 's/^/      /'
    fail=1
  else
    printf '  ✓ %s\n' "$m"
  fi
done < <(grep -h '^module ' $RTL_FILES | sed -E 's/^module ([A-Za-z0-9_]+).*/\1/' | sort)

echo
if command -v iverilog >/dev/null 2>&1; then
  echo "Icarus Verilog 语法检查（-Wall）"
  # sensitivity-entire-array：`always @(*)` 读整块存储时，Icarus 会提示敏感表
  # 覆盖了数组的全部字。这正是所需的行为（Keccak 的一轮组合逻辑要读全部 25 个
  # lane），提示本身不指向缺陷，因此单独关掉；其余 -Wall 项一条不留。
  out=$(iverilog -t null -Wall -Wno-sensitivity-entire-array -o /dev/null $STUBS $RTL_FILES 2>&1)
  if [ -n "$out" ]; then
    printf '%s\n' "$out" | sed 's/^/      /'
    fail=1
  else
    echo "  ✓ 无告警"
  fi
else
  echo "Icarus Verilog 语法检查：SKIP（没装 iverilog）"
fi

echo
if [ "$fail" -eq 0 ]; then
  echo "全部通过（$n 个模块）"
else
  echo "有告警未处理"
fi
exit $fail
