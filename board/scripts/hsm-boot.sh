#!/bin/sh
# hsm-boot.sh —— 上电自动进入「演示就绪」：网络 + 密码位流 + daemon
#
# 由 initramfs 里的 /etc/init.d/hsm-boot 在 rc5 阶段调起。**逻辑全在这个文件里，
# 而这个文件在 SD 卡上** —— 镜像里那个只有五行（有就跑、没有就算）。
# 于是改行为不用重出 image.ub，改这个文件就行；出问题也只要删掉它。
#
# ============================================================================
# 【铁律：网络先于一切】
# ============================================================================
# 这个脚本每次开机都跑，而它要做的事（换 PL 位流）**会把 eth0 弄没**
# —— eth0 是 `80000000.ethernet`，网卡在厂家 PL 里，位流一换硬件就不存在了。
#
# 所以顺序是死的：**先把 eth1 配好、确认它活着，再碰 PL。**
# eth1 是 `ff0e0000.ethernet`，PS 里的 GEM 硬核，PL 怎么刷都动不了它。
#
# 反过来的顺序（先刷 PL 再配网）有一个不可接受的失败模式：位流刷坏了，
# 而网络还没起来 —— 板子永久失联，只能拔卡。三振退备份镜像那套救不了它，
# 因为**Linux 启动成功了**，三振看的是"有没有进 Linux"，不是"能不能连上"。
#
# ============================================================================
# 【每一步失败之后都必须还剩点什么】
# ============================================================================
#   · eth1 起不来   → 照样往下走，但把失败写进状态文件。此时 eth0 还在
#                     （PL 还没动），至少这一轮还能连上去看。
#   · 位流装载失败  → 装回厂家位流、重绑驱动、把 eth0 救回来。
#   · daemon 起不来 → 位流和网络都留着，人能连上来手动查。
# 也就是说：**没有任何一条路径的终点是"连不上"。**
set -u

D=/media/sd-mmcblk1p2/hsm
BIT=$D/zu3eg_hsm.bit
LOG=$D/hsm-boot.log
STATUS=$D/HSM_STATUS
ETH1_IP=192.168.50.175
ETH1_MASK=255.255.255.0

say() { echo "[$(cut -d' ' -f1 /proc/uptime)] $*" >> $LOG; sync; }
st()  { echo "$*" >> $STATUS; sync; }

: > $LOG
: > $STATUS
say "=== hsm-boot 开始 ==="

# ============================================================================
# 第一步：网络。**在碰 PL 之前**，而且不管后面成不成都先做完。
# ============================================================================
busybox ifconfig eth1 up 2>/dev/null
busybox ifconfig eth1 "$ETH1_IP" netmask "$ETH1_MASK" up 2>/dev/null

# 同网段两块网卡时，ARP 必须收敛到发起请求的那个口。默认行为下回包可能
# 从另一个口出去，表现是"时通时不通" —— 演示里最难解释的那种故障。
echo 1 > /proc/sys/net/ipv4/conf/all/arp_ignore   2>/dev/null
echo 2 > /proc/sys/net/ipv4/conf/all/arp_announce 2>/dev/null

# 等链路。冷启动时 PHY 协商比 init 慢，不等的话下面的判断必然是假阴性。
i=0
while [ $i -lt 20 ]; do
    [ "$(cat /sys/class/net/eth1/carrier 2>/dev/null)" = "1" ] && break
    sleep 1
    i=$((i + 1))
done

if [ "$(cat /sys/class/net/eth1/carrier 2>/dev/null)" = "1" ]; then
    say "eth1 就绪：$ETH1_IP（等了 ${i}s）"
    st "NET=ok eth1=$ETH1_IP"
else
    # 不 return —— 网线可能只是没插。此时 eth0 还在（PL 没动），
    # 这一轮仍然连得上；但**不能**再去换位流，那会把仅剩的路也断掉。
    say "!!! eth1 没有链路（等了 ${i}s）。为了不失联，跳过位流装载。"
    st "NET=fail eth1=no-carrier"
    st "PL=skipped 原因=eth1没链路，换位流会连eth0一起失去"
    st "READY=no"
    exit 0
