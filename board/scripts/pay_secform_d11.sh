# pay_secform_d11.sh —— 送检位流（SECURE_ONLY=1）在 D11 改动之后的验证
#
# ============================================================================
# 【为什么这一趟不能省】
# ============================================================================
# D11 把两个核的防火墙 ADDR_MASK 从 0xC0 改成 0x80，也就是**地址窗口从 0x3F
# 放宽到 0x7F**。窗口是防火墙的一部分，改宽了就必须重新证明"门还是关着的"——
# 演示位流下证不了这件事（那边门本来就开着）。
#
# 三件事：
#   ① 反向：hsm_secneg —— 普通世界直接扫地址，四个核的 VERSION 全部读回 0；
#   ② 反向补充：**新放宽出来的那一段**（0x40/0x44，两个核各一对打包口）
#      普通世界读回也必须是 0。少了这一条，等于只证明了旧窗口还关着。
#   ③ 正向：hsm_kem3 / hsm_hwtest 经 /dev/secmmio → EL3 跑完整 KAT ——
#      证明"关上门之后安全世界仍然用得了"，否则一块砖也能通过 ①②。
#
# ⚠️ **只读，一笔写都不发。** SECURE_ONLY=1 的核被拒的写是 DECERR，
#    posted 写以 SError 打回内核只能 panic，代价是一次断电
#    （aarch64-write-error-is-serror）。读被拒是 RAZ，安全。
D=$(dirname "$0")
[ -f "$D/hsm_secneg" ] || D=/media/sd-mmcblk1p2/hsm
OUT=$D/secform_d11.txt
: > $OUT

pids() { ps | grep pqchsm_fpgad | grep -v grep | busybox awk '{print $1}'; }
for p in $(pids); do kill $p 2>/dev/null; done
sleep 2
for p in $(pids); do kill -9 $p 2>/dev/null; done
sleep 1

echo "载入送检位流 zu3eg_hsm.bit（SECURE_ONLY=1）" | tee -a $OUT
mkdir -p /lib/firmware
cp $D/zu3eg_hsm.bit /lib/firmware/ 2>/dev/null
fpgautil -b /lib/firmware/zu3eg_hsm.bit -f Full >> $OUT 2>&1
sleep 2
ST=$(cat /sys/class/fpga_manager/fpga0/state 2>/dev/null)
echo "fpga_manager state = ${ST}" | tee -a $OUT
case "$ST" in operating) ;; *) echo "PL 不是 operating，停" | tee -a $OUT; exit 2 ;; esac

echo "" | tee -a $OUT
echo "=== ① 形态自证：风扇口该读得到，TRNG 该读回 0 ===" | tee -a $OUT
FAN=$(busybox devmem 0x80050000 2>/dev/null || echo ERR)
TRNG=$(busybox devmem 0x80000020 2>/dev/null || echo ERR)
echo "  FAN(0x80050000)=${FAN}  TRNG_VER(0x80000020)=${TRNG}" | tee -a $OUT
# 风扇那一路永远 SECURE_ONLY=0 —— 它证明 /dev/mem 这条路本身是通的。
# 少了它，一根拔掉的线也会"看起来像门关着"。
if [ "$FAN" != "0x00010000" ]; then
    echo "  ✗ 风扇口读不到 —— /dev/mem 这条路本身有问题，下面的 0 不算数" | tee -a $OUT
    exit 2
fi
if [ "$TRNG" = "0x00000000" ]; then
    echo "  ✓ 是送检形态（普通世界读 TRNG VERSION 回 0）" | tee -a $OUT
else
    echo "  ✗ TRNG 读到 ${TRNG} —— 装的不是送检位流" | tee -a $OUT; exit 2
fi

echo "" | tee -a $OUT
echo "=== ② 新放宽出来的窗口 0x40/0x44 对普通世界也必须读回 0 ===" | tee -a $OUT
bad=0
for a in 0x80030040 0x80030044 0x80060040 0x80060044 \
         0x80030000 0x80060000 0x8003003C 0x80060038; do
    V=$(busybox devmem $a 2>/dev/null || echo ERR)
    if [ "$V" = "0x00000000" ]; then
        echo "  ✓ ${a} = ${V}" | tee -a $OUT
    else
        echo "  ✗ ${a} = ${V} —— 放宽窗口之后漏了" | tee -a $OUT
        bad=1
    fi
done
[ "$bad" = "0" ] && echo "  ✓ 含打包口在内，整段对普通世界都是 0" | tee -a $OUT

echo "" | tee -a $OUT
echo "=== ③ 正向：安全世界经 EL3 仍然用得了 ===" | tee -a $OUT
busybox rmmod secmmio 2>/dev/null
busybox insmod $D/secmmio.ko >> $OUT 2>&1
$D/hsm_kem3 >> $OUT 2>&1
echo "  hsm_kem3 rc=$?" | tee -a $OUT
cp /tmp/hsm_kem3.txt $D/RESULT_secform_d11_kem3.txt 2>/dev/null
# hsm_kem3 自己在末尾打 "done pass=N fail=M" —— 直接引它，别另起一套计数。
# 第一版这里用 grep -cE "✓|PASS" 去数，而它的输出根本不是那个格式，于是恒为 0；
# 判据打成 0 比没有判据更糟：看起来像"一条都没过"，而实际是 20/0。
busybox grep -E "^done pass=" /tmp/hsm_kem3.txt 2>/dev/null | sed 's/^/  hsm_kem3 /' | tee -a $OUT

echo "" | tee -a $OUT
echo "=== ④ 反向：hsm_secneg（普通世界扫地址）===" | tee -a $OUT
$D/hsm_secneg >> $OUT 2>&1
echo "  hsm_secneg rc=$?" | tee -a $OUT

# 收尾：装回演示位流并把 daemon 起回来（板子默认形态）
echo "" | tee -a $OUT
echo "=== 收尾：装回演示位流 ===" | tee -a $OUT
cp $D/zu3eg_hsm_dev.bit /lib/firmware/ 2>/dev/null
fpgautil -b /lib/firmware/zu3eg_hsm_dev.bit -f Full >> $OUT 2>&1
sleep 2
busybox rmmod secmmio 2>/dev/null
busybox insmod $D/secmmio.ko >> $OUT 2>&1
setsid $D/pqchsm_fpgad -lock >> $D/hsm-daemon.log 2>&1 < /dev/null &
sleep 5
echo "  daemon [$(pids | tr '\n' ' ')]  TRNG_VER=$(busybox devmem 0x80000020)" | tee -a $OUT
sync
