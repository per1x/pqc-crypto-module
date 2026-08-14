# pay_facfan.sh —— 验证「厂家设计 + 风扇」那一版 bitstream **网卡还活着**
#
# ============================================================================
# 【为什么这一步不能省】
# ============================================================================
# 这份 bitstream 是要写进 boot.scr、开机就装的。装上去之后如果 `eth0` 起不来，
# 结果不是"风扇没调好"，而是**板子失联** —— 而唯一的补救手段是串口，
# 那条线的 TX 方向本来就是坏的。
#
# 所以先用运行时装载把这件事问清楚：装上它、把驱动绑回去、把 IP 配回来，
# 然后**停在那儿等我从网络连进来**。连得上，就说明这份 bitstream 里的
# AXI Ethernet 是好的，可以放心交给 boot.scr。
#
# 无论结果如何，harness 收尾都会恢复出厂 bitstream，所以这一步本身是可退的。
#
# ⚠️ 这里故意**自己**做 rebind + 配 IP，而不是等 harness 收尾去做 ——
#    因为我要验的正是"在这份新 bitstream 之下网卡能不能用"，
#    等收尾恢复成出厂 bit 再配 IP，验的就是出厂 bit 了，等于没验。
#
# ⚠️⚠️ **自己绑了驱动，就必须自己解绑回去。**（这一条是断电换来的。）
#    第一版跑完直接返回，harness 的收尾于是在 eth0 驱动还绑着、AXI 主口
#    还活着的时候 fpgautil 重配 fabric —— 总线当场挂死，而 AXI 一挂那条
#    sysrq 看门狗也跑不起来，480 秒兜底没救回来，只能断电。
#    plharness.sh 的收尾现在会无条件先解绑一遍（双保险），但 payload
#    自己也要收干净：谁改的状态谁负责恢复。
D=/media/sd-mmcblk1p2/hsm
L=$D/facfan.log
: > $L

echo "载入 factory_fan.bit（厂家设计 + 风扇）" | tee -a $L
mkdir -p /lib/firmware
cp $D/factory_fan.bit /lib/firmware/ 2>/dev/null
fpgautil -b /lib/firmware/factory_fan.bit -f Full >> $L 2>&1
echo "rc=$? state=$(cat /sys/class/fpga_manager/fpga0/state)" >> $L
sync
sleep 3

echo "把 PL 驱动绑回去" >> $L
while read d drv; do
    echo $d > /sys/bus/platform/drivers/$drv/bind 2>/dev/null
done < $D/drvmap
sleep 3

busybox ip link set eth0 up 2>/dev/null
busybox ip addr add 192.168.50.174/24 dev eth0 2>/dev/null
sleep 2
echo "eth0: $(busybox ip -o addr show eth0 2>/dev/null | head -1)" >> $L
echo "PL 温度(AMS): $(cat /sys/bus/iio/devices/iio:device0/in_temp2_pl_temp_raw 2>/dev/null)" >> $L
sync

# 留一个窗口让我从网络连进来核实。240 秒之内 harness 的看门狗（480s）不会响。
echo "=== 网络窗口开启，240 秒 ===" >> $L
i=0
while [ $i -lt 24 ]; do
    echo "$(cut -d' ' -f1 /proc/uptime) 存活 PL温度raw=$(cat /sys/bus/iio/devices/iio:device0/in_temp2_pl_temp_raw 2>/dev/null)" >> $L
    sync
    sleep 10
    i=$((i+1))
done
echo "=== 窗口结束，解绑驱动再交回 harness ===" >> $L
# 自己绑的自己解 —— 理由见文件头那条断电换来的注释
for d in $(ls /sys/bus/platform/devices/ | grep -E "^8[0-9a-f]{7}\."); do
    if [ -e /sys/bus/platform/devices/$d/driver ]; then
        echo $d > /sys/bus/platform/devices/$d/driver/unbind 2>/dev/null
    fi
done
sleep 2
echo "=== 已解绑，交回 harness ===" >> $L
sync
