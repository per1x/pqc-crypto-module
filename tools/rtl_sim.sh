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
  # 必须清干净：残留的 sim_build 会让 make 认为没变化而跳过重新编译，
  # 于是新的 TOPLEVEL 用上一次的编译产物，报"找不到根句柄"
  rm -rf sim_build results.xml
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
run test_mlkem_keygen mlkem_keygen
for d in 1 4 5 10 11; do
  run test_compress   mlkem_compress   "$d"
done
for d in 1 4 5 10 11; do
  run test_decompress mlkem_decompress "$d"
done
echo
echo "  ML-DSA 算子与数据通路"
run test_mldsa_units tb_mldsa_units
run test_mldsa_ntt   mldsa_ntt_core

echo
echo "  Keccak"
run test_keccak      keccak_f1600
run test_sha3_core   sha3_core

echo
echo "  总线接口"
run test_axi         pqc_accel_axi

echo
echo "  噪声源健康检测"
run test_trng_health tb_trng_health

# TRNG 整链走单独的 Makefile.trng：环振仿真要 ps 级时间单位，
# 与上面那套 1 ns 的编译不能共用（理由见 Makefile.trng 文件头）。
run_trng() { # run_trng <MODULE> <TOPLEVEL> [PARAMS]
  local label="$2"
  [ -n "${3:-}" ] && label="$2 ${3#-P}"
  rm -rf "sim_build_$2" "sim_build_$2_p" results.xml
  if ! make -s -f Makefile.trng MODULE="$1" TOPLEVEL="$2" PARAMS="${3:-}" >"$LOG" 2>&1; then
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

echo
echo "  TRNG 整链"
run_trng test_trng_source    trng_source
run_trng test_trng_cond      trng_cond
run_trng test_trng_top       trng_top
run_trng test_trng_axi       trng_axi
# 告警要确定性发生，把 RCT 阈值压到 2 —— 等自然出现 41 连的话跑不完
run_trng test_trng_top_alarm trng_top -Ptrng_top.RCT_CUTOFF=2
rm -rf sim_build_trng_*

rm -rf sim_build results.xml
echo
if [ "$fail" -eq 0 ]; then
  echo "全部通过（$total 个测试）"
else
  echo "有失败"
fi
exit $fail
