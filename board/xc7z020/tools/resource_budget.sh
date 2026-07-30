#!/usr/bin/env bash
# XC7Z020 资源预算：用 Yosys 综合到 7 系列单元后统计
#
# 【这份数字是什么，不是什么】
# 它是 Yosys 用开源的 7 系列单元库综合出来的结果，**不是 Vivado 的实现结果**。
# 两者会有差距：Vivado 的打包（LUT 组合进 SLICE、FF 与 LUT 共享、SRL 推断、
# BRAM 推断策略）与 Yosys 不同，通常 Vivado 的 LUT 数会更低一些，BRAM 推断更积极。
#
# 那它有什么用：**在没有 Vivado 的机器上，回答"数量级上放不放得下"**。
# 一块 XC7Z020 有 53200 个 LUT、106400 个触发器、220 个 DSP48E1、140 个 36Kb BRAM。
# 如果 Yosys 报出来的数字已经是这些的几倍，那不用等 Vivado 就知道要重新设计；
# 如果只占十几个百分点，那余量足够，等有板子时再用 Vivado 的报告核准。
#
# 上板前必须用 board/xc7z020/vivado/build_bitstream.tcl 生成 utilization.rpt，
# 以那份为准。
#
# 前置：yosys（brew install yosys）
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
RTL=$(find "$ROOT/hardware/rtl" -name '*.v' | sort | tr '\n' ' ')
BOARD_RTL="$ROOT/board/xc7z020/rtl/pqc_accel_zynq.v"

command -v yosys >/dev/null 2>&1 || { echo "SKIP: 没装 yosys（brew install yosys）"; exit 0; }

# XC7Z020 的可用资源
LUT_TOTAL=53200
FF_TOTAL=106400
DSP_TOTAL=220
BRAM36_TOTAL=140

LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

# 统计一个顶层：输出 "LUT FF DSP BRAM36 LUTRAM MUXF CARRY4"
# 第二个参数是可选的顶层参数覆盖，例如 "-chparam INCLUDE_NTT 0"
synth_one() {
  local top="$1"
  local chparam="${2:-}"
  yosys -p "read_verilog $RTL $BOARD_RTL
    $chparam
    synth_xilinx -family xc7 -flatten -top $top
    stat
  " >"$LOG" 2>&1 || { echo "FAIL"; return; }
  # 综合时加 -flatten：层次一展平就只剩一个模块、一份单元清单，
  # 不必去猜 `=== design hierarchy ===` 那一段的分组方式（它把顶层自身的单元与
  # 子模块的单元分成两段列出，段的边界随层数变化，解析起来很脆）。
  #
  # INV 计进 LUT：Vivado 会把它实现成一个 LUT1。
  # IBUF/OBUF/BUFG 只在把模块单独当顶层综合时才产生，真实设计里这些端口在片内，
  # 因此不计入。
  awk -v top="$top" '
    $0 == "=== " top " ===" { if (!seen) { inblock = 1; seen = 1; next } }
    inblock && /^$/ { if (started) inblock = 0 }
    inblock && /^[ \t]+[0-9]+[ \t]+[A-Z][A-Z0-9_$]*$/ {
      started = 1
      n = $1; name = $2
      if (name ~ /^LUT[1-6]$/ || name == "LUT6_2" || name == "INV") lut += n
      else if (name ~ /^FD[RSCP]E?$/ || name ~ /^FD[RSCP]E_1$/) ff += n
      else if (name == "DSP48E1") dsp += n
      else if (name == "RAMB18E1") bram += n / 2
      else if (name == "RAMB36E1") bram += n
      else if (name ~ /^RAM[0-9]+[XM]/) lutram += n
      else if (name == "MUXF7" || name == "MUXF8") muxf += n
      else if (name == "CARRY4") carry += n
    }
    END { printf "%d %d %d %d %d %d %d\n", lut, ff, dsp, bram, lutram, muxf, carry }
  ' "$LOG"
}

pct() { awk -v a="$1" -v b="$2" 'BEGIN { printf "%.1f%%", 100*a/b }'; }

