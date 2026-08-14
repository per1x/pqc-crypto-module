# pay_audit_regress.sh —— 修复后的 bitstream 上，算法侧回归
#
# 这一轮动了地址译码、ML-KEM 的擦除与参数校验、TRNG 的取样路径三处。
# 这个 payload 只回答一个问题：**算法结果有没有被碰坏。**
# 判据仍然是 NIST ACVP / FIPS 197 / GB/T 的官方向量，不是与自己比。
#
# 与 pay_audit.sh 分两次跑：一次 payload 只干一件事，别去挤那条 480 秒看门狗。
D=/media/sd-mmcblk1p2/hsm
OUT=$D/auditregresslog.txt
: > $OUT

echo "载入修复后的 zu3eg_hsm.bit" | tee -a $OUT
mkdir -p /lib/firmware
cp $D/zu3eg_hsm.bit /lib/firmware/ 2>/dev/null
fpgautil -b /lib/firmware/zu3eg_hsm.bit -f Full >> $OUT 2>&1
sleep 2
sync

echo "回归：hsm_hwtest（ML-KEM-512 / AES / SM4 / SM3 / 密钥仓 / 金丝雀）" | tee -a $OUT
$D/hsm_hwtest >> $OUT 2>&1
echo "hsm_hwtest rc=$?" | tee -a $OUT
cp /tmp/RESULT_hwtest.txt $D/RESULT_hwtest_audit.txt 2>/dev/null
sync
echo "payload 结束" | tee -a $OUT
sync
