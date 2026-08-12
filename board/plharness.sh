#!/bin/sh
# plharness.sh <payload.sh> —— 所有碰 PL / 网卡的动作都必须经过这里
#
# 三次断网、三次要人断电，全部源于同一个错误：把 unbind 网卡的命令放在
# **前台 SSH 命令**里跑 —— 第一个被解绑的就是 eth0，SSH 当场断，后面的
# rebind 与恢复 IP 再也不会执行。
#
# 所以把「脱离终端、收尾必定恢复网络、带看门狗」这三件事固化在这里，
# 上层只写 payload，不再手敲这些步骤。
#
# ============================================================================
# 【收尾必须先解绑再重配 —— 又一次断电换来的】
# ============================================================================
# 第一版的收尾直接 fpgautil 装回出厂 bit，默认"payload 跑完时驱动还是解绑的"。
# 但有的 payload **自己会把驱动绑回去**（例如要验"新 bitstream 下网卡能不能
# 用"，就必须先绑上再从网络连进来）。于是收尾就变成了：**在 eth0 驱动还绑着、
# AXI 主口还活着的时候重配 fabric** —— 总线当场挂死。
#
# 更糟的是 AXI 一挂，那条 sysrq 看门狗自己也跑不起来（它要写 /proc，而进程
# 调度还活着但总线不通），480 秒兜底救不回来，只能断电。
#
# 所以收尾不再假设 payload 留下的状态：**无条件先解绑一遍，再重配**。
# 重复解绑一个已经解绑的设备只是一条被忽略的错误，代价是零。
D=/media/sd-mmcblk1p2/hsm
L=$D/harness.log
say() { echo "$@" >> $L; sync; }

unbind_all() {
    for d in $(ls /sys/bus/platform/devices/ | grep -E "^8[0-9a-f]{7}\."); do
        if [ -e /sys/bus/platform/devices/$d/driver ]; then
            drv=$(readlink /sys/bus/platform/devices/$d/driver | sed 's#.*/##')
            echo "$d $drv" >> $D/drvmap
            echo $d > /sys/bus/platform/devices/$d/driver/unbind 2>/dev/null
        fi
    done
}

: > $L; sync
say "=== harness 开始 $(date) payload=$1 ==="

# 看门狗：真卡死就重启（key 现在是持久的，重启代价可接受）
# ⚠️ 它救不了 AXI 总线级的挂死 —— 那种情况下写 /proc 也不通。所以它是
#    兜底，不是许可证：别拿"反正有看门狗"当理由去做会挂总线的操作。
( sleep 480; echo "WATCHDOG 480s" >> $L; sync; echo b > /proc/sysrq-trigger ) &
WD=$!

# 记录并解绑所有 PL 驱动
: > $D/drvmap
unbind_all
say "已解绑 $(wc -l < $D/drvmap) 个 PL 驱动"
sync; sleep 2

# ---- 跑 payload ----
say "--- payload 开始 ---"
sh "$1" >> $L 2>&1
say "--- payload 结束 rc=$? ---"

# ---- 收尾：无论如何都要把网络弄回来 ----
# **先解绑再重配。** payload 可能自己把驱动绑回去了，带着活的 AXI 主口
# 重配 fabric 会挂总线（见文件头）。这里不作任何假设。
say "恢复：先无条件解绑（payload 可能自己绑回去过）"
unbind_all
sleep 2

say "恢复：装回厂家 bitstream"
cp $D/factory.bit /lib/firmware/ 2>/dev/null
fpgautil -b /lib/firmware/factory.bit -f Full >> $L 2>&1
sleep 2
say "恢复：重新 bind 驱动"
# drvmap 可能被 unbind_all 追加了重复行，bind 两次的第二次只是一条错误
sort -u $D/drvmap | while read d drv; do
    echo $d > /sys/bus/platform/drivers/$drv/bind 2>/dev/null
done
sleep 3
say "恢复：eth0 地址"
busybox ip link set eth0 up 2>/dev/null
busybox ip addr add 192.168.50.174/24 dev eth0 2>/dev/null
sleep 2
say "eth0: $(busybox ip -o addr show eth0 2>/dev/null | head -1)"
say "=== harness 结束 ==="
kill $WD 2>/dev/null
sync
