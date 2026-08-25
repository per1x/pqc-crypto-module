# pay_iopack.sh —— D11：4 字节打包口的上板吞吐 A/B
#
# 用演示位流：iopack_bench 要在同一进程里比 /dev/mem 直连与经 EL3 两条通路，
# 前者在送检形态下读回 0，测不出任何东西。
#
# ⚠️ 跑之前把 daemon 停掉：它和 bench 都在驱动同一批寄存器，同时跑的话
#    两边互相踩，测出来的数字与"谁抢到了"有关而不是与打包有关。
D=$(dirname "$0")
[ -f "$D/iopack_bench" ] || D=/media/sd-mmcblk1p2/hsm
OUT=$D/iopacklog.txt
: > $OUT

pids() { ps | grep pqchsm_fpgad | grep -v grep | busybox awk '{print $1}'; }
for p in $(pids); do kill $p 2>/dev/null; done
sleep 2
for p in $(pids); do kill -9 $p 2>/dev/null; done
sleep 1
echo "daemon 已停（剩 [$(pids | tr '\n' ' ')]）" | tee -a $OUT

echo "载入带打包口的演示位流" | tee -a $OUT
mkdir -p /lib/firmware
cp $D/zu3eg_hsm_dev.bit /lib/firmware/ 2>/dev/null
fpgautil -b /lib/firmware/zu3eg_hsm_dev.bit -f Full >> $OUT 2>&1
sleep 2
ST=$(cat /sys/class/fpga_manager/fpga0/state 2>/dev/null)
echo "fpga_manager state = $ST" | tee -a $OUT
case "$ST" in operating) ;; *) echo "PL 不是 operating，停" | tee -a $OUT; exit 2 ;; esac

busybox rmmod secmmio 2>/dev/null
busybox insmod $D/secmmio.ko >> $OUT 2>&1

echo "=== iopack_bench ===" | tee -a $OUT
$D/iopack_bench 20 >> $OUT 2>&1
echo "iopack_bench rc=$?" | tee -a $OUT

# 收尾：把 daemon 起回来（新版，带打包搬运）
setsid $D/pqchsm_fpgad -lock >> $D/hsm-daemon.log 2>&1 < /dev/null &
sleep 5
echo "daemon 起回来了：[$(pids | tr '\n' ' ')]" | tee -a $OUT
sync
