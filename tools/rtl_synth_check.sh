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
RTL_FILES=$(find "$ROOT/hardware/rtl" -name '*.v' | sort | tr '\n' ' ')

command -v yosys >/dev/null 2>&1 || { echo "SKIP: 没装 yosys（brew install yosys）"; exit 0; }

fail=0
n=0
LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

echo "Yosys 可综合性检查（通用综合流程）"
while read -r m; do
  n=$((n + 1))
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
echo "  （以上都是预期之内：keccak 状态必须全并行访问，sync_fifo 要 FWFT 时序。"
echo "    判断标准见 hardware/rtl/common/ram_dp.v 的注释。）"

echo
if [ "$fail" -eq 0 ]; then
  echo "全部可综合（$n 个模块）"
else
  echo "有模块无法综合"
fi
exit $fail
