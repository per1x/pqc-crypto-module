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

# 统计一个顶层：输出 "LUT FF DSP BRAM36"
synth_one() {
  local top="$1"
  yosys -p "read_verilog $RTL $BOARD_RTL
    synth_xilinx -family xc7 -top $top
    stat
  " >"$LOG" 2>&1 || { echo "FAIL"; return; }
  # stat 的格式是「计数 单元名」，且只统计顶层那一段。
  # INV 计进 LUT：Vivado 会把它实现成一个 LUT1。
  # IBUF/OBUF 是把叶子模块单独当顶层综合才产生的，真实设计里这些端口在片内，
  # 因此不计入。
  awk '
    /^=== / { inblock = ($2 == top) }
    inblock && /^ +[0-9]+ +[A-Z0-9_$]+$/ {
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
  ' top="$top" "$LOG"
}

pct() { awk -v a="$1" -v b="$2" 'BEGIN { printf "%.1f%%", 100*a/b }'; }

echo "XC7Z020 资源预算（Yosys 综合到 7 系列单元，非 Vivado 实现结果）"
echo
printf '%-22s %8s %8s %6s %7s %8s\n' "模块" "LUT" "FF" "DSP" "BRAM36" "LUTRAM"
printf '%-22s %8s %8s %6s %7s %8s\n' "----------------------" "--------" "--------" "------" "-------" "--------"

for top in pqc_accel_zynq pqc_accel_axi ntt_core keccak_f1600 mldsa_ntt_core \
           mlkem_rej_uniform mldsa_rej_uniform_buf trng_health axi4lite_regs; do
  read -r lut ff dsp bram lutram muxf carry <<<"$(synth_one "$top")"
  if [ "$lut" = "FAIL" ]; then
    printf '%-22s %8s\n' "$top" "综合失败"
    continue
  fi
  printf '%-22s %8s %8s %6s %7s %8s\n' "$top" "$lut" "$ff" "$dsp" "$bram" "$lutram"
  if [ "$top" = "pqc_accel_zynq" ]; then
    TOP_LUT=$lut; TOP_FF=$ff; TOP_DSP=$dsp; TOP_BRAM=$bram
  fi
done

echo
echo "顶层占 XC7Z020 的比例（不含 PS7、AXI 互联与 AXI-DMA，那些由 Vivado 的 IP 提供）"
printf '  LUT     %8s / %-8s  %s\n' "$TOP_LUT"  "$LUT_TOTAL"    "$(pct "$TOP_LUT" "$LUT_TOTAL")"
printf '  FF      %8s / %-8s  %s\n' "$TOP_FF"   "$FF_TOTAL"     "$(pct "$TOP_FF" "$FF_TOTAL")"
printf '  DSP48E1 %8s / %-8s  %s\n' "$TOP_DSP"  "$DSP_TOTAL"    "$(pct "$TOP_DSP" "$DSP_TOTAL")"
printf '  BRAM36  %8s / %-8s  %s\n' "$TOP_BRAM" "$BRAM36_TOTAL" "$(pct "$TOP_BRAM" "$BRAM36_TOTAL")"
echo
echo "AXI-DMA 与互联另计，按 Xilinx 的典型值：axi_dma（simple mode，32 位）"
echo "约 1200 LUT / 1600 FF，AXI 互联每个从口约 400 LUT。"
