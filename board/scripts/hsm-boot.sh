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
# ============================================================================
# 【默认起演示形态，送检形态留在旁边随时可切】
# ============================================================================
# SD 上并排放着两份位流，**只差防火墙的 SECURE_ONLY 一个参数**：
#
#   zu3eg_hsm_dev.bit   演示形态  SECURE_ONLY=0  普通世界经 /dev/mem 直连核
#   zu3eg_hsm.bit       送检形态  SECURE_ONLY=1  普通世界一个寄存器都摸不到，
#                                               每一笔访问经 /dev/secmmio → EL3 白名单
#
# 默认起**演示形态**：上电即可演示（网络在、核直连得到、daemon 起、端到端能跑），
# 而且 KAT 与现场排查不需要先换镜像。切送检形态不用改这个脚本 ——
#     sh pl_deadman.sh /media/sd-mmcblk1p2/hsm/zu3eg_hsm.bit 300
# 运行时载入即可（死人开关兜底，网络不丢、不用断电）。
#
# ⚠️ **daemon 两种形态下走的是同一条路**：/dev/secmmio → EL3 SiP 白名单。
#    演示形态下普通世界也摸得到，但 daemon 不走那条 —— 少一条路径就少一处
#    "只在某个形态下才验过"的缺口。所以换形态不影响服务层。
#
# ⚠️ 白名单必须包含 ML-DSA 的槽 6（0x8006_0000），否则送检形态下 daemon 读不到
#    它，而且症状是"位流明明装好了却什么都读不出来"。见
#    boot/atf/patch_atf_secmmio.py 里槽表那段。
BIT=$D/zu3eg_hsm_dev.bit
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
# 第零步：时钟。**在起 daemon 之前**，因为远程口是 mTLS，而证书有有效期。
# ============================================================================
# 这块板有 /dev/rtc，正常情况下时间是对的。但只要它掉一次（RTC 没电、
# 被谁写坏、或者干脆换了块板），系统时间就会落到证书的 notBefore 之前，
# 于是**握手被拒、远程口用不了**，而症状一点都不像时钟问题：
# TLS 1.3 的拒绝是握手后才送到的，客户端那侧看到的是"设备信息是一串乱码"。
# 这个坑 2026-08-18 实测踩过一次（板子慢 5 天）。
#
# 兜底很朴素：时间**不许比 SD 上这些文件的最后修改时间还早**。
# 那些文件是上一次装凭据/上一次开机写下的，所以这条规则等价于
# "时钟单调不倒退"，足以让证书始终处在有效期内。
#
# ⚠️ 只往前调，绝不往后调 —— 往后调会把审计日志的时间戳弄乱，
#    而那比时钟不准糟得多。
CLOCK_STAMP=$D/CLOCK_STAMP
newest=0
for f in $D/pki/hsm_device.crt $D/pki/hsm_ca.crt $CLOCK_STAMP; do
    [ -f "$f" ] || continue
    t=$(date -u -r "$f" +%s 2>/dev/null) || continue
    [ -n "$t" ] || continue
    [ "$t" -gt "$newest" ] && newest=$t
done
now=$(date -u +%s 2>/dev/null || echo 0)
if [ "$newest" -gt "$now" ]; then
    date -u -s "@$newest" >/dev/null 2>&1
    busybox hwclock -w -u >/dev/null 2>&1
    say "时钟往前拨到 $(date -u)（原来比 SD 上的凭据还早，mTLS 会因此失败）"
    st "CLOCK=corrected"
else
    say "时钟看起来正常：$(date -u)"
    st "CLOCK=ok"
fi
# 推进一格，好让下一次开机至少不早于这一次
touch $CLOCK_STAMP 2>/dev/null

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
    say "eth1 就绪：${ETH1_IP}（等了 ${i}s）"
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
    say "没有 ${BIT}，保持厂家位流"
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
    say "!!! 位流装载失败（state=${PLST}），回退厂家位流"
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
    # -lock：启动就把私钥外泄闩锁置上，ML-KEM 的 dk 在**硬件里**再也送不出
    # 总线。这是交付/演示形态该有的姿态。
    #
    # ⚠️ 代价说清楚：闩锁只有**重新装载位流**才解得开，而 ACVP 的 KeyGen 向量
    #    需要核对 dk。所以跑那套 KAT 之前要先重启（或重装位流）并且不带 -lock。
    # ⚠️⚠️ **批 1 之后这个 daemon 有两个新的前置，上板前先确认，别在板上现查。**
    #
    #  ① **BOOT.BIN 里的 BL31 必须带种子 SiP**（0x8200ff14，
    #     boot/atf/patch_atf_secmmio.py）。KeyGen 现在的种子由 EL3 生成并
    #     直接写进 PL —— 旧 BL31 上这条 SMC 是未知的，ATF 回 SMC_UNK，
    #     症状是"daemon 起来了、ping 通了、一做 KeyGen 就 HARDFAIL"。
    #     daemon 启动时会探一次内核模块认不认这个 ioctl，但**服务本身在不在
    #     要到第一次 KeyGen 才确证**（非法目标被拒与服务不存在回的都是 EIO）。
    #  ② **位流必须是带种子暂存口的那一版**（mlkem_axi 0x38 / mldsa_axi 0x34）。
    #     旧位流上那个偏移不存在，写进去石沉大海，KeyGen 会因为
    #     SEED_ERR 被拒 —— 这一条倒是吵的，STATUS 上看得见。
    #  ③ **内核模块要重新编**（board/kmod，新增 SECMMIO_SEED ioctl）。
    #
    # 三样是一套，换其中一样不换另外两样都会失败。顺序：
    #   先证明能 JTAG 救援 → 换位流 → 换 BOOT.BIN → 重编内核模块 → 起 daemon。
    setsid $D/pqchsm_fpgad -lock >> $LOG 2>&1 < /dev/null &
    sleep 2
    if [ -S /tmp/pqchsm_fpgad.sock ]; then   # 见 service/wire.h
        say "daemon 已起"
        st "DAEMON=ok"
        # TCP 前端是**可选**的：pki/ 里三样齐了才监听（fail-closed，见 wire.h）。
        # 所以这里如实报"有没有在听"，而不是假定它一定在。
        #
        # ⚠️ 判据从"有没有 hsm_token"换成了"pki 三件套全不全" ——
        #    远程口 2026-08-18 从「明文 TCP + 预共享口令」换成了 mTLS。
        #    旧的 hsm_token 文件即便还在也不再有任何作用，别照着它判断。
        if busybox netstat -ltn 2>/dev/null | grep -q ":9797 "; then
            st "TCP=9797 远程可用（mTLS）"
        elif [ -f "$D/pki/hsm_ca.crt" ] && [ -f "$D/pki/hsm_device.crt" ] &&
             [ -f "$D/pki/hsm_device.key" ]; then
            st "TCP=fail 凭据齐全但没监听上"
        else
            st "TCP=off pki/ 凭据不全，只提供本机 socket"
        fi
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
