# pay_service.sh —— 服务层端到端演示：应用 → 库 → daemon → 内核 → EL3 → FPGA
D=/media/sd-mmcblk1p2/hsm
OUT=$D/servicelog.txt
: > $OUT

echo "载入 SECURE_ONLY=1 版 zu3eg_hsm.bit" | tee -a $OUT
mkdir -p /lib/firmware
cp $D/zu3eg_hsm.bit /lib/firmware/ 2>/dev/null
fpgautil -b /lib/firmware/zu3eg_hsm.bit -f Full >> $OUT 2>&1
sleep 2
sync

ST=$(cat /sys/class/fpga_manager/fpga0/state 2>/dev/null)
echo "fpga_manager state = $ST" | tee -a $OUT
case "$ST" in operating) ;; *) echo "PL 不是 operating，停" | tee -a $OUT; exit 2 ;; esac

busybox insmod $D/secmmio.ko >> $OUT 2>&1

echo "=== 起 daemon ===" | tee -a $OUT
$D/pqchsm_fpgad >> $OUT 2>&1 &
DPID=$!
sleep 2

echo "=== 跑独立应用 sdf_demo（另一个进程）===" | tee -a $OUT
$D/sdf_demo >> $OUT 2>&1
echo "sdf_demo rc=$?" | tee -a $OUT

# 反证：同一时刻，普通世界直接读核必须全部被拒
echo "=== 反证：普通世界直读 ===" | tee -a $OUT
$D/hsm_secneg >> $OUT 2>&1
echo "hsm_secneg rc=$?" | tee -a $OUT

kill $DPID 2>/dev/null
head -20 /proc/secmmio >> $OUT 2>&1
sync
echo "payload 结束" | tee -a $OUT
sync
