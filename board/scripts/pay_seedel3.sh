# pay_seedel3.sh —— 批 1 第 5 项 + 白名单互锁的上板验证（演示位流）
#
# 为什么用**演示位流**（SECURE_ONLY=0）而不是送检位流：
# seedel3 的 [4b] 要证"普通世界自己发事务写种子口会被拒、而且不产生总线错误"。
# 送检位流下整块从机对普通世界都是关的，那一步就退化成"防火墙挡住了"，
# 证不到**种子口自己那道门**。种子口的门与 SECURE_ONLY 无关，这正是要验的点。
#
# ⚠️ 要求已经切到 BOOT0006（带 PQC_SEED SiP 的 BL31）。没有那个 SiP 的话
#    SECMMIO_SEED 会被 EL3 当未知 FID 拒掉 —— 不伤板子，只是跑不动。
D=/media/sd-mmcblk1p2/hsm
OUT=$D/seedel3log.txt
: > $OUT

echo "载入带种子暂存口的演示位流 zu3eg_hsm_dev.bit" | tee -a $OUT
mkdir -p /lib/firmware
cp $D/zu3eg_hsm_dev.bit /lib/firmware/ 2>/dev/null
fpgautil -b /lib/firmware/zu3eg_hsm_dev.bit -f Full >> $OUT 2>&1
sleep 2
sync

# 闸门：PL 必须 operating 才允许发 SMC（这一步不碰 PL 总线）
ST=$(cat /sys/class/fpga_manager/fpga0/state 2>/dev/null)
echo "fpga_manager state = $ST" | tee -a $OUT
case "$ST" in
  operating) ;;
  *) echo "PL 不是 operating，拒绝继续" | tee -a $OUT; sync; exit 2 ;;
esac

echo "装 secmmio.ko" | tee -a $OUT
busybox rmmod secmmio 2>/dev/null
busybox insmod $D/secmmio.ko >> $OUT 2>&1
ls -l /dev/secmmio >> $OUT 2>&1

echo "=== seedel3 ===" | tee -a $OUT
$D/seedel3 >> $OUT 2>&1
echo "seedel3 rc=$?" | tee -a $OUT

echo "=== seedprobe（非安全世界那一侧的独立复核）===" | tee -a $OUT
$D/seedprobe >> $OUT 2>&1
echo "seedprobe rc=$?" | tee -a $OUT

sync
