#!/bin/sh
# fanquiet-init.sh —— 开机把带温控的 bitstream 装进 PL（风扇不再满速）
#
# ============================================================================
# 【为什么走 Linux init，而不是 boot.scr 里的 fpga loadb】
# ============================================================================
# 试过 boot.scr 那条路，U-Boot 挂死在 `fpga loadb` 上，板子起不来，要人拔 SD 卡。
# 命令本身是存在的（U-Boot 命令表里有 "Load device from bitstream buffer"），
# 但它在这块板上会挂 —— 而 boot.scr 一旦挂住**没有任何兜底**：黄金 BOOT.BIN
# 的 FSBL 不武装看门狗，板子也没有 JTAG。
#
# Linux init 这条路不一样：boot.scr 里已经有**三振退备份镜像**的机制
# （boot_try1/2/3 → use_backup → image_backup.ub），而这个脚本是随 image.ub
# 一起烤进去的，所以它把板子搞到起不来的话，**第三次启动会自动退回备份镜像**，
# 不需要人插手。代价只是风扇从上电到 Linux 起来那二十来秒仍是出厂状态 ——
# 对"太吵"这个诉求没有任何影响。
#
# ============================================================================
# 【三条不变量】
# ============================================================================
#  ① **重配 PL 之前必须先解绑 PL 驱动。** 带着活的 AXI 主口重配 fabric 会把
#     总线挂死，而 AXI 一挂，连 sysrq 都写不进去（栽过一次，要断电）。
#  ② **装完必须自检网络，起不来就自己退回出厂 bitstream。** 这个脚本会在
#     每次启动跑，一旦它把网卡弄没了而 Linux 又启动成功，三振机制**不会**
#     触发（那机制看的是"有没有进 Linux"），板子就永久失联了。自愈是必须的。
#  ③ 找不到 bitstream 就**安静退出**，什么都不做 —— 删掉那个文件就是回退手段。
set -u
D=/media/sd-mmcblk1p2/hsm
BIT=$D/fanquiet.bit
LOG=$D/fanquiet-init.log
IP=192.168.50.174/24

say() { echo "$(cut -d' ' -f1 /proc/uptime) $*" >> $LOG; sync; }

[ -f "$BIT" ] || exit 0          # ③ 没有就什么都不做
: > $LOG
say "开始：装 $BIT"

# ---- ① 先解绑所有 PL 驱动 ----
: > $D/drvmap.init
for d in $(ls /sys/bus/platform/devices/ 2>/dev/null | grep -E "^8[0-9a-f]{7}\."); do
    if [ -e /sys/bus/platform/devices/$d/driver ]; then
        drv=$(readlink /sys/bus/platform/devices/$d/driver | sed 's#.*/##')
        echo "$d $drv" >> $D/drvmap.init
        echo $d > /sys/bus/platform/devices/$d/driver/unbind 2>/dev/null
    fi
done
say "已解绑 $(wc -l < $D/drvmap.init) 个 PL 驱动"
sleep 1

rebind() {
    sort -u $D/drvmap.init | while read d drv; do
        echo $d > /sys/bus/platform/drivers/$drv/bind 2>/dev/null
    done
    sleep 3
    busybox ip link set eth0 up 2>/dev/null
    busybox ip addr add $IP dev eth0 2>/dev/null
    sleep 2
}

net_ok() {
    busybox ip -o addr show eth0 2>/dev/null | grep -q "inet 192.168.50.174"
}

# ---- 装 ----
mkdir -p /lib/firmware
cp $BIT /lib/firmware/fanquiet.bit 2>/dev/null
fpgautil -b /lib/firmware/fanquiet.bit -f Full >> $LOG 2>&1
say "fpgautil rc=$? state=$(cat /sys/class/fpga_manager/fpga0/state 2>/dev/null)"
sleep 2
rebind
say "网络：$(busybox ip -o addr show eth0 2>/dev/null | head -1)"

# ---- ② 自检 + 自愈 ----
if net_ok; then
    say "成功：温控 bitstream 已生效，网络正常"
    exit 0
fi

say "网络没起来 —— 退回出厂 bitstream"
for d in $(ls /sys/bus/platform/devices/ 2>/dev/null | grep -E "^8[0-9a-f]{7}\."); do
    [ -e /sys/bus/platform/devices/$d/driver ] && \
        echo $d > /sys/bus/platform/devices/$d/driver/unbind 2>/dev/null
done
sleep 1
cp $D/factory.bit /lib/firmware/ 2>/dev/null
fpgautil -b /lib/firmware/factory.bit -f Full >> $LOG 2>&1
sleep 2
rebind
if net_ok; then
    say "已退回出厂 bitstream，网络恢复（风扇回到满速，但板子还在）"
else
    say "退回之后网络仍然没起来 —— 只能靠三振退备份镜像了"
fi
exit 0
