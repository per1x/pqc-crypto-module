#!/bin/sh
# try_mldsa.sh —— 带死人开关地试装「含 ML-DSA 的 bitstream + 新 daemon」
#
# ============================================================================
# 【为什么要死人开关】
# ============================================================================
# 这次换的是**整块 PL 加服务进程**：新 bitstream 里多了一个 AXI 从机（槽 6），
# daemon 也换成认识 ML-DSA 那三条 OP 的新版。任何一步坏掉的后果都是板子失联，
# 而失联之后**只能断电**（这块板的 reboot 命令不生效，见 hsmreboot.sh）。
#
# 所以这个壳的规矩是：**先自己往回退，再等人确认**。
#   装 → 自检 → 我从网络连进来摸确认文件 → 壳看到确认就退出保持现状；
#   300 秒内没等到确认 → 无条件退回原来的 bitstream 与原来的 daemon。
#
# 与 try_fanquiet.sh 同一个套路，差别是这次要连 daemon 一起回滚。
#
# ============================================================================
# 【三条不变量，抄自 fanquiet-init.sh，一条都不能省】
# ============================================================================
#  ① **重配 PL 之前必须先解绑 PL 驱动。** 带着活的 AXI 主口重配 fabric 会把
#     总线挂死，AXI 一挂连 sysrq 都写不进去 —— 栽过一次，代价是断电。
#  ② **fpgautil 返回 0 不等于装成功。** 只有 fpga_manager 的 state 是
#     "operating" 才算数（这条也栽过：返回 0、state 却是 unknown）。
#  ③ 装完必须**自检到能用**才算数，不能只看进程起来了。
set -u
D=/media/sd-mmcblk1p2/hsm
NEWBIT=$D/zu3eg_hsm_mldsa.bit
OLDBIT=$D/zu3eg_hsm.bit
NEWD=$D/pqchsm_fpgad.new
OKFILE=$D/mldsa_confirmed
LOG=$D/trymldsa.log
WAIT=300

say() { echo "$(cut -d' ' -f1 /proc/uptime) $*" >> $LOG; sync; }

: > $LOG
rm -f $OKFILE
say "=== 试装开始 ==="

[ -f "$NEWBIT" ] || { say "没有 $NEWBIT，什么都不做"; exit 0; }
[ -f "$NEWD" ]   || { say "没有 $NEWD，什么都不做"; exit 0; }

# 备份现役 daemon，回滚要用
cp -f $D/pqchsm_fpgad $D/pqchsm_fpgad.bak 2>/dev/null
say "现役 daemon 已备份到 pqchsm_fpgad.bak"

load_bit() {   # load_bit <bit 文件>
    B="$1"
    # ① 先解绑
    : > $D/drvmap.try
    for d in $(ls /sys/bus/platform/devices/ 2>/dev/null | grep -E "^8[0-9a-f]{7}\."); do
        if [ -e /sys/bus/platform/devices/$d/driver ]; then
            drv=$(readlink /sys/bus/platform/devices/$d/driver | sed 's#.*/##')
            echo "$d $drv" >> $D/drvmap.try
            echo $d > /sys/bus/platform/devices/$d/driver/unbind 2>/dev/null
        fi
    done
    say "已解绑 $(wc -l < $D/drvmap.try) 个 PL 驱动"
    sleep 1
    cp -f "$B" /lib/firmware/ 2>/dev/null
    fpgautil -b /lib/firmware/$(basename $B) -f Full >> $LOG 2>&1
    sleep 2
    # ② fpgautil 的返回值不作数，只认 fpga_manager 的 state
    st=$(cat /sys/class/fpga_manager/fpga0/state 2>/dev/null)
    say "装 $(basename $B) 之后 fpga_manager state = $st"
    [ "$st" = "operating" ] || return 1
    sort -u $D/drvmap.try 2>/dev/null | while read d drv; do
        echo $d > /sys/bus/platform/drivers/$drv/bind 2>/dev/null
    done
    sleep 2
    return 0
}

