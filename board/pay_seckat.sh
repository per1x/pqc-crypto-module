# pay_seckat.sh —— SECURE_ONLY=1 全功能 bitstream：安全世界跑完整 KAT + 反证
#
# 两半必须都跑，缺一半都不成立：
#   正向  hsm_kem3 / hsm_hwtest（默认走 /dev/secmmio，由 EL3 发每一笔核访问）
#   反向  hsm_secneg（普通世界直接读，必须全部 DECERR；**一笔写都不发**）
#
# ⚠️ 这个 payload 要求已经切到带 secmmio SiP 的 BOOT0005 槽。
#    没有那个 SiP 的话，secmmio.ko 的每次 ioctl 都会被 EL3 当未知 FID 拒掉，
#    表现是 hsm_kem3 一上来就报"EL3 读被拒"——不会伤板子，只是跑不动。
#
# ⚠️ **绝不要在这一版 bitstream 上用 -D 跑那两个程序。** -D 是给
#    SECURE_ONLY=0 的旧 bitstream 用的；对 SECURE_ONLY=1 的核写一笔就是
#    DECERR -> SError -> 内核 panic，当场丢板子。默认（不带参数）是安全的。
D=/media/sd-mmcblk1p2/hsm
OUT=$D/seckatlog.txt
: > $OUT

echo "载入 SECURE_ONLY=1 版 zu3eg_hsm.bit" | tee -a $OUT
mkdir -p /lib/firmware
cp $D/zu3eg_hsm.bit /lib/firmware/ 2>/dev/null
fpgautil -b /lib/firmware/zu3eg_hsm.bit -f Full >> $OUT 2>&1
sleep 2
sync

# 闸门一：PL 必须是 operating 才允许发 SMC（这一步不碰 PL 总线）
ST=$(cat /sys/class/fpga_manager/fpga0/state 2>/dev/null)
echo "fpga_manager state = $ST" | tee -a $OUT
case "$ST" in
  operating) ;;
  *) echo "PL 不是 operating，拒绝继续" | tee -a $OUT; sync; exit 2 ;;
esac

echo "装 secmmio.ko" | tee -a $OUT
busybox insmod $D/secmmio.ko >> $OUT 2>&1
ls -l /dev/secmmio >> $OUT 2>&1

echo "=== 正向：安全世界跑 ML-KEM 三参数集 KAT ===" | tee -a $OUT
$D/hsm_kem3 >> $OUT 2>&1
echo "hsm_kem3 rc=$?" | tee -a $OUT
cp /tmp/hsm_kem3.txt $D/RESULT_seckem3.txt 2>/dev/null

echo "=== 正向：安全世界跑对称/国密/密钥仓/TRNG 自测 ===" | tee -a $OUT
$D/hsm_hwtest >> $OUT 2>&1
echo "hsm_hwtest rc=$?" | tee -a $OUT
cp /tmp/hsm_hwtest.txt $D/RESULT_sechwtest.txt 2>/dev/null

echo "=== 反证：普通世界直接读，必须全部 DECERR ===" | tee -a $OUT
$D/hsm_secneg >> $OUT 2>&1
echo "hsm_secneg rc=$?" | tee -a $OUT

head -20 /proc/secmmio >> $OUT 2>&1
sync
echo "payload 结束" | tee -a $OUT
sync
