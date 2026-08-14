# pay_audit.sh —— 审计修复的上板复验
#
# 载入修复后的 zu3eg_hsm.bit，跑 hsm_audit，把报告留在 SD 卡上。
#
# ⚠️ 这个 payload **不重新 bind PL 驱动**。eth0 在厂家 PL 里，一旦绑回去
#    再让 harness 收尾重配 fabric 就会挂总线（见 plharness.sh 文件头）。
#    报告落在 SD 卡上，harness 恢复网络之后再取。
#
# ⚠️ **只跑 hsm_audit，不再顺带跑 hsm_hwtest。** 第一版把两个挤在一次里，
#    离 harness 那条 480 秒看门狗太近。算法侧的回归单独一次跑
#    （pay_audit_regress.sh），一次 payload 只干一件事。
D=/media/sd-mmcblk1p2/hsm
OUT=$D/auditlog.txt
: > $OUT

echo "载入修复后的 zu3eg_hsm.bit" | tee -a $OUT
mkdir -p /lib/firmware
cp $D/zu3eg_hsm.bit /lib/firmware/ 2>/dev/null
fpgautil -b /lib/firmware/zu3eg_hsm.bit -f Full >> $OUT 2>&1
sleep 2
echo "载入完成 rc=$?" | tee -a $OUT
sync

echo "跑 hsm_audit" | tee -a $OUT
$D/hsm_audit >> $OUT 2>&1
echo "hsm_audit rc=$?" | tee -a $OUT
sync

sync
echo "payload 结束" | tee -a $OUT
sync
