#!/bin/sh
# mldsa_grid.sh —— ML-DSA 的九格矩阵：KeyGen / Sign / Verify × 44 / 65 / 87
#
# 这是 ML-DSA 改动的**总关卡**。每一格都跑到对 ACVP 官方向量为止：
#   keygen 8 条（含 pk/sk 逐字节）  sign 10 条（含 siggen det+rnd 各 15 条逐字节）
#   verify 4 条（含 sigver 15 条 + 3 条自造反例）
# 合计 66 条。任何一格红都不许合并。
#
# 用法：
#   sh tools/mldsa_grid.sh              # 默认 Verilator，约 12 分钟
#   sh tools/mldsa_grid.sh icarus       # Icarus，约 1.8 小时
#
# ============================================================================
# 【两个仿真器怎么分工 —— 别只跑快的那个】
# ============================================================================
# 实测（同一份 RTL、同一批向量、九格全量）：
#     Verilator  ~12 分钟
#     Icarus     ~1.8 小时          ≈ 8.9×
#
# 所以**开发内环用 Verilator**：改一版验一版，等得起。
#
# ⚠️ **但合并前、出 bitstream 前必须用 Icarus 再跑一遍。**
#    Verilator 是二值仿真、**不传播 X**，而本项目坑表的第 1 条就是
#    "空敏感列表 → 输出 X"（见 docs/reference/mldsa-keygen-design.zh-CN.md）。
#    那类 bug 在 Icarus 下暴露成 X，在 Verilator 下会安静地变成 0 —— 也就是说
#    只跑 Verilator 的话，这个项目最典型的一类 RTL 错误会全部漏过去。
#
# 各 tb 的参数表不同（keygen 只有 K/L/ETA，verify 没有 ETA），多传一个
# iverilog 和 verilator 都会报 "parameter not found" —— 下面按 op 分别给。

SIMU="${1:-verilator}"
cd /Volumes/Work/projects/code/pqc-hsm-fpga/hardware/tb/cocotb || exit 1
export PATH=/Volumes/Work/projects/code/pqc-hsm-fpga/.venv-rtl/bin:$PATH

for P in 44 65 87; do
  case $P in
    44) K=4; L=4; ETA=2; TAU=39; G1=17; MD=0; OMG=80; BETA=78;  CTB=32 ;;
    65) K=6; L=5; ETA=4; TAU=49; G1=19; MD=1; OMG=55; BETA=196; CTB=48 ;;
    87) K=8; L=7; ETA=2; TAU=60; G1=19; MD=1; OMG=75; BETA=120; CTB=64 ;;
  esac
  for M in keygen sign verify; do
    B="g_${SIMU}_${P}_${M}"; rm -rf "$B"
    # 各 tb 的参数表不同：keygen 只有 K/L/ETA，verify 没有 ETA。
    # 多传一个 iverilog/verilator 都会报 "parameter not found"。
    case $M in
      keygen) PA="PARAM_K=$K PARAM_L=$L PARAM_ETA=$ETA" ;;
      sign)   PA="PARAM_K=$K PARAM_L=$L PARAM_ETA=$ETA PARAM_TAU=$TAU PARAM_G1LOG=$G1 PARAM_MODE=$MD PARAM_OMG=$OMG PARAM_BETA=$BETA PARAM_CTB=$CTB" ;;
      verify) PA="PARAM_K=$K PARAM_L=$L PARAM_TAU=$TAU PARAM_G1LOG=$G1 PARAM_MODE=$MD PARAM_OMG=$OMG PARAM_BETA=$BETA PARAM_CTB=$CTB" ;;
    esac
    LOG=/tmp/grid_${SIMU}_${P}_${M}.log
    env SIM="$SIMU" SIM_BUILD="$B" MLDSA_ALG="ML-DSA-$P" $PA \
        make MODULE="test_mldsa_$M" TOPLEVEL="tb_mldsa_$M" > "$LOG" 2>&1
    line=$(grep -E 'TESTS=' "$LOG" | tail -1 | sed 's/.*\*\* //; s/ *\*\*.*//')
    if [ -z "$line" ]; then
      echo "  $P/$M  ❌ 没跑起来 —— $(grep -iE 'error|not found' "$LOG" | head -1)"
    else
      echo "  $P/$M  $line"
    fi
    rm -rf "$B"
  done
done