fi

# ============================================================================
# 第二步：密码位流
# ============================================================================
if [ ! -f "$BIT" ]; then
    # 没有位流文件就什么都不做 —— 删掉它就是回退手段，不用改脚本。
    say "没有 $BIT，保持厂家位流"
    st "PL=factory READY=no"
    exit 0
fi

# 重配 PL 之前必须先解绑 PL 驱动：带着活的 AXI 主口重配 fabric 会把总线挂死，
# 而 AXI 一挂连 sysrq 都写不进去，只能断电。
: > $D/drvmap.boot
for d in $(ls /sys/bus/platform/devices/ | grep -E "^8[0-9a-f]{7}\."); do
    if [ -e /sys/bus/platform/devices/$d/driver ]; then
        drv=$(readlink /sys/bus/platform/devices/$d/driver | sed 's#.*/##')
        echo "$d $drv" >> $D/drvmap.boot
        echo $d > /sys/bus/platform/devices/$d/driver/unbind 2>/dev/null
    fi
done
say "解绑了 $(wc -l < $D/drvmap.boot) 个 PL 驱动（eth0 在内，eth1 不在）"
sleep 1

# ⚠️ 传 SD 上的原路径，别自己先拷进 /lib/firmware —— fpgautil 内部就是
#    `cp %s /lib/firmware`，传 /lib/firmware 里的路径等于让它自己覆盖自己。
# ⚠️ 它**装载失败也返回 0**，唯一说实话的是 fpga_manager 的 state。
fpgautil -b "$BIT" -f Full >> $LOG 2>&1
sleep 2
PLST=$(cat /sys/class/fpga_manager/fpga0/state 2>/dev/null)

if [ "$PLST" != "operating" ]; then
    say "!!! 位流装载失败（state=$PLST），回退厂家位流"
    fpgautil -b $D/factory.bit -f Full >> $LOG 2>&1
    sleep 2
    sort -u $D/drvmap.boot | while read d drv; do
        echo $d > /sys/bus/platform/drivers/$drv/bind 2>/dev/null
    done
    sleep 2
    busybox ifconfig eth0 192.168.50.174 netmask 255.255.255.0 up 2>/dev/null
    say "已回退，eth0 也救回来了（eth1 全程没动）"
    st "PL=fail-rolledback state=$PLST"
    st "READY=no"
    exit 0
fi
say "密码位流已装载（state=operating），eth0 随厂家 PL 一起消失，这是预期的"
st "PL=crypto"

# ============================================================================
# 第三步：安全 MMIO 通道 + daemon
# ============================================================================
# 四个功能核都是 SECURE_ONLY=1，普通世界一个寄存器都摸不到。daemon 靠
# /dev/secmmio 把每一笔核访问经 SMC 转给 EL3 发出（BL31 里的白名单 SiP）。
# 所以这个模块不是可选项，它是 daemon 唯一的硬件通路。
if [ -f "$D/secmmio.ko" ]; then
    /sbin/insmod $D/secmmio.ko >> $LOG 2>&1
    if [ -e /dev/secmmio ]; then
        say "secmmio 就绪"
        st "SECMMIO=ok"
    else
        # 最常见的原因是当前 BOOT.BIN 里的 BL31 没有那个 SiP
        # （黄金镜像就没有）。位流和网络都还在，人能连上来查。
        say "!!! insmod 之后没有 /dev/secmmio —— BOOT.BIN 里的 BL31 可能没带 SiP"
        st "SECMMIO=fail"
        st "READY=no"
        exit 0
    fi
fi

if [ -x "$D/pqchsm_fpgad" ]; then
    setsid $D/pqchsm_fpgad >> $LOG 2>&1 < /dev/null &
    sleep 2
    if [ -S /tmp/pqchsm_fpgad.sock ]; then   # 见 service/wire.h
        say "daemon 已起"
        st "DAEMON=ok"
        st "READY=yes"
    else
        say "!!! daemon 起来了但没看到 socket"
        st "DAEMON=nosock"
        st "READY=no"
    fi
else
    st "DAEMON=absent READY=no"
fi

say "=== hsm-boot 结束 ==="
