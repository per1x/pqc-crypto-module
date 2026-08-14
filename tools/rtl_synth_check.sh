#!/usr/bin/env bash
# RTL 可综合性检查（Yosys，厂商中立）
#
# lint 与仿真都看不出的一类问题，在这里才会暴露：**仿真语义与综合语义的分歧**。
# 典型例子是把同步条件混进异步复位分支
#
#     always @(posedge clk or negedge rst_n)
#         if (!rst_n || soft_reset) ...      // soft_reset 不在敏感表里
#
# 仿真器照着写法执行，结果看起来完全正确；综合工具看到的却是"一个不在敏感表里
# 的复位条件"，要么报错，要么综合出与仿真不同的电路。Yosys 会直接判为错误。
#
# 第二件事是把存储摊成寄存器。Yosys 遇到无法映射成 RAM 的存储会打印
# "Replacing memory ... with list of registers"，那意味着面积按位翻倍地涨 ——
# 一块 16 KiB 的缓冲摊开就是 13 万个触发器，比整片 XC7Z020 的触发器还多。
# 本脚本把这类提示单独列出来，因为它是"看着能跑、其实放不下"的主要来源。
#
# 用的是 Yosys 的通用综合流程，不针对任何厂商器件；器件相关的资源估算在
# 板级分支里做。
#
# 前置：yosys（brew install yosys）
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# 文件名之间必须用空格连接：yosys 的 -p 脚本按行拆命令，换行会被当成新命令
# 风扇温控在 hardware/rtl/ 之外（hardware/platform/fan_ctrl/），一样要过可综合性检查。
RTL_FILES=$(find "$ROOT/hardware/rtl" "$ROOT/hardware/platform/fan_ctrl" -name '*.v' 2>/dev/null | sort | tr '\n' ' ')

command -v yosys >/dev/null 2>&1 || { echo "SKIP: 没装 yosys（brew install yosys）"; exit 0; }

fail=0
n=0
LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

# 允许存在组合环的模块。
# ring_osc 是反相器环 —— 组合环**就是**它的工作原理，环被打断就没有熵了。
# 综合侧靠 hardware/syn/constraints/trng_ro.xdc 里的 DONT_TOUCH +
# ALLOW_COMBINATORIAL_LOOPS + LUTLP-1 降级三条放行；Yosys 没有对应开关，
# `check -assert` 一律把逻辑环判成错误，所以这几个模块要单独走一条路。
#
# 单独那条路**不是"跳过检查"**：仍然跑 check，只是把"ring_osc 里的逻辑环"
# 这一条从问题清单里划掉，剩下任何一条问题照样算失败。所以这几个模块的
# 其它可综合性问题不会被这条例外掩盖。
#
# trng_source / trng_top / trng_axi 在列，是因为它们例化了 ring_osc，
# 环被逐层带上来。
LOOP_OK=" ring_osc trng_source trng_top trng_axi "

# 板级顶层例化 Xilinx 的 PS IP（zynq_ultra_ps_e_0）与 BUFGCE_DIV 原语，
# Yosys 手上没有这两个东西，报 "module not found" 是必然的。
# 这**不是**缺陷：板级顶层按定义就是绑器件的那一层，算法核那边仍然是
# 可移植的纯 RTL（整个仓库的厂商原语只出现在这一个文件里）。
# 它的正确性由 Vivado 的实现流程本身保证 —— 跑不通就出不了 bitstream。
#
# fan_sysmon 也在列：它例化 SYSMONE4（UltraScale+ 的系统监测原语）。
# 那个文件里**只有**原语和一根连线，真正的时序逻辑被特意拆到 sysmon_drp.v，
# 所以"厂商原语"这件事只污染一个文件，DRP 状态机照样进 Yosys 和 cocotb。
VENDOR_TOP=" zu3eg_hsm_top fan_sysmon "

