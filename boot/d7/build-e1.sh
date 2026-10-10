#!/bin/bash
# E1（TA 密钥熵接 PL TRNG）：在 D7 的产物上增量构建 BL31 + optee_os + BOOT_E1.BIN + TA。
# 目录全新（*_e1），D7 的产物一个字节不动。配方与 D7 逐项相同（见同目录其它脚本）。
set -e
source /tools/Xilinx/Vitis/2020.1/settings64.sh >/dev/null
export PATH=/tools/Xilinx/Vitis/2020.1/gnu/aarch64/lin/aarch64-none/bin:$PATH
SRC=/home/build/pqc-fpga/e1/src
W=/home/build/wdt_patch
echo "== BL31"
[ -d $W/atf_e1 ] || cp -a $W/atf_d7 $W/atf_e1
python3 $SRC/boot/atf/patch_atf_secmmio.py $W/atf_e1 | tail -2
cd $W/atf_e1
make CROSS_COMPILE=aarch64-none-elf- PLAT=zynqmp SPD=opteed DEBUG=1 LOG_LEVEL=20 \
     BUILD_BASE=$W/atf_e1_build bl31 -j8 2>&1 | tail -1
aarch64-none-elf-readelf -l $W/atf_e1_build/zynqmp/debug/bl31/bl31.elf | grep -A1 LOAD | head -2
echo "== optee_os"
cd /home/build/p4_optee
[ -d optee_os_e1 ] || cp -a optee_os_d7 optee_os_e1
python3 $SRC/boot/optee/patch_optee_seedpta.py /home/build/p4_optee/optee_os_e1 | tail -1
cd optee_os_e1
make -j8 O=out/arm-plat-zynqmp PLATFORM=zynqmp PLATFORM_FLAVOR=zcu102 \
  CROSS_COMPILE64=aarch64-none-elf- CFG_ARM64_core=y CFG_RPMB_FS=n \
  CFG_TEE_CORE_LOG_LEVEL=1 CFG_TEE_TA_LOG_LEVEL=1 CFG_USER_TA_TARGETS=ta_arm64 \
  CFG_ZYNQMP_HUK=n CFG_ZYNQMP_HUK_ASSUME_NO_AUTH=y CFG_CORE_ASLR=n 2>&1 | grep -E "error|Error" || true
diff ../optee_os_d7/out/arm-plat-zynqmp/conf.mk out/arm-plat-zynqmp/conf.mk && echo "conf.mk 与 d7 一致"
echo "== BOOT_E1.BIN"
cd $W/images
python3 - /home/build/p4_optee/optee_os_e1/out/arm-plat-zynqmp/core/tee.bin <<'PY'
import sys, struct
b = open(sys.argv[1], 'rb').read(); assert b[:4] == b'OPTE'
open('tee_load_e1.bin', 'wb').write(b[28:])
tl = struct.unpack('<I', b[len(b)-24:len(b)-20])[0]; assert tl == 0x18
print('boot_embdata.total_len = 0x%x' % tl)
PY
aarch64-none-elf-objcopy -I binary -O elf64-littleaarch64 -B aarch64 tee_load_e1.bin tee_load_e1.o
aarch64-none-elf-ld -T tee_load.ld tee_load_e1.o -o tee_load_e1.elf
sed -e "s#/atf_d7_build/#/atf_e1_build/#" -e "s#tee_load_d7.elf#tee_load_e1.elf#" boot_d7.bif > boot_e1.bif
diff boot_d7.bif boot_e1.bif | grep "^>" || true
bootgen -image boot_e1.bif -arch zynqmp -w -o BOOT_E1.BIN 2>&1 | grep -E "ERROR|WARNING" || true
md5sum BOOT_E1.BIN
echo "== TA"
cd $SRC/tee/ta && make TA_DEV_KIT_DIR=/home/build/p4_optee/optee_os_e1/out/arm-plat-zynqmp/export-ta_arm64 2>&1 | grep -E " error|SIGN" || true
md5sum out/*.ta
mkdir -p /home/build/pqc-fpga/e1/deploy
cp $W/images/BOOT_E1.BIN out/*.ta /home/build/pqc-fpga/e1/deploy/
echo E1_DONE
