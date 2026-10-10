# pay_d7.sh —— D7（对称密钥装载归 TEE）上板一把跑完：能在板上自证的那几条
#
# 前提（本脚本**不做**、只检查）：
#   · 板子跑的是 D7 那一套：BL31（PQC_SYMKEY 0x8200ff16 + 槽 1 整槽排除）、
#     optee_os（PTA 第二条命令）、TA（GET_INFO 特性位含 bit4）、daemon；
#   · 演示形态（zu3eg_hsm_dev.bit，hsm-boot.sh 开机就绪），daemon 在跑、
#     **此刻没有客户端连着**（d7probe 收尾会发 ZEROIZE，正在用的会话会丢密钥）。
#   · 板上 $D 里有：ta_symkey、d7probe、sdf_demo（构建机交叉编译）。
#
# 送检形态那一半（EL3 拒槽 1）不在这里：切 zu3eg_hsm.bit 之后跑 pay_seckat.sh，
# 看 hsm_hwtest 的"密钥仓（槽 1）：EL3 通用读写都拒"那条 PASS，SKIP=2。
# wire_fuzz.py 从 Mac 那侧对板子跑（它要 mTLS 凭据）。
#
# 顺序有讲究：ta_symkey 在 daemon 会话之外往槽 3/4/5/7 装了密钥 → d7probe 收尾
# 的 ZEROIZE 把它们擦掉 → sdf_demo 再装、会话结束 daemon 再擦 → 最后读 VALID_MAP
# 必须是 0。最后那条读的是"daemon 的段末即擦"，不是 d7probe 那次。
D=/media/sd-mmcblk1p2/hsm
OUT=$D/d7log.txt
: > $OUT
rc_all=0

ST=$(cat /sys/class/fpga_manager/fpga0/state 2>/dev/null)
echo "fpga_manager state = $ST" | tee -a $OUT
case "$ST" in
  operating) ;;
  *) echo "PL 不是 operating，拒绝继续（TA 会经 EL3 碰 PL）" | tee -a $OUT; sync; exit 2 ;;
esac

echo "=== ① TA 接口纪律（正反两面）===" | tee -a $OUT
$D/ta_symkey >> $OUT 2>&1; rc=$?
echo "ta_symkey rc=$rc" | tee -a $OUT; [ $rc -eq 0 ] || rc_all=1

echo "=== ② 演示位流：普通世界装不进对称密钥（装载门）===" | tee -a $OUT
$D/d7probe >> $OUT 2>&1; rc=$?
echo "d7probe rc=$rc" | tee -a $OUT; [ $rc -eq 0 ] || rc_all=1

echo "=== ③ sdf_demo：KAT 槽 + 生成/blob 重装 + 明文导入被拒 ===" | tee -a $OUT
$D/sdf_demo > $D/d7_sdf_demo.txt 2>&1; rc=$?
cat $D/d7_sdf_demo.txt >> $OUT
nok=$(grep -c "✅" $D/d7_sdf_demo.txt)
nbad=$(grep -c "❌" $D/d7_sdf_demo.txt)
echo "sdf_demo rc=$rc ✅=$nok ❌=$nbad" | tee -a $OUT
[ $rc -eq 0 ] && [ "$nbad" -eq 0 ] || rc_all=1
# [5] 的四条新判据必须**出现**（不出现 = 跑的不是 D7 的 sdf_demo，比失败更该叫出来）
for k in "明文导入被拒" "KAT 槽 7" "同一明文得同一密文" "装不进 KAT 槽 7"; do
  if grep "✅" $D/d7_sdf_demo.txt | grep -q "$k"; then
    echo "  有：$k" | tee -a $OUT
  else
    echo "  ✗ 缺：$k" | tee -a $OUT; rc_all=1
  fi
done

echo "=== ④ 段末即擦：会话结束后对称金库为空 ===" | tee -a $OUT
sleep 1
$D/d7probe vmap >> $OUT 2>&1; rc=$?
echo "d7probe vmap rc=$rc" | tee -a $OUT; [ $rc -eq 0 ] || rc_all=1
# daemon 的日志是 hsm-boot.sh 重定向的那个文件（logf_ 写 stderr），不是 syslog
grep -q "KV_VALID 仍为 1" $D/hsm-boot.log 2>/dev/null \
  && { echo "  ✗ daemon 日志里出现了「KV_VALID 仍为 1」" | tee -a $OUT; rc_all=1; }

echo "=== 结论：rc_all=$rc_all（0 = D7 板上这几条全过）===" | tee -a $OUT
sync
exit $rc_all