echo "XC7Z020 资源预算（Yosys 综合到 7 系列单元，非 Vivado 实现结果）"
echo
printf '%-22s %8s %8s %6s %7s %8s %8s\n' "模块" "LUT" "FF" "DSP" "BRAM36" "MUXF" "CARRY4"
printf '%-22s %8s %8s %6s %7s %8s %8s\n' "----------------------" "--------" "--------" "------" "-------" "--------" "--------"

measure() { # measure <显示名> <顶层> [chparam]
  read -r lut ff dsp bram lutram muxf carry <<<"$(synth_one "$2" "${3:-}")"
  if [ "$lut" = "FAIL" ]; then
    printf '%-26s %8s\n' "$1" "综合失败"
    return 1
  fi
  printf '%-26s %8s %8s %6s %7s %8s %8s\n' "$1" "$lut" "$ff" "$dsp" "$bram" "$muxf" "$carry"
  LAST_LUT=$lut; LAST_FF=$ff; LAST_DSP=$dsp; LAST_BRAM=$bram
}

# 交付配置：不含 NTT 核。取舍的依据就在下面两行数字里。
measure "pqc_accel_zynq（交付配置）" pqc_accel_zynq "chparam -set INCLUDE_NTT 0 pqc_accel_axi"
SHIP_LUT=$LAST_LUT; SHIP_FF=$LAST_FF; SHIP_DSP=$LAST_DSP; SHIP_BRAM=$LAST_BRAM
measure "pqc_accel_zynq（含 NTT）"   pqc_accel_zynq
FULL_LUT=$LAST_LUT

for top in ntt_core keccak_f1600 mldsa_ntt_core \
           mlkem_rej_uniform mldsa_rej_uniform_buf trng_health axi4lite_regs; do
  measure "$top" "$top"
done

echo
echo "交付配置占 XC7Z020 的比例（不含 PS7、AXI 互联与 AXI-DMA，那些由 Vivado 的 IP 提供）"
printf '  LUT     %8s / %-8s  %s\n' "$SHIP_LUT"  "$LUT_TOTAL"    "$(pct "$SHIP_LUT" "$LUT_TOTAL")"
printf '  FF      %8s / %-8s  %s\n' "$SHIP_FF"   "$FF_TOTAL"     "$(pct "$SHIP_FF" "$FF_TOTAL")"
printf '  DSP48E1 %8s / %-8s  %s\n' "$SHIP_DSP"  "$DSP_TOTAL"    "$(pct "$SHIP_DSP" "$DSP_TOTAL")"
printf '  BRAM36  %8s / %-8s  %s\n' "$SHIP_BRAM" "$BRAM36_TOTAL" "$(pct "$SHIP_BRAM" "$BRAM36_TOTAL")"
echo
echo "AXI-DMA 与互联另计，按 Xilinx 的典型值：axi_dma（simple mode，32 位）"
echo "约 1200 LUT / 1600 FF，AXI 互联每个从口约 400 LUT。"
echo

# 预算是要能失败的检查，不是一段说明文字。
# 上限取器件容量的 70%：留给 AXI-DMA、互联与布线拥塞的余量。
LIMIT=$((LUT_TOTAL * 70 / 100))
rc=0
if [ "$SHIP_LUT" -gt "$LIMIT" ]; then
  echo "✗ 交付配置 $SHIP_LUT LUT 超出预算上限 $LIMIT（器件容量的 70%）"
  rc=1
else
  echo "✓ 交付配置 $SHIP_LUT LUT 在预算上限 $LIMIT 之内（器件容量的 70%）"
fi
if [ "$FULL_LUT" -le "$LUT_TOTAL" ]; then
  echo "⚠ 含 NTT 的配置只用了 $FULL_LUT LUT，已不超出 $LUT_TOTAL —— "
  echo "  裁掉 NTT 的理由可能已经不成立，重新评估 docs/resource-budget.md 的结论"
else
  echo "✓ 含 NTT 的配置 $FULL_LUT LUT 超出器件容量 $LUT_TOTAL，裁掉它的理由成立"
fi
exit $rc