rollback() {
    say "!!! 回滚：退回 $(basename $OLDBIT) 与原 daemon"
    killall pqchsm_fpgad 2>/dev/null; sleep 1
    load_bit "$OLDBIT"
    cp -f $D/pqchsm_fpgad.bak $D/pqchsm_fpgad 2>/dev/null
    chmod +x $D/pqchsm_fpgad
    ( $D/pqchsm_fpgad >> $D/hsm-daemon.log 2>&1 & )
    sleep 3
    say "回滚完成：$(busybox ip -o addr show eth1 2>/dev/null | head -1)"
    sync
}

# ---- 停旧 daemon、装新 bitstream ----
killall pqchsm_fpgad 2>/dev/null
sleep 1
if ! load_bit "$NEWBIT"; then
    say "新 bitstream 没装上（state 不是 operating）"
    rollback
    exit 1
fi

# ---- 换 daemon 并起来 ----
cp -f $NEWD $D/pqchsm_fpgad && chmod +x $D/pqchsm_fpgad
( $D/pqchsm_fpgad >> $D/hsm-daemon.log 2>&1 & )
sleep 4

# ---- ③ 自检：网络在、daemon 在、而且**真能读到密码核** ----
ip=$(busybox ip -o addr show eth1 2>/dev/null | grep -o '192\.168\.50\.[0-9]*' | head -1)
[ -n "$ip" ] || { say "eth1 没地址"; rollback; exit 1; }
# ⚠️ **这块板上没有 pgrep**（busybox 没编进去）。第一版这里写的是
#    `pgrep pqchsm_fpgad >/dev/null || 回滚` —— 命令不存在 → 非零退出 →
#    走了回滚分支。于是新 bitstream 明明装好了（state=operating）、daemon 也
#    起来了，却被**自检本身的 bug** 判成失败回滚掉。
#    教训：自检里每一条命令都要先确认板上真有，否则"检查"变成了"必然失败"。
ps | grep -q "[p]qchsm_fpgad" || { say "daemon 没起来"; rollback; exit 1; }
say "自检：eth1=$ip，daemon 在跑"

# ---- 自检第二层：**真读到 ML-DSA 核**才算装对 ----
# 只看"进程在跑"是不够的：旧 bitstream 上这个 daemon 一样能起来（它的启动
# 自检读的是 TRNG，那个两版都有）。要证明**装的是含 ML-DSA 的那一版**，
# 必须读到槽 6 那个核。daemon 的版本串里带各核的 VERSION，没有 ML-DSA 就没有那一项。
sleep 1
info=$($D/sdf_demo 2>/dev/null | head -20)
echo "$info" >> $LOG
# ⚠️ 判据是 **mldsa= 后面非零**，不是"字符串里有 mldsa 这几个字母"。
#    新 daemon 无论装的哪一版 bitstream 都会打印 mldsa=0x........，
#    而槽 6 不存在时经防火墙读回来的是 **0**（RAZ/WI）。
#    只 grep 'mldsa' 的话，装着旧位流也能通过 —— 那种自检等于没有。
if echo "$info" | grep -q 'mldsa=0x00000000'; then
    say "mldsa VERSION 读回 0 —— 装上的不是含 ML-DSA 的那一版"
    rollback
    exit 1
fi
echo "$info" | grep -q 'mldsa=0x' || { say "版本串里没有 mldsa 项（daemon 是旧的？）"; rollback; exit 1; }
say "自检：mldsa VERSION 非零，槽 6 的核真在硅上"

i=0
while [ $i -lt $((WAIT/10)) ]; do
    if [ -f $OKFILE ]; then
        say "已确认 —— 保持现状，不回滚"
        sync
        exit 0
    fi
    sleep 10
    i=$((i+1))
done

say "$WAIT 秒没等到确认"
rollback
exit 1
