#!/bin/sh
# trng_restart_plreload.sh —— 用 **PL 重配** 当 restart 的 SP 800-90B 重启矩阵
#
#   sh trng_restart_plreload.sh [行数] [每行样本数] [输出文件]
#   默认 200 1000 /media/sd-mmcblk1p2/hsm/restart_plreload.bin
#
# ============================================================================
# 【它比 trngrestart 的 ZEROIZE 强在哪，又仍然弱在哪】
# ============================================================================
# SP 800-90B §3.1.4.3 的 restart 指的是**噪声源重新上电**。这块板上做不到
# 1000 次真 POR（没有能自动化拔电的台子），所以只能找代理。两级代理：
#
#   ① TRNG-ZEROIZE（board/src/trngrestart.c）：写 CTRL 的 ZEROIZE，环振随
#      enable 停振再起振，健康检测重跑。**fabric 没动**，触发器的初值、
#      布线、环振的物理实例都还是上一次那份。
#
#   ② **PL 重配（本脚本）**：整块 fabric 重新配置 —— 环振是被**重新实例化**
#      出来的，所有触发器回到配置位流里的初值，配置逻辑重新走一遍。
#      这比 ① 接近"上电"得多，因为它连"这一份电路是怎么建立起来的"都重来了。
#
# ⚠️ **它仍然不是 POR**：芯片没断电，电源轨没重新建立，结温没回到室温，
#    PS 侧一直在跑。覆盖不到的正是"上电瞬态"与"冷启动温度"这两类。
#    这一条会写进输出文件头，和数据一起走。
#
# ⚠️ **行数默认 200 而不是 1000**：每行要重配一次 PL（约 3 秒），1000 行要
#    一小时以上，而这块板是共享的演示机。200 行的**列估计统计功效低于**
#    标准建议的 1000 行 —— 报数时必须写明，不能拿它冒充 1000 行的结论。
#    要跑满 1000 行就显式传参，别改默认值。
#
# ============================================================================
# 【三条纪律】
# ============================================================================
#  ① 每次重配后**只认 fpga_manager 的 state**，不看 fpgautil 的退出码
#     （它装载失败照样返回 0，这块板上栽过）。
#  ② 采样程序自己也会先查 state 再发第一笔 SMC —— 两道，别去掉任何一道。
#  ③ 跑之前把 daemon 停掉：它也在驱动这些核，两边交错采到的不是干净样本。
#     跑完由调用方自己决定要不要起回来（本脚本不替你起，免得掩盖失败）。
set -u
D=/media/sd-mmcblk1p2/hsm
ROWS="${1:-200}"
COLS="${2:-1000}"
OUT="${3:-$D/restart_plreload.bin}"
BIT=$D/zu3eg_hsm_dev.bit
LOG=$D/restart_plreload.log

say() { echo "[$(date '+%H:%M:%S')] $*" | tee -a "$LOG"; sync; }

: > "$LOG"
say "=== PL 重配重启矩阵：${ROWS} 行 × ${COLS} 样本 → $OUT ==="

if ps | grep -q "[p]qchsm_fpgad"; then
    say "!!! daemon 还在跑 —— 先 killall pqchsm_fpgad 再来，否则采到的样本不干净"
    exit 1
fi
[ -f "$BIT" ] || { say "没有 $BIT"; exit 1; }

# 文件头由本脚本写一次；每一行由 trngrestart -A 追加（它那时不写头）。
{
    echo "#SP800-90B-restart-matrix rows=$ROWS cols=$COLS"
    echo "#restart=PL-RECONFIGURATION (whole fabric reprogrammed; the ring"
    echo "# oscillator is re-instantiated and every flop returns to its"
    echo "# bitstream init value. Closer to power-on than a TRNG zeroize, but"
    echo "# still NOT a power-on reset: the chip stayed powered, the supplies"
    echo "# were not re-established, and the die temperature did not return to"
    echo "# ambient.)"
    echo "#tap=RAW (pre-conditioning, src_valid/src_bit)"
    echo "#sampling=AFTER startup_done AND draining 128 raw words (operational)"
    echo "#rows=$ROWS is BELOW the 1000 that SP 800-90B suggests; the per-column"
    echo "# estimate therefore has less statistical power. Do not report this as"
    echo "# a 1000-row result."
    echo "#binary payload follows: rows x ceil(cols/8) bytes, MSB-first within each byte"
} > "$OUT"

i=0
fail=0
while [ "$i" -lt "$ROWS" ]; do
    fpgautil -b "$BIT" -f Full >> "$LOG" 2>&1
    sleep 1
    ST=$(cat /sys/class/fpga_manager/fpga0/state 2>/dev/null)
    if [ "$ST" != "operating" ]; then
        say "!!! 第 $i 次重配之后 state=$ST —— 停下，不再碰总线"
        fail=1
        break
    fi
    if ! $D/trngrestart 1 "$COLS" "$OUT" -a -A >> "$LOG" 2>&1; then
        say "!!! 第 $i 行采集失败，停下"
        fail=1
        break
    fi
    i=$((i + 1))
    [ $((i % 20)) -eq 0 ] && say "  已采 $i / $ROWS 行"
done

say "=== 结束：实际采到 $i 行（失败标志 $fail）==="
sync
exit $fail
