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

# 一个 MODULE 里的用例分属不同顶层时用这个：多带一个用例名做过滤。
# 打包那三个模块就是这种情况 —— 共用一份测试文件，但 t₁/t₀/η 各是一个顶层，
# 不过滤的话每个顶层都会去跑另外两个的用例，全都报失败。
run_one() { # run_one <MODULE> <TOPLEVEL> <TESTCASE> [标签]
  local label="${4:-$3}"
  rm -rf sim_build results.xml
  if ! COCOTB_TEST_FILTER="$3" make -s MODULE="$1" TOPLEVEL="$2" >"$LOG" 2>&1; then
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
run test_mlkem_encaps mlkem_encaps
run test_mlkem_decaps mlkem_decaps
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
run_one test_mldsa_pack mldsa_polyt1_pack   test_polyt1_pack   "polyt1 (10 位)"
run_one test_mldsa_pack mldsa_polyt0_pack   test_polyt0_pack   "polyt0 (13 位)"
run_one test_mldsa_pack mldsa_polyeta_pack  test_polyeta_pack  "polyeta (η=2)"
run_one test_mldsa_sampler tb_mldsa_sampler test_poly_uniform "RejNTTPoly (SHAKE128)"
run_one test_mldsa_sampler tb_mldsa_sampler test_poly_eta     "RejBoundedPoly (η=2)"

echo
echo "  Keccak"
run test_keccak      keccak_f1600
run test_sha3_core   sha3_core

echo
echo "  总线接口"
run test_xbar        axi4lite_xbar
run test_firewall    axi4lite_firewall
run test_axi         pqc_accel_axi
run test_key_vault_core key_vault
run test_key_vault   key_vault_axi
run test_mlkem_axi   mlkem_axi

echo
echo "  对称与国密"
run test_aes         aes_core
run test_sm4         sm4_core
run test_sm3         sm3_core
run test_sym_vault   sym_vault_top

echo
echo "  风扇控制"
run test_fan_ctrl    fan_ctrl
run test_sysmon_drp  sysmon_drp

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
# 调理器吃的必须与健康检测吃的是同一条流 —— 逐比特对比两条流本身，
# 不是看我自己写的那个 drops 计数（用它验自己等于没验）。
run_trng test_trng_nodrop    trng_top
# 反证：把取样 FIFO 压到 2 深，强迫溢出，证明那条计数是活的 ——
# 否则"默认深度下 drops==0"与"计数根本没接上"长得一模一样。
run_trng test_trng_drops     trng_top -Ptrng_top.SAMPLE_FIFO_DEPTH=2
run_trng test_trng_axi       trng_axi
# 告警要确定性发生，把 RCT 阈值压到 2 —— 等自然出现 41 连的话跑不完
run_trng test_trng_top_alarm trng_top -Ptrng_top.RCT_CUTOFF=2
# 原始噪声抽头：默认 RAW_TAP=0（通路根本不存在），只有这一条用例把它打开。
# 打开的构建只用于跑 SP 800-90B 取数 —— 把噪声源的原始比特摆在总线上
# 等于把熵源内部状态给了读它的人，不是产品形态。
run_trng test_trng_raw       trng_top -Ptrng_top.RAW_TAP=1
rm -rf sim_build_trng_*

rm -rf sim_build results.xml
echo
if [ "$fail" -eq 0 ]; then
  echo "全部通过（$total 个测试）"
else
  echo "有失败"
fi
exit $fail
