#!/bin/sh
# mldsa_grid.sh —— ML-DSA 的十二格矩阵：KeyGen / Sign / Verify / **AXI** × 44 / 65 / 87
#
# 这是 ML-DSA 改动的**总关卡**。每一格都跑到对 ACVP 官方向量为止：
#   keygen 8 条（含 pk/sk 逐字节）  sign 10 条（含 siggen det+rnd 各 15 条逐字节）
#   verify 4 条（含 sigver 15 条 + 3 条自造反例）
#   axi   22 条（整条链路 AXI → engine → 三个核，判据同样是 ACVP + oracle）
# 任何一格红都不许合并。
#
# ⚠️ 第四列 axi 是替身删掉之后加的。以前 mldsa_axi 接的是行为级替身，
#    参数集对它没有意义，所以不在这张表里；现在它接真 engine，而 engine 是
#    **编译期参数化**的（pset 对不上就拒绝启动），于是它和另外三列一样，
#    每个参数集都要单独跑一遍。
#    —— 这一列不是形式：pk 最后一个字节那个段边界 bug（见 mldsa_axi.v 里
#    out_addr 那段注释）只有在"真 engine + 整段读回来对 ACVP"时才现形。
#
# 用法：
#   sh tools/mldsa_grid.sh              # 默认 Verilator，约 15 分钟
#   sh tools/mldsa_grid.sh icarus       # Icarus，数小时
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
# ⚠️ 路径从**脚本自己的位置**推出来，不写死某一个 checkout。
#    写死的后果很隐蔽：在另一个 worktree 里跑这个脚本，跑的是别人那棵树的 RTL，
#    而输出看起来完全正常 —— 于是"改完跑了关卡"变成了"跑了别人的关卡"。
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT/hardware/tb/cocotb" || exit 1
[ -d "$ROOT/.venv-rtl/bin" ] && export PATH="$ROOT/.venv-rtl/bin:$PATH"

for P in 44 65 87; do
  case $P in
    # PS 是 pset 端口/参数的序号（0/1/2），engine 与 mldsa_axi 拿它校验
    44) K=4; L=4; ETA=2; TAU=39; G1=17; MD=0; OMG=80; BETA=78;  CTB=32; PS=0 ;;
    65) K=6; L=5; ETA=4; TAU=49; G1=19; MD=1; OMG=55; BETA=196; CTB=48; PS=1 ;;
    87) K=8; L=7; ETA=2; TAU=60; G1=19; MD=1; OMG=75; BETA=120; CTB=64; PS=2 ;;
  esac
  for M in keygen sign verify axi; do
    B="g_${SIMU}_${P}_${M}"; rm -rf "$B"
    # 各 tb 的参数表不同：keygen 只有 K/L/ETA，verify 没有 ETA。
    # 多传一个 iverilog/verilator 都会报 "parameter not found"。
    #
    # ⚠️ **axi 那一格一个参数都不传。** mldsa_axi 与它下面的 engine 现在是
    #    **运行时选参数集**的（engine 没有任何参数，从机的长度计算用 MODE
    #    寄存器里的 pset 线），所以参数集靠**测试用例写寄存器**来切，不靠
    #    编译期覆盖。这一格因此天然就是"同一份 RTL 跑三套"的证据。
    #    这里曾经传过全套 + PSET —— 那是三个核还只支持编译期单参数集时的写法，
    #    运行时化之后再传就会报 "Parameters ... were not found in the design"。
    case $M in
      keygen) PA="PARAM_K=$K PARAM_L=$L PARAM_ETA=$ETA" ;;
      sign)   PA="PARAM_K=$K PARAM_L=$L PARAM_ETA=$ETA PARAM_TAU=$TAU PARAM_G1LOG=$G1 PARAM_MODE=$MD PARAM_OMG=$OMG PARAM_BETA=$BETA PARAM_CTB=$CTB" ;;
      verify) PA="PARAM_K=$K PARAM_L=$L PARAM_TAU=$TAU PARAM_G1LOG=$G1 PARAM_MODE=$MD PARAM_OMG=$OMG PARAM_BETA=$BETA PARAM_CTB=$CTB" ;;
      axi)    PA="" ;;
    esac
    case $M in
      axi) TOP=mldsa_axi ;;
      *)   TOP="tb_mldsa_$M" ;;
    esac
    LOG=/tmp/grid_${SIMU}_${P}_${M}.log
    env SIM="$SIMU" SIM_BUILD="$B" MLDSA_ALG="ML-DSA-$P" $PA \
        make MODULE="test_mldsa_$M" TOPLEVEL="$TOP" > "$LOG" 2>&1
    # ⚠️ grep 必须带 -a。日志里有 cocotb 打的中文与制表符，grep 偶尔会把整个
    #    文件判成二进制，然后只输出 "Binary file ... matches" —— 那一格看起来
    #    就像失败了，实际测试是过的。踩过一次，别再拿掉这个 -a。
    line=$(grep -a -E 'TESTS=' "$LOG" | tail -1 | sed 's/.*\*\* //; s/ *\*\*.*//')
    if [ -z "$line" ]; then
      echo "  $P/$M  ❌ 没跑起来 —— $(grep -a -iE 'error|not found' "$LOG" | head -1)"
    else
      echo "  $P/$M  $line"
    fi
    rm -rf "$B"
  done
done
