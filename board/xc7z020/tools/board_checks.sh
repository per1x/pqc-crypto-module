#!/usr/bin/env bash
# 板级分支上"不用板子就能验"的全部检查，一个入口跑完
#
# 分支带来的东西有四类，每类都有对应的离线判据：
#   板级包装 RTL     Verilator -Wall + Yosys 可综合性
#   Vivado Tcl       tclsh 桩执行（控制流真实跑到）
#   交付配置         cocotb 在 INCLUDE_NTT=0 下跑寄存器契约与数据面
#   资源预算         Yosys 综合到 7 系列单元，并对 LUT 上限做硬性检查
#
# 剩下必须上板的只有：比特流生成、烧录、真实地址映射是否与地址表一致、
# 时钟与复位是否真的通、时序是否收敛。这些在 docs/ 下逐项标注了。
#
# 用法：board/xc7z020/tools/board_checks.sh [--fast]
#       --fast 跳过资源预算（Yosys 综合几分钟）
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"

FAST=0
[ "${1:-}" = "--fast" ] && FAST=1

fail=0
run_step() { # run_step <标题> <命令...>
  echo "════ $1"
  if "${@:2}"; then
    echo
  else
    echo "  ↑ 这一步失败"
    fail=1
    echo
  fi
}

run_step "板级 RTL 静态检查" "$HERE/rtl_lint_board.sh"
run_step "Vivado Tcl 离线检查" "$HERE/tcl_check.sh"

run_step "交付配置的 cocotb 回归（INCLUDE_NTT=0）" "$HERE/cocotb_ship_config.sh"

if [ "$FAST" -eq 0 ]; then
  run_step "XC7Z020 资源预算" "$HERE/resource_budget.sh"
else
  echo "════ XC7Z020 资源预算"
  echo "  SKIP：--fast"
  echo
fi

if [ "$fail" -eq 0 ]; then
  echo "板级离线检查全部通过"
else
  echo "板级离线检查有失败"
fi
exit $fail