echo "Yosys 可综合性检查（通用综合流程）"
while read -r m; do
  if [[ "$VENDOR_TOP" == *" $m "* ]]; then
    printf '  – %-24s (板级顶层，含厂商原语，由 Vivado 流程验证)\n' "$m"
    continue
  fi
  n=$((n + 1))
  if [[ "$LOOP_OK" == *" $m "* ]]; then
    yosys -q -p "
      read_verilog $RTL_FILES
      hierarchy -top $m -check
      proc
      opt
      memory
      opt
      check
      stat
    " >"$LOG" 2>&1
    # check 报的每个问题都以 "Warning: found ..." 开头；只放行 ring_osc 的环。
    # 带参数例化后 Yosys 会把模块改名成 `$paramod$<hex>\ring_osc`（STAGES 每
    # 个实例不同，所以 trng_source 下面有 8 个不同的哈希），改名形式一并放行；
    # 但前缀写死成 $paramod$<hex>\，免得 my_ring_osc 之类也被顺手放过。
    others=$(grep -E '^Warning: found ' "$LOG" \
             | grep -vE 'logic loop in module (\$paramod\$[0-9a-f]+\\)?ring_osc:' \
             || true)
    if [[ -n "$others" ]] || grep -q '^ERROR' "$LOG"; then
      printf '  ✗ %-24s\n' "$m"
      { grep -E '^ERROR' "$LOG"; echo "$others"; } | head -5 | sed 's/^/      /'
      fail=1
      continue
    fi
    printf '  ✓ %-24s (组合环已按环振例外放行)\n' "$m"
    continue
  fi

  if ! yosys -q -p "
    read_verilog $RTL_FILES
    hierarchy -top $m -check
    proc
    opt
    memory
    opt
    check -assert
    stat
  " >"$LOG" 2>&1; then
    printf '  ✗ %-24s\n' "$m"
    grep -E "^ERROR|^Warning" "$LOG" | head -5 | sed 's/^/      /'
    fail=1
    continue
  fi
  printf '  ✓ %-24s\n' "$m"
done < <(grep -h '^module ' $RTL_FILES | sed -E 's/^module ([A-Za-z0-9_]+).*/\1/' | sort)

# 摊成寄存器的存储：整设计跑一遍就够，逐模块跑得到的是同一份清单。
# 这不是错误，但要看得见 —— 它决定了面积是按位涨还是按块涨。
echo
echo "映射不成 RAM、摊成寄存器的存储"
yosys -q -p "read_verilog $RTL_FILES
  hierarchy -top pqc_accel_axi
  proc
  opt
  memory
" 2>&1 | sed -n 's/^Warning: Replacing memory \\\([A-Za-z0-9_]*\) .*See \(.*\)$/  \1  <- \2/p' \
  | sed 's#'"$ROOT"'/##' | sort -u | head -20

# 下面这几条是**故意留着**的，不要照着 ram_dp 去改：
#
#   keccak_f1600 的 A / Anext / Ath / B / C / D
#     A 是 1600 bit 置换状态，每一轮 25 条 lane 要同时读、同时写 ——
#     BRAM 只有两个口，装不下这种全并行访问，它本来就该是触发器。
#     B/C/D/Ath/Anext 是轮内的组合中间量，Yosys 把数组一律当 memory 报，
#     综合出来是纯组合线网，不占存储。
#
#   sync_fifo 的 mem
#     实例只有 TRNG 那一个，32 bit × FIFO_DEPTH，一共 1 Kbit 上下，
#     摊成触发器的代价可以忽略。更要紧的是它是 FWFT（首字直通）：
#     rd_valid 拉高的同一拍 rd_data 就得有效，因为 AXI4-Lite 要在同一次
#     读事务里返回数据。换成同步读的 BRAM 就得加一拍，那要动 trng_top
#     和 AXI 读通路 —— 为 1 Kbit 付这个代价不值。
#   key_vault 的 keys / fill：**这里是故意摊成寄存器的**，理由与上面两处不同 ——
#     密钥仓要求"擦除一拍完成"。BRAM 只能逐地址写零，擦除过程中存在一个
#     "擦了一半"的窗口，掉电或复位卡在那个窗口里残留就还在片上。
#     2048 bit 摊成 2048 个 FF 是这颗片子的 1.5%，为这条性质付得起。
#     判断标准仍然是 ram_dp.v 那条：**看规模**。这里的规模允许，S3 那块 16 KiB
#     的缓冲不允许。
#   aes_core 的 rkey / wbuf / sb_in、sm4_core 的 rk、sm3_core 的 w
#     都是几十个字的小阵列，而且**每一拍都要多口并行访问**：15 个轮密钥要
#     按轮号整块选出、16 个 S 盒同时查、SM3 的窗口一拍里要读 w[0]/w[3]/w[7]/
#     w[10]/w[13] 五个位置。BRAM 只有两个口，装不下这种访问模式 ——
#     和 keccak 的状态是同一类，本来就该是触发器。
echo "  （以上都是预期之内：keccak 状态与对称核的小阵列必须多口并行访问，"
echo "    sync_fifo 要 FWFT 时序，key_vault 要一拍擦完整个密钥仓。"
echo "    判断标准见 hardware/rtl/common/ram_dp.v。）"

echo
if [ "$fail" -eq 0 ]; then
  echo "全部可综合（$n 个模块）"
else
  echo "有模块无法综合"
fi
exit $fail
