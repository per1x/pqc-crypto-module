# pay_d7_sec.sh —— D7 的送检形态（SECURE_ONLY=1）那一半，经 plharness.sh 跑：
#   sh plharness.sh pay_d7_sec.sh        （脱离终端；收尾装回厂家位流，之后重启回演示形态）
#
# ① 形态自证：风扇口读得到（/dev/mem 这条路本身是通的），TRNG VERSION 读回 0；
# ② 反向：普通世界**读**槽 1（key_vault VERSION / LOAD_REJ / VALID_MAP）全部读回 0；
# ③ 正反向：hsm_hwtest 经 EL3 —— "EL3 通用读写都拒槽 1"那条 PASS，SKIP=2；
#    hsm_kem3 经 EL3 照跑；hsm_secneg 普通世界扫地址 rc=0；
# ④ 正向：D7 daemon 在送检形态下起来，sdf_demo 的 [5]/[7] 全过 ——
#    分组运算经 EL3 白名单（槽 2），装载经 TA → PTA → PQC_SYMKEY（槽 1）。
#
# ⚠️ **普通世界只读，一笔写都不发**（SECURE_ONLY=1 的核被拒的写在旧位流上是 SError）。
D=/media/sd-mmcblk1p2/hsm
OUT=$D/d7seclog.txt
: > $OUT
rc_all=0

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

echo "=== ① 形态自证 ===" | tee -a $OUT
FAN=$(busybox devmem 0x80050000 2>/dev/null || echo ERR)
TRNG=$(busybox devmem 0x80000020 2>/dev/null || echo ERR)
echo "  FAN=${FAN}  TRNG_VER=${TRNG}" | tee -a $OUT
[ "$FAN" = "0x00010000" ] || { echo "  ✗ 风扇口读不到，下面的 0 不算数" | tee -a $OUT; exit 2; }
[ "$TRNG" = "0x00000000" ] || { echo "  ✗ 不是送检位流" | tee -a $OUT; exit 2; }
echo "  ✓ 送检形态" | tee -a $OUT

echo "=== ② 普通世界读槽 1 全部为 0 ===" | tee -a $OUT
for a in 0x80010000 0x8001001C 0x80010034; do
    V=$(busybox devmem $a 2>/dev/null || echo ERR)
    if [ "$V" = "0x00000000" ]; then echo "  ✓ ${a} = ${V}" | tee -a $OUT
    else echo "  ✗ ${a} = ${V}" | tee -a $OUT; rc_all=1; fi
done

echo "=== ③ 经 EL3：hsm_hwtest / hsm_kem3；普通世界：hsm_secneg ===" | tee -a $OUT
busybox rmmod secmmio 2>/dev/null
busybox insmod $D/secmmio.ko >> $OUT 2>&1
$D/hsm_hwtest >> $OUT 2>&1; echo "  hsm_hwtest rc=$?" | tee -a $OUT
cp /tmp/hsm_hwtest.txt $D/RESULT_d7_sechwtest.txt 2>/dev/null
grep -E "密钥仓（槽 1）|SKIP|通过 [0-9]+" /tmp/hsm_hwtest.txt | tee -a $OUT
grep -q "PASS  密钥仓（槽 1）：EL3 通用读写都拒" /tmp/hsm_hwtest.txt || rc_all=1
grep -qE "失败 0，跳过 2" /tmp/hsm_hwtest.txt || rc_all=1
# hsm_kem3 的 "done pass=N fail=M" 打在 **stdout**（进 ${OUT}），不在 /tmp/hsm_kem3.txt ——
# 第一版去后者里找，结果全过也判成失败（2026-10-10 上板踩到）。
$D/hsm_kem3 > /tmp/d7_kem3.out 2>&1; echo "  hsm_kem3 rc=$?" | tee -a $OUT
cat /tmp/d7_kem3.out >> $OUT
grep -E "done pass=" /tmp/d7_kem3.out | tee -a $OUT
grep -qE "done pass=[0-9]+ fail=0" /tmp/d7_kem3.out || rc_all=1
$D/hsm_secneg >> $OUT 2>&1; r=$?; echo "  hsm_secneg rc=${r}" | tee -a $OUT
[ $r -eq 0 ] || rc_all=1

echo "=== ④ 送检形态下 D7 daemon + sdf_demo ===" | tee -a $OUT
rm -f /tmp/pqchsm_fpgad.sock
setsid $D/pqchsm_fpgad -lock >> $D/hsm-boot.log 2>&1 < /dev/null &
for i in 1 2 3 4 5 6 7 8 9 10; do [ -S /tmp/pqchsm_fpgad.sock ] && break; sleep 1; done
$D/sdf_demo > $D/d7sec_sdf_demo.txt 2>&1; r=$?
nok=$(grep -c "✅" $D/d7sec_sdf_demo.txt); nbad=$(grep -c "❌" $D/d7sec_sdf_demo.txt)
echo "  sdf_demo rc=${r} ✅=${nok} ❌=${nbad}" | tee -a $OUT
[ $r -eq 0 ] && [ "$nbad" -eq 0 ] || rc_all=1
grep -E "^\[5\]|^\[7\]|KAT 槽 7|同一明文|明文导入|装不进" $D/d7sec_sdf_demo.txt | tee -a $OUT
sleep 1
grep -q "KV_VALID 仍为 1" $D/hsm-boot.log && { echo "  ✗ 会话后金库没擦（daemon 自证）" | tee -a $OUT; rc_all=1; }
for p in $(pids); do kill $p 2>/dev/null; done

echo "=== 结论：rc_all=${rc_all}（0 = 送检形态这几条全过）===" | tee -a $OUT
sync
exit $rc_all
