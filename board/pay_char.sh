# pay_char.sh —— 表征版 bitstream 上板：TRNG 原始比特 + 风扇上升沿
#
# 一次上板同时办两件在产品版下**做不到**的事：
#
#  ① **导 TRNG 调理前的原始比特**，供 SP 800-90B 出真实最小熵。
#     产品版里 RAW_TAP=0，那条通路连寄存器一起不存在。
#
#  ② **验风扇的上升沿**（温度上去→档位上去）。产品阈值是 45/55/65/75°C，
#     而这块板**热不起来** —— 实测风扇完全停转 + 四个 CPU 满载 + PL 密码核
#     循环，180 秒结温只从 34.1 升到 36.5°C 就趋平。表征版把阈值压到
#     30/32/34/36°C，落在这块板真够得着的区间里。控制律是同一份 RTL。
#
# ⚠️ 表征版**不是产品形态**：低阈值 + 原始噪声抽头都只用于取数。
D=/media/sd-mmcblk1p2/hsm
OUT=$D/charlog.txt
: > $OUT

rd() { $D/rdreg "$1" 2>/dev/null | sed -n 's/^0x[0-9a-f]* = 0x\([0-9a-f]*\)$/\1/p'; }

fansample() {
    st=$(rd 0x80050004); tc=$(rd 0x80050008)
    [ -z "$st" ] && st=0
    [ -z "$tc" ] && tc=0
    echo "$(cut -d' ' -f1 /proc/uptime) $1 duty=$(( (0x$st >> 16) & 0xFF )) step=$(( (0x$st >> 24) & 7 )) forced=$(( (0x$st >> 27) & 1 )) stuck=$(( (0x$st >> 29) & 1 )) C10=$(( 0x$tc & 0xFFFF ))" >> $OUT
    sync
}

echo "载入表征版 zu3eg_hsm_char.bit" | tee -a $OUT
mkdir -p /lib/firmware
cp $D/zu3eg_hsm_char.bit /lib/firmware/ 2>/dev/null
fpgautil -b /lib/firmware/zu3eg_hsm_char.bit -f Full
echo "rc=$? state=$(cat /sys/class/fpga_manager/fpga0/state)" | tee -a $OUT
sleep 3

echo "--- 各从机 VERSION ---" | tee -a $OUT
for a in 0x80000020 0x80010000 0x80020000 0x80030000 0x80050000; do $D/rdreg $a; done 2>&1 | tee -a $OUT

# ============================ ① TRNG 原始比特 ============================
echo "--- 导 TRNG 原始比特（目标 1M 比特 = 32768 字）---" | tee -a $OUT
$D/trngraw 32768 $D/trngraw.bin 2>&1 | tee -a $OUT
ls -la $D/trngraw.bin 2>&1 | tee -a $OUT
sync

# ============================ ② 风扇上升沿 ============================
# 低阈值下：30°C 升 1 档、32 升 2、34 升 3、36 升 4。
# 先用覆盖口把风扇停掉让它热起来，**同时盯 step**——覆盖只压占空比，
# 档位机照常按真实温度走，所以升档过程在覆盖期间就能看到。
echo "--- 风扇上升沿：先看基线 ---" | tee -a $OUT
i=0; while [ $i -lt 4 ]; do fansample base; sleep 5; i=$((i+1)); done

echo "--- 覆盖：风扇停转，让它自己热起来 ---" | tee -a $OUT
busybox devmem 0x8005000C 32 0x1
for c in 1 2 3 4; do (while :; do :; done) & done
( while :; do $D/hsm_hwtest >/dev/null 2>&1; done ) & PL=$!
i=0
while [ $i -lt 30 ]; do
    fansample heat
    sleep 5
    i=$((i+1))
done

echo "--- 放开覆盖，看占空比是不是立刻跟上当前档位 ---" | tee -a $OUT
busybox devmem 0x8005000C 32 0x0
i=0; while [ $i -lt 6 ]; do fansample release; sleep 5; i=$((i+1)); done

kill $PL 2>/dev/null
kill %1 %2 %3 %4 2>/dev/null

echo "--- 撤负载，看它降回来 ---" | tee -a $OUT
i=0; while [ $i -lt 12 ]; do fansample cool; sleep 5; i=$((i+1)); done

echo "--- 结果 ---" | tee -a $OUT
cat $OUT
sync
