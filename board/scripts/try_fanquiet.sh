#!/bin/sh
# try_fanquiet.sh —— 「限时确认否则回滚」地试跑 fanquiet-init.sh
#
# 这个壳只在**运行时验证**用，不会被烤进 image.ub。
#
# 为什么要它：fanquiet-init.sh 自己带自愈（装完自检网络，起不来就退回出厂
# bitstream），但那只覆盖"网卡没起来"这一种坏法。还有一种坏法是**整条 AXI
# 挂死**，那时脚本自己也跑不下去了。所以外面再套一层：
#
#   跑脚本 → 我从网络连进来 → 摸一个确认文件 → 壳看到确认就退出；
#   240 秒内没等到确认，就无条件退回出厂 bitstream + 恢复网络。
#
# 于是这次试跑的最坏代价是"240 秒后自动恢复原状"，而不是要人断电。
D=/media/sd-mmcblk1p2/hsm
OK=$D/fanquiet_confirmed
LOG=$D/tryfan.log
: > $LOG
rm -f $OK

echo "=== 试跑开始 $(date) ===" >> $LOG
sh $D/fanquiet-init.sh
echo "--- init 脚本返回 rc=$? ---" >> $LOG
cat $D/fanquiet-init.log >> $LOG 2>/dev/null
sync

i=0
while [ $i -lt 24 ]; do
    if [ -f $OK ]; then
        echo "已确认（我从网络连进来并摸了确认文件）—— 保持现状不回滚" >> $LOG
        sync
        exit 0
    fi
    sleep 10
    i=$((i+1))
done

echo "240 秒没等到确认 —— 无条件回滚" >> $LOG
for d in $(ls /sys/bus/platform/devices/ 2>/dev/null | grep -E "^8[0-9a-f]{7}\."); do
    [ -e /sys/bus/platform/devices/$d/driver ] && \
        echo $d > /sys/bus/platform/devices/$d/driver/unbind 2>/dev/null
done
sleep 1
cp $D/factory.bit /lib/firmware/ 2>/dev/null
fpgautil -b /lib/firmware/factory.bit -f Full >> $LOG 2>&1
sleep 2
sort -u $D/drvmap.init 2>/dev/null | while read d drv; do
    echo $d > /sys/bus/platform/drivers/$drv/bind 2>/dev/null
done
sleep 3
busybox ip link set eth0 up 2>/dev/null
busybox ip addr add 192.168.50.174/24 dev eth0 2>/dev/null
echo "回滚完成：$(busybox ip -o addr show eth0 2>/dev/null | head -1)" >> $LOG
sync
