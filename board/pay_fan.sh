# pay_fan.sh —— 风扇温控的上板验证载荷（经 plharness.sh 跑）
#
# 要回答的问题只有一个：**温度→转速真的跟随吗**，而不是"听着好像变轻了"。
# 所以三段式：空载 → 加负载 → 撤负载，每 5 秒采一次样，最后看曲线。
#
# 判据（写在前面，免得看到数据再编）：
#   ① 空载段占空比应当在 25%（0 档）左右，且**不为 0**；
#   ② 加负载段结温上升，占空比**跟着上一档或几档**；
#   ③ 撤负载后结温回落，占空比**跟着降回来**（迟滞会让它慢一点，这是对的）；
#   ④ forced_full 在正常温区应当是 0；若一直是 1，说明 SYSMON 没读到数，
#      风扇被兜底摁在满速 —— 那是安全的，但温控没工作。
#
# ============================================================================
# 【为什么同时记 PS 侧 AMS 的温度】
# ============================================================================
# 我的温度是自己写的 DRP 状态机从 SYSMONE4 读的。拿它自己证明自己没有意义 ——
# 状态机读错寄存器、换算系数写错，读数照样"看起来像温度"。
# PS 侧的 AMS 驱动（/sys/bus/iio/.../in_temp2_pl_temp_raw）走的是**完全不同的
# 通路**，读的是同一个 PL 温度。两条读数吻合，DRP 这条路才算被验证过。
#
# 附带一个决定性的旁证：**装厂家 bitstream 时 PL 温度读数是 0**（厂家 PL 里
# 没有 SYSMON）。载入我的设计之后它若变成合理数值，就说明我这个 SYSMONE4
# 实例确实活着 —— 这一条不经过我写的任何一行代码。
D=/media/sd-mmcblk1p2/hsm
AMS=/sys/bus/iio/devices/iio:device0
OUT=$D/fanlog.txt

: > $OUT

rd() { $D/rdreg "$1" 2>/dev/null | sed -n 's/^0x[0-9a-f]* = 0x\([0-9a-f]*\)$/\1/p'; }

# AMS 原始码 → 摄氏度×10。offset/scale 取自 sysfs（-36058 / 7.7715）。
ams_c10() { echo $(( ($1 - 36058) * 7772 / 100000 )); }

sample() {
    st=$(rd 0x80050004)
    tc=$(rd 0x80050008)
    [ -z "$st" ] && st=0
    [ -z "$tc" ] && tc=0
    code=$(( 0x$st & 0xFFFF ))
    duty=$(( (0x$st >> 16) & 0xFF ))
    step=$(( (0x$st >> 24) & 7 ))
    forced=$(( (0x$st >> 27) & 1 ))
    tmo=$(( (0x$st >> 28) & 1 ))
    myc10=$(( 0x$tc & 0xFFFF ))
    psraw=$(cat $AMS/in_temp0_ps_temp_raw 2>/dev/null || echo 0)
    plraw=$(cat $AMS/in_temp2_pl_temp_raw 2>/dev/null || echo 0)
    up=$(cut -d' ' -f1 /proc/uptime)
    echo "$up $1 duty=$duty step=$step forced=$forced tmo=$tmo mineC10=$myc10 code=$code psC10=$(ams_c10 $psraw) plC10=$(ams_c10 $plraw) plraw=$plraw" >> $OUT
    sync
}

echo "载入 zu3eg_hsm_fan.bit"
mkdir -p /lib/firmware
cp $D/zu3eg_hsm_fan.bit /lib/firmware/ 2>/dev/null
fpgautil -b /lib/firmware/zu3eg_hsm_fan.bit -f Full
echo "rc=$? state=$(cat /sys/class/fpga_manager/fpga0/state)"
sleep 3

echo "--- 风扇观测口 VERSION（应当是 0x00010000）---"
$D/rdreg 0x80050000
echo "--- 密码从机 VERSION（确认这一版没把别的弄坏）---"
for a in 0x80000020 0x80010000 0x80020000 0x80030000; do $D/rdreg $a; done

# ---- ① 空载 60 秒 ----
echo "--- 空载 60s ---"
i=0
while [ $i -lt 12 ]; do sample idle_a; sleep 5; i=$((i+1)); done

# ---- ② 加负载 150 秒 ----
# PS 四个核跑满 + 反复跑硬件自测（把 PL 里的密码核也带起来）。
# 两边一起烧，才像真的负载。
echo "--- 加负载 150s ---"
for c in 1 2 3 4; do (while :; do :; done) & done
HOGS=$(jobs -p 2>/dev/null)
( while :; do $D/hsm_hwtest >/dev/null 2>&1; done ) &
PLLOAD=$!
i=0
while [ $i -lt 30 ]; do sample load; sleep 5; i=$((i+1)); done
kill $PLLOAD 2>/dev/null
for p in $HOGS; do kill $p 2>/dev/null; done
kill %1 %2 %3 %4 2>/dev/null

# ---- ③ 撤负载 90 秒 ----
echo "--- 撤负载 90s ---"
i=0
while [ $i -lt 18 ]; do sample idle_b; sleep 5; i=$((i+1)); done

echo "--- 采样结果 ---"
cat $OUT
sync
