#!/usr/bin/env bash
# 分模块 + Verilator 的**迭代**用仿真 —— 改一处只跑那一处
#
# ============================================================================
# 【为什么要有这个脚本】
# ============================================================================
# tools/rtl_sim.sh 是**全套 + Icarus**：约 40 分钟。拿它当改一行验一次的循环，
# 一天里绝大部分时间都花在重跑没被碰过的模块上（实测：改 mlkem_axi 一个文件，
# 全套里 30 多个与它无关的用例照跑一遍）。
#
# 这个脚本做两件事，各自都是几十倍的差距：
#   ① **只跑你点名的 testbench**；
#   ② 用 **Verilator**（编译型）而不是 Icarus（解释型）。
#
# 实测对比（本机，同一份 RTL）：
#   mldsa_axi 全套 32 条    Icarus ~15–20 分钟   →  Verilator **92 秒**
#   mlkem_axi 全套 23 条    Icarus ~135 秒       →  Verilator **52 秒**
#   tb_mldsa_sign 10 条     Icarus 数分钟        →  Verilator **161 秒**
#
# ============================================================================
# ⚠️ 【它不能替代 rtl_sim.sh —— 这一条不是免责声明，是真踩过】
# ============================================================================
# **Verilator 是二值仿真，不传播 X。** 本轮改动里有三个 bug 正是靠 X 暴露的：
#
#   · 三个 ML-DSA testbench 没驱动新加的 wipe/wipe_addr 口 → Icarus 下是 X，
#     X 一进 ram_dp 的 B 口地址复用，所有读出变成 X，用例当场炸；
#   · mlkem_axi 的 xbar_viol_count 是输入端口，单跑从机时没人驱动 → 同上；
#   · 读没写过的输出缓冲 BRAM → 同上。
#
# **这三个在 Verilator 下都会安静地显示 0，用例照过。** 也就是说：
#   · **开发内环用它求快**；
#   · **合并 / 出定版前必须跑一次 tools/rtl_sim.sh（Icarus，全套）求准。**
# 这个分工在 hardware/tb/cocotb/Makefile 的文件头里也写着，不是本脚本的发明。
#
# ============================================================================
# 用法
# ============================================================================
#   tools/rtl_sim_fast.sh                      # 跑默认那组（见下面 DEFAULT）
#   tools/rtl_sim_fast.sh mlkem_axi            # 只跑一个
#   tools/rtl_sim_fast.sh mlkem_axi mldsa_axi  # 跑几个
#   tools/rtl_sim_fast.sh --list               # 看有哪些名字
#   PARAM_SECURE_ONLY=0 tools/rtl_sim_fast.sh mlkem_axi   # 带参数覆盖
#
# 名字是 TOPLEVEL，不是文件名 —— 与 rtl_sim.sh 里的第二列一致。
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VENV="$ROOT/.venv-rtl"

if [ ! -x "$VENV/bin/cocotb-config" ]; then
  echo "SKIP: 没有 cocotb 环境。建：python3 -m venv .venv-rtl && .venv-rtl/bin/pip install cocotb"
  exit 0
fi
command -v verilator >/dev/null 2>&1 || {
  echo "SKIP: 没装 verilator（brew install verilator）"; exit 0; }
[ -f "$ROOT/vectors/rtl/mont_reduce.hex" ] \
  || python3 "$ROOT/hardware/model/export_vectors.py" >/dev/null

export PATH="$VENV/bin:$PATH"

