# pay_restart.sh —— 采 SP 800-90B §3.1.4.3 的重启矩阵
#
# ⚠️ 这个 payload 要载**表征版** bitstream（zu3eg_hsm_char.bit，RAW_TAP=1）。
#    表征版把噪声源的**调理前**原始比特接到了一个可读寄存器上 ——
#    那是熵源的内部状态，**绝不能出现在产品/送检那一版里**。两版分开命名、
#    分开传，就是为了不会有人拿错。
#
# 核仍然是 SECURE_ONLY=1，所以读 RAW 也得经 /dev/secmmio 由 EL3 发。
D=/media/sd-mmcblk1p2/hsm
OUT=$D/restartlog.txt
: > $OUT

echo "载入表征版 zu3eg_hsm_char.bit（RAW_TAP=1）" | tee -a $OUT
mkdir -p /lib/firmware
cp $D/zu3eg_hsm_char.bit /lib/firmware/ 2>/dev/null
fpgautil -b /lib/firmware/zu3eg_hsm_char.bit -f Full >> $OUT 2>&1
sleep 2
sync

ST=$(cat /sys/class/fpga_manager/fpga0/state 2>/dev/null)
echo "fpga_manager state = $ST" | tee -a $OUT
case "$ST" in
  operating) ;;
  *) echo "PL 不是 operating，拒绝继续" | tee -a $OUT; sync; exit 2 ;;
esac

busybox insmod $D/secmmio.ko >> $OUT 2>&1

echo "=== 采运行态矩阵（等 startup_done 之后再取，-a）===" | tee -a $OUT
$D/trngrestart 1000 1000 $D/restart_after.bin -a >> $OUT 2>&1
echo "trngrestart -a rc=$?" | tee -a $OUT
ls -l $D/restart_after.bin >> $OUT 2>&1

echo "=== 同批顺序采集 512 KiB（供 H_原始 对照）===" | tee -a $OUT
$D/trngraw 131072 $D/trngraw_same_run.bin >> $OUT 2>&1
echo "trngraw rc=$?" | tee -a $OUT
ls -l $D/trngraw_same_run.bin >> $OUT 2>&1

head -20 /proc/secmmio >> $OUT 2>&1
sync
echo "payload 结束" | tee -a $OUT
sync
