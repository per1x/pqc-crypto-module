# pay_ta_deploy.sh —— 把 TA 装到板子的 OP-TEE 能找到的地方
#
# ⚠️ **/lib 在这块板上是 initramfs，重启就没了。** TA 的持久副本放在 SD 上
#    （$D/*.ta），开机时由 hsm-boot.sh 复制到 /lib/optee_armtz/。
#    直接往 /lib 里放而不管 SD，症状是"今天验过了，明天开机 TA 又找不到"。
#
# OP-TEE 找 TA 的顺序：先看 early TA（编进镜像的），再经 tee-supplicant 到
# 普通世界文件系统的 /lib/optee_armtz/<uuid>.ta。本项目用后者 —— 好处是换 TA
# 不用重打 BOOT.BIN，代价是**TA 的完整性只由签名保证，不由启动链保证**
# （见 ARCHITECTURE-TARGET §7：未烧 eFUSE，镜像替换本来就挡不住）。
D=/media/sd-mmcblk1p2/hsm
mkdir -p /lib/optee_armtz
for f in $D/*.ta; do
    [ -f "$f" ] || continue
    cp -f "$f" /lib/optee_armtz/
done
ls -l /lib/optee_armtz/ 2>/dev/null

# tee-supplicant 由 rootfs 自带并在开机时起来；这里只在它不在时补起
if ! ps | grep -q "[t]ee-supplicant"; then
    setsid /usr/sbin/tee-supplicant >/dev/null 2>&1 &
    sleep 2
fi
ps | grep "[t]ee-supplicant" | grep -v grep
