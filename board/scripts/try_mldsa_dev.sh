#!/bin/sh
# try_mldsa_dev.sh —— 带死人开关地试装**开发形态**位流（zu3eg_hsm_dev.bit）
#
# ============================================================================
# 【为什么要一份开发形态的位流】
# ============================================================================
# 送检形态（SECURE_ONLY=1）下普通世界一个寄存器都摸不到，每一笔核访问都要经
# BL31 的 SiP 白名单 —— 而那份白名单是 ML-DSA 之前写的，**没有 0x8006_0000**。
# 于是 ML-DSA 在送检形态下根本不可达，与位流对不对无关。
#
# 补白名单要重建 BL31，那是**动启动镜像**。在拿到许可之前，用开发形态位流
# （SECURE_ONLY=0）先回答一个更要紧的问题：**ML-DSA 在真硅上算得对不对。**
# 开发形态下 Linux 能经 /dev/mem 直连寄存器，不需要 daemon 也不需要 BL31。
#
# ⚠️ **这不是送检形态。** 产物名 zu3eg_hsm_dev.bit 与送检那份分开，就是为了
#    不混。跑完验证要换回去。任何基于这次结果的材料都必须写清"开发形态"。
#
# ============================================================================
# 【自检判据：直接读 ML-DSA 的 VERSION】
# ============================================================================
# 这是开发形态独有的好处：普通世界能直接读到槽 6。各核 VERSION 是非零常量
# 0x0001_0000，而核不存在时经防火墙读回来是 0（RAZ/WI）。所以
#     devmem 0x80060000 == 0x00010000
# 一条就同时证明了：位流装上了、装的是含 ML-DSA 的那一版、而且它可达。
# 比先前用风扇档位当指纹强得多 —— 那个只能证明"是新位流"，不能证明"核可达"。
#
# 三条不变量与 try_mldsa.sh 相同：①重配前先解绑 PL 驱动 ②fpgautil 返回 0
# 不算数、只认 fpga_manager 的 state ③自检不过就自己退回去。
set -u
D=/media/sd-mmcblk1p2/hsm
NEWBIT=$D/zu3eg_hsm_dev.bit
OLDBIT=$D/zu3eg_hsm.bit
OKFILE=$D/mldsa_dev_confirmed
LOG=$D/trymldsadev.log
WAIT=600
MDVER=0x80060000

say() { echo "$(cut -d' ' -f1 /proc/uptime) $*" >> $LOG; sync; }

: > $LOG
rm -f $OKFILE
say "=== 开发形态位流试装开始 ==="
[ -f "$NEWBIT" ] || { say "没有 ${NEWBIT}，什么都不做"; exit 0; }

load_bit() {
    B="$1"
    : > $D/drvmap.dev
    for d in $(ls /sys/bus/platform/devices/ 2>/dev/null | grep -E "^8[0-9a-f]{7}\."); do
        if [ -e /sys/bus/platform/devices/$d/driver ]; then
            drv=$(readlink /sys/bus/platform/devices/$d/driver | sed 's#.*/##')
            echo "$d $drv" >> $D/drvmap.dev
            echo $d > /sys/bus/platform/devices/$d/driver/unbind 2>/dev/null
        fi
    done
    say "已解绑 $(wc -l < $D/drvmap.dev) 个 PL 驱动"
    sleep 1
    cp -f "$B" /lib/firmware/ 2>/dev/null
    fpgautil -b /lib/firmware/$(basename $B) -f Full >> $LOG 2>&1
    sleep 2
    st=$(cat /sys/class/fpga_manager/fpga0/state 2>/dev/null)
    say "装 $(basename $B) 之后 fpga_manager state = $st"
    [ "$st" = "operating" ] || return 1
    sort -u $D/drvmap.dev 2>/dev/null | while read d drv; do
        echo $d > /sys/bus/platform/drivers/$drv/bind 2>/dev/null
    done
    sleep 2
    return 0
}

rollback() {
    say "!!! 回滚：退回 $(basename $OLDBIT)"
    killall pqchsm_fpgad 2>/dev/null; sleep 1
    load_bit "$OLDBIT"
    ( $D/pqchsm_fpgad >> $D/hsm-daemon.log 2>&1 & )
    sleep 3
    say "回滚完成：$(busybox ip -o addr show eth1 2>/dev/null | head -1)"
    sync
}

killall pqchsm_fpgad 2>/dev/null
sleep 1
if ! load_bit "$NEWBIT"; then
    say "开发位流没装上（state 不是 operating）"
    rollback; exit 1
fi

# ---- 自检 ① 网络还在 ----
ip=$(busybox ip -o addr show eth1 2>/dev/null | grep -o '192\.168\.50\.[0-9]*' | head -1)
[ -n "$ip" ] || { say "eth1 没地址"; rollback; exit 1; }
say "自检：eth1=$ip"

# ---- 自检 ② ML-DSA 核可达（开发形态独有）----
v=$(busybox devmem $MDVER 2>/dev/null)
say "ML-DSA VERSION($MDVER) = $v"
case "$v" in
    0x00010000) say "自检：ML-DSA 核在硅上且普通世界可达 —— 开发形态确认" ;;
    *) say "VERSION 不是 0x00010000（核不可达 / 装的不是开发位流）"; rollback; exit 1 ;;
esac

# ---- 顺带记一笔风扇（这一版仍是 30/35 阈值，新的 35/37 要重出位流）----
fs=$(busybox devmem 0x80050004 2>/dev/null)
say "风扇 STATUS=$fs 占空比=$(( ($fs>>16)&0xFF ))% 档位=$(( ($fs>>24)&0x7 ))"

say "等待确认（$WAIT 秒），期间可以跑 mldsa_hwtest"
i=0
while [ $i -lt $((WAIT/10)) ]; do
    [ -f $OKFILE ] && { say "已确认 —— 保持现状"; sync; exit 0; }
    sleep 10
    i=$((i+1))
done
say "$WAIT 秒没等到确认"
rollback
exit 1
