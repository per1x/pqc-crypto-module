#!/bin/sh
# pl_deadman.sh —— 装载密码位流，带「死人开关」自动回滚
#
#   sh pl_deadman.sh <bitstream> [宽限秒数]
#
# ============================================================================
# 【和 plharness.sh 的分工】
# ============================================================================
# plharness.sh 是**做实验**用的：跑完 payload 无条件装回厂家位流。
# 那正是它该做的 —— 实验的默认结局是"回到已知好的状态"。
#
# 但演示形态要的是**留在密码位流里**：网络、daemon、密码核同时在。
# 用 harness 做不到，它一定会把 PL 换回去。
#
# 所以这个脚本的默认结局相反：**留下**。安全性由死人开关提供 ——
#
#   装载 → 等最多 N 秒 → 期间有人 touch $KEEP 就留下，超时就回滚。
#
# 谁来 touch？**从 eth1 SSH 进来的人。** 于是"能不能留下"这个判断
# 恰好等价于"换了位流之后网络还通不通" —— 判据和目的是同一件事，
# 不需要额外的健康检查，也就不会出现"检查通过了但其实连不上"。
#
# ============================================================================
# 【为什么 eth1 不在解绑名单里】
# ============================================================================
# unbind 只扫 /sys/bus/platform/devices 里 `8xxxxxxx.` 开头的（PL 地址空间）。
# eth0 是 `80000000.ethernet`，在 PL 里，会被解绑而且随位流消失；
# eth1 是 `ff0e0000.ethernet`，PS 的硬核 GEM，地址不在那个范围，
# **碰都不会碰到**。这就是网络能穿过 PL 重配活下来的全部原因。
set -u
BIT="$1"
GRACE="${2:-180}"
D=/media/sd-mmcblk1p2/hsm
L=$D/deadman.log
KEEP=$D/PL_KEEP
say() { echo "[$(date '+%H:%M:%S')] $*" >> $L; sync; }

rm -f "$KEEP"
: > $L; sync
say "=== 开始 bit=$BIT 宽限=${GRACE}s ==="
say "eth1 现状: $(busybox ifconfig eth1 2>/dev/null | grep 'inet addr' | head -1)"

# ---- 解绑 PL 驱动 ----
# 带活的 AXI 主口重配 fabric 会挂总线，而总线一挂，sysrq 看门狗自己也写不了
# /proc，只能断电。所以这一步不是礼貌，是硬前提。
: > $D/drvmap_dm
for d in $(ls /sys/bus/platform/devices/ | grep -E "^8[0-9a-f]{7}\."); do
    if [ -e /sys/bus/platform/devices/$d/driver ]; then
        drv=$(readlink /sys/bus/platform/devices/$d/driver | sed 's#.*/##')
        echo "$d $drv" >> $D/drvmap_dm
        echo $d > /sys/bus/platform/devices/$d/driver/unbind 2>/dev/null
    fi
done
say "已解绑 $(wc -l < $D/drvmap_dm) 个 PL 驱动（eth0 在其中，eth1 不在）"
sleep 2

# ---- 装载 ----
#
# ⚠️ 两个坑，都是这里踩出来的：
#
# ① **直接传 SD 上的原路径，别自己先拷进 /lib/firmware。**
#    fpgautil 里就三行（strings 出来的）：
#        mkdir -p /lib/firmware ; cp %s /lib/firmware ; rm /lib/firmware/%s
#    也就是说搬运是它自己的事。你要是先把文件拷进 /lib/firmware、再把那个
#    路径传给它，它执行的就是 `cp /lib/firmware/x.bit /lib/firmware` ——
#    源和目标同一个文件。
#
#    实测这一步**不致命**：busybox cp 报 "are the same file" 就退出，
#    不动文件，而文件已经在它要的位置上，所以装载照样成功（plharness.sh 的
#    回滚段就是这个写法，验过是好的）。但它会往日志里塞一条像是出了事的
#    错误行，而真正出事的时候长得也差不多 —— 省掉这层歧义，值。
#
# ② **fpgautil 装载失败照样 return 0。** 踩到的那次它明明打了
#    "BIN FILE loading through FPGA manager failed"，`$?` 仍然是 0。
#    唯一说实话的是 /sys/class/fpga_manager/fpga0/state ——
#    成功是 "operating"，失败是 "firmware request error" 之类。
#    **所以判据一律用 state，不用退出码。**
fpgautil -b "$BIT" -f Full >> $L 2>&1
sleep 2
ST=$(cat /sys/class/fpga_manager/fpga0/state 2>/dev/null)
say "装载后 state=$ST（退出码不看，见上）"
if [ "$ST" != "operating" ]; then
    say "!!! 装载失败（state=$ST），不等宽限期，立刻回滚"
    GRACE=0
fi
say "eth1 载入后: carrier=$(cat /sys/class/net/eth1/carrier 2>/dev/null) $(busybox ifconfig eth1 2>/dev/null | grep 'inet addr' | head -1)"

# ---- 死人开关 ----
say "等 ${GRACE}s：从 eth1 连进来 touch $KEEP 就留下，否则回滚"
i=0
while [ $i -lt "$GRACE" ]; do
    if [ -f "$KEEP" ]; then
        say "=== 收到 KEEP，保留密码位流。结束 ==="
        exit 0
    fi
    sleep 2
    i=$((i + 2))
done

# ---- 超时回滚 ----
# 走到这里意味着：换了位流之后没人能连进来。原因可能是 eth1 也断了、
# 也可能是别的 —— 不重要，重要的是**板子必须自己回到能连的状态**，
# 不能要人断电。
say "!!! 超时无人确认，回滚到厂家位流"
for d in $(ls /sys/bus/platform/devices/ | grep -E "^8[0-9a-f]{7}\."); do
    [ -e /sys/bus/platform/devices/$d/driver ] && \
        echo $d > /sys/bus/platform/devices/$d/driver/unbind 2>/dev/null
done
sleep 2
fpgautil -b $D/factory.bit -f Full >> $L 2>&1
sleep 2
say "回滚装载后 state=$(cat /sys/class/fpga_manager/fpga0/state 2>/dev/null)"
sort -u $D/drvmap_dm | while read d drv; do
    echo $d > /sys/bus/platform/drivers/$drv/bind 2>/dev/null
done
sleep 3
busybox ip link set eth0 up 2>/dev/null
busybox ip addr add 192.168.50.174/24 dev eth0 2>/dev/null
say "回滚完成 eth0: $(busybox ip -o addr show eth0 2>/dev/null | head -1)"
say "=== 结束（已回滚）==="
