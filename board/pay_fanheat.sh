# pay_fanheat.sh —— 验证**上升沿**：温度真的上去了，转速真的跟上
#
# ============================================================================
# 【为什么要专门做这个实验】
# ============================================================================
# 上一轮已经证明了传感器是活的（读数在抖、与 PS 侧 AMS 吻合到 1°C 以内），
# 也看到了**下降沿**：43.6°C 时在 1 档 40%，掉到 T1_DN(40°C) 以下才回 0 档。
#
# 但**上升沿没验到**：加负载那 150 秒里结温不升反降，从 43.6 一路掉到 28.1°C。
# 原因不是温控不工作，是这颗片子在这个设计下**根本烧不热** —— 4 个 CPU 忙
# 循环加上反复跑硬件自测，产生的热量抵不过 25% 占空比的风。
#
# 所以换个方向制造温升：**用覆盖口把风扇关掉**，让它自己热起来。
# 这样一次实验能同时验三件事：
#   ① 温度真的会升（否则说明温控根本没在影响散热，那 25% 也是白设的）；
#   ② 放开覆盖之后，自动档位**按当时的真实温度**落在正确的档上；
#   ③ 若真到 80°C，forced_full 生效且**覆盖压不住它** —— 不过这一步是
#      兜底，实验本身在 75°C 就收手，不拿最后一道防线当终点。
#
# ============================================================================
# 【安全边界】
# ============================================================================
#  · 超过 75°C 就立刻放开覆盖，不等 80°C 的硬上限自己动手 —— 硬上限是
#    最后一道防线，不是常规手段，拿它当实验终点等于把余量花光。
#  · 器件是工业级（-40~100°C 结温），75°C 还有 25°C 余量。
#  · 无论怎样，harness 收尾都会装回出厂 bitstream（风扇满速），
#    外加 480 秒看门狗。所以最坏情况是板子重启，不是烤片子。
D=/media/sd-mmcblk1p2/hsm
AMS=/sys/bus/iio/devices/iio:device0
OUT=$D/fanheat.txt
: > $OUT

rd() { $D/rdreg "$1" 2>/dev/null | sed -n 's/^0x[0-9a-f]* = 0x\([0-9a-f]*\)$/\1/p'; }
ams_c10() { echo $(( ($1 - 36058) * 7772 / 100000 )); }

# 返回：当前温度（摄氏度×10），并把一行采样写进日志
sample() {
    st=$(rd 0x80050004); tc=$(rd 0x80050008)
    [ -z "$st" ] && st=0
    [ -z "$tc" ] && tc=0
    duty=$(( (0x$st >> 16) & 0xFF ))
    step=$(( (0x$st >> 24) & 7 ))
    forced=$(( (0x$st >> 27) & 1 ))
    stuck=$(( (0x$st >> 29) & 1 ))
    code=$(( 0x$st & 0xFFFF ))
    myc10=$(( 0x$tc & 0xFFFF ))
    psraw=$(cat $AMS/in_temp0_ps_temp_raw 2>/dev/null || echo 0)
    up=$(cut -d' ' -f1 /proc/uptime)
    echo "$up $1 duty=$duty step=$step forced=$forced stuck=$stuck mineC10=$myc10 code=$code psC10=$(ams_c10 $psraw)" >> $OUT
    sync
    CUR=$myc10
}

echo "载入 zu3eg_hsm_fan.bit"
mkdir -p /lib/firmware
cp $D/zu3eg_hsm_fan.bit /lib/firmware/ 2>/dev/null
fpgautil -b /lib/firmware/zu3eg_hsm_fan.bit -f Full
sleep 3
$D/rdreg 0x80050000

# ---- ① 基线 30 秒 ----
i=0; while [ $i -lt 4 ]; do sample base; sleep 5; i=$((i+1)); done

# ---- ② 关风扇 + 加负载，让它热起来 ----
# 0x0C bit0 = 覆盖使能，[15:8] = 覆盖占空比。写 1 = 使能 + 占空比 0。
echo "覆盖：风扇停转（占空比 0）"
busybox devmem 0x8005000C 32 0x1
for c in 1 2 3 4; do (while :; do :; done) & done
( while :; do $D/hsm_hwtest >/dev/null 2>&1; done ) & PLLOAD=$!

i=0
while [ $i -lt 36 ]; do
    sample heat
    # 75°C 就收手 —— 硬上限(80°C)是最后一道防线，不是实验终点
    if [ "$CUR" -ge 750 ]; then
        echo "到 75.0°C，提前放开覆盖" >> $OUT
        break
    fi
    sleep 5
    i=$((i+1))
done

# ---- ③ 放开覆盖，看自动档位接不接得住 ----
echo "放开覆盖，交回自动温控"
busybox devmem 0x8005000C 32 0x0
i=0; while [ $i -lt 20 ]; do sample auto; sleep 5; i=$((i+1)); done

kill $PLLOAD 2>/dev/null
kill %1 %2 %3 %4 2>/dev/null

# ---- ④ 撤负载，看它降回来 ----
i=0; while [ $i -lt 8 ]; do sample cool; sleep 5; i=$((i+1)); done

echo "--- 采样结果 ---"
cat $OUT
sync