# TOPLEVEL → MODULE。只列**有独立 testbench**的顶层。
declare -a NAMES=(
  mont_reduce butterfly_ct butterfly_gs ntt_core mlkem_basemul tb_mlkem_units
  mlkem_keygen mlkem_encaps mlkem_decaps
  tb_mldsa_units mldsa_ntt_core tb_mldsa_sampler tb_mldsa_keygen
  tb_mldsa_sign tb_mldsa_verify tb_mldsa_engine
  keccak_f1600 sha3_core
  axi4lite_xbar axi4lite_firewall pqc_accel_axi
  key_vault key_vault_axi mlkem_axi mldsa_axi
  aes_core sm4_core sm3_core sym_vault_top
  fan_ctrl sysmon_drp tb_trng_health
)
module_for() {
  case "$1" in
    mont_reduce)        echo test_ops ;;
    butterfly_ct|butterfly_gs) echo test_butterfly ;;
    ntt_core)           echo test_ntt_core ;;
    mlkem_basemul)      echo test_basemul ;;
    tb_mlkem_units)     echo test_mlkem_units ;;
    mlkem_keygen)       echo test_mlkem_keygen ;;
    mlkem_encaps)       echo test_mlkem_encaps ;;
    mlkem_decaps)       echo test_mlkem_decaps ;;
    tb_mldsa_units)     echo test_mldsa_units ;;
    mldsa_ntt_core)     echo test_mldsa_ntt ;;
    tb_mldsa_sampler)   echo test_mldsa_sampler ;;
    tb_mldsa_keygen)    echo test_mldsa_keygen ;;
    tb_mldsa_sign)      echo test_mldsa_sign ;;
    tb_mldsa_verify)    echo test_mldsa_verify ;;
    tb_mldsa_engine)    echo test_mldsa_engine ;;
    keccak_f1600)       echo test_keccak ;;
    sha3_core)          echo test_sha3_core ;;
    axi4lite_xbar)      echo test_xbar ;;
    axi4lite_firewall)  echo test_firewall ;;
    pqc_accel_axi)      echo test_axi ;;
    key_vault)          echo test_key_vault_core ;;
    key_vault_axi)      echo test_key_vault ;;
    mlkem_axi)          echo test_mlkem_axi ;;
    mldsa_axi)          echo test_mldsa_axi ;;
    aes_core)           echo test_aes ;;
    sm4_core)           echo test_sm4 ;;
    sm3_core)           echo test_sm3 ;;
    sym_vault_top)      echo test_sym_vault ;;
    fan_ctrl)           echo test_fan_ctrl ;;
    sysmon_drp)         echo test_sysmon_drp ;;
    tb_trng_health)     echo test_trng_health ;;
    *)                  echo "" ;;
  esac
}

# 不给参数时的默认组：**总线那一层加它下面的整核**。改 RTL 十有八九动的是这些，
# 而它们又是唯一会端到端对 ACVP 的几条。TRNG 整链走 Makefile.trng，不在这里。
DEFAULT=(mlkem_axi mldsa_axi tb_mldsa_engine mlkem_keygen mlkem_encaps
         mlkem_decaps tb_mldsa_keygen tb_mldsa_sign tb_mldsa_verify sha3_core)

if [ "${1:-}" = "--list" ]; then
  printf '%s\n' "${NAMES[@]}"
  exit 0
fi

TARGETS=("$@")
[ ${#TARGETS[@]} -eq 0 ] && TARGETS=("${DEFAULT[@]}")

RUNDIR="$ROOT/build/rtl_fast.$$"
mkdir -p "$RUNDIR"
trap 'rm -rf "$RUNDIR"' EXIT
cd "$ROOT/hardware/tb/cocotb" || exit 1

fail=0
total=0
t0=$(date +%s)
echo "分模块仿真（Verilator，编译型）"
echo "⚠️ 二值仿真，不传播 X —— 合并前仍要跑一次 tools/rtl_sim.sh（Icarus 全套）"
echo
for top in "${TARGETS[@]}"; do
  mod="$(module_for "$top")"
  if [ -z "$mod" ]; then
    printf '  ? %-22s 不认识这个顶层（tools/rtl_sim_fast.sh --list）\n' "$top"
    fail=1
    continue
  fi
  s=$(date +%s)
  out="$(make -s SIM=verilator SIM_BUILD="$RUNDIR/$top" \
              MODULE="$mod" TOPLEVEL="$top" 2>&1)"
  e=$(date +%s)
  line="$(printf '%s' "$out" \
          | grep -oE 'TESTS=[0-9]+ PASS=[0-9]+ FAIL=[0-9]+ SKIP=[0-9]+' | tail -1)"
  if [ -z "$line" ]; then
    printf '  ✗ %-22s （make 失败，%ss）\n' "$top" "$((e - s))"
    printf '%s\n' "$out" | tail -15 | sed 's/^/      /'
    fail=1
    continue
  fi
  total=$((total + $(printf '%s' "$line" | sed -E 's/TESTS=([0-9]+).*/\1/')))
  case "$line" in
    *"FAIL=0"*) printf '  ✓ %-22s %-38s %ss\n' "$top" "$line" "$((e - s))" ;;
    *)          printf '  ✗ %-22s %-38s %ss\n' "$top" "$line" "$((e - s))"
                printf '%s\n' "$out" | grep -E 'AssertionError|FAIL ' | head -5 \
                  | sed 's/^/      /'
                fail=1 ;;
  esac
done
t1=$(date +%s)

echo
if [ "$fail" -eq 0 ]; then
  echo "全部通过（$total 个测试，共 $((t1 - t0)) 秒）"
else
  echo "有失败（共 $((t1 - t0)) 秒）"
fi
exit $fail
