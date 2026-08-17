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

[ -f "$NEWBIT" ] || { say "没有 ${NEWBIT}，什么都不做"; exit 0; }
[ -f "$NEWD" ]   || { say "没有 ${NEWD}，什么都不做"; exit 0; }

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
say "自检：eth1=${ip}，daemon 在跑"

# ---- 自检第二层：**真读到 ML-DSA 核**才算装对 ----
# 只看"进程在跑"是不够的：旧 bitstream 上这个 daemon 一样能起来（它的启动
# 自检读的是 TRNG，那个两版都有）。要证明**装的是含 ML-DSA 的那一版**，
# 必须读到槽 6 那个核。daemon 的版本串里带各核的 VERSION，没有 ML-DSA 就没有那一项。
# ---- 自检第二层：**装上的确实是新位流** ----
#
# 【为什么判据是风扇占空比，而不是 ML-DSA 的版本号】
# 最初这里读 mldsa VERSION（槽 6）。但送检形态下 SECURE_ONLY=1，每一笔核访问
# 都要经 BL31 的 SiP 白名单，而那份白名单是 ML-DSA 之前写的、**没有
# 0x8006_0000 这一条** —— 于是读必然被 EL3 拒绝，与"位流对不对"无关。
# 用一个必然失败的检查当判据，等于把自检变成"必然回滚"。
#
# ⚠️ **2026-08-17 更新：那个根因已经修掉了。** 白名单补上了槽 6、BL31 重建、
#    BOOT.BIN 已提升，送检形态下 ML-DSA 现在读得到（见
#    boot/atf/patch_atf_secmmio.py 与 board/logs/RESULT_secform_mldsa.txt）。
#    也就是说下面这段"只能用风扇当指纹"的理由**不再成立**；这个脚本保持原样
#    是因为它验的是另一件事（新旧位流的风扇档位差异），而不是因为读不到核。
#    要写新的自检，直接读 mldsa VERSION 是更强的判据。
#
# 换成风扇：**风扇观测口（槽 5）SECURE_ONLY=0**（它不在密码边界内），
# 普通世界经 /dev/mem 直接读得到，不经白名单。而两版位流的低温占空比不同：
#     旧位流最低 25%   新位流最低 8%（并在 35°C 加了一档）
# 所以低温下读到 8% 这件事**同时证明**了两件事：新位流真的装上了、
# 而且风扇那处改动在真硅上生效。一个寄存器顶两个判据。
#
# STATUS(0x8005_0004)：[15:0]=温度码 [23:16]=占空比% [26:24]=档位
sleep 1
fan_st=$(busybox devmem 0x80050004 2>/dev/null)
duty=$(( ( $fan_st >> 16 ) & 0xFF ))
step=$(( ( $fan_st >> 24 ) & 0x7 ))
tc=$(( $fan_st & 0xFFFF ))
say "风扇 STATUS=$fan_st → 温度码=$tc 占空比=${duty}% 档位=$step"
# ⚠️ **判据是档位，不是占空比。** 我第一版拿"占空比 ≤20% 才算新位流"当判据，
#    结果把一个完全正常的新位流判成了旧的：板子当时 31.9°C，正落在新档位表的
#    迟滞带里（T0_DN=30 / T0_UP=35），复位后从档 5 往下降，降到档 1 就停住
#    （再降需要 ≤30°C）—— 所以新位流在这个温度下**本来就该是 25%**。
#
#    真正能区分两版的是**档位数**，因为两版的降档条件不同：
#        旧位流（5 档）：档 1→0 需 ≤T1_DN=40°C  → 31.9°C 时降到 **档 0**
#        新位流（6 档）：档 1→0 需 ≤T0_DN=30°C  → 31.9°C 时停在 **档 1**
#    也就是说同样是 25%，旧位流报档 0、新位流报档 1。
#
#    合法组合：档 0（<30°C，新位流的 8% 安静档）或 档 1（30~45°C，25%）。
#    旧位流在这个温区只会报档 0 且占空比 25% —— 那是"档 0 却 25%"的组合，
#    新位流的档 0 是 8%。所以判据写成：**(档 0 且 ≤12%) 或 (档 1 且 ~25%)**。
ok=0
[ "$step" -eq 0 ] && [ "$duty" -le 12 ] && ok=1     # 安静档：新位流独有
[ "$step" -eq 1 ] && [ "$duty" -ge 20 ] && ok=1     # 迟滞带里的 25%：新位流的档 1
if [ "$ok" -ne 1 ]; then
    say "档位=$step 占空比=${duty}% 不是新位流的合法组合（新：档0=8% / 档1=25%）"
    rollback
    exit 1
fi
say "自检：档位=$step 占空比=${duty}% —— 确认是含 ML-DSA 与新风扇档位的那一版位流"

# ML-DSA 那一侧**如实记一笔，但不作为回滚判据** —— 它读不到是已知的、
# 且原因在 BL31 白名单，不在位流。写进日志免得日后误以为验过了。
info=$($D/sdf_demo 2>/dev/null | head -20)
echo "$info" >> $LOG
if echo "$info" | grep -q 'mldsa=0x00000000'; then
    say "注意：mldsa VERSION 读回 0 —— 槽 6 不在 BL31 的 SiP 白名单里，"
    say "      **ML-DSA 本次未在硬件上验证**，这一条不许当作已验。"
fi

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
