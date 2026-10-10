#!/bin/bash
# D7：打 BOOT_D7.BIN —— 与 BOOT_B2.BIN 同一 bif，只换 BL31 与 OP-TEE 两项。只落盘。
set -e
source /tools/Xilinx/Vitis/2020.1/settings64.sh >/dev/null
export PATH=/tools/Xilinx/Vitis/2020.1/gnu/aarch64/lin/aarch64-none/bin:$PATH
cd /home/build/wdt_patch/images
SRC=/home/build/p4_optee/optee_os_d7/out/arm-plat-zynqmp/core/tee.bin
python3 - "$SRC" <<'PY'
import sys, struct
b = open(sys.argv[1], 'rb').read()
assert b[:4] == b'OPTE', '不是 OPTE 头'
open('tee_load_d7.bin', 'wb').write(b[28:])
tl = struct.unpack('<I', b[len(b)-24:len(b)-20])[0]
print('去头后 %d 字节，尾部 boot_embdata.total_len = 0x%x（应为 0x18）' % (len(b)-28, tl))
assert tl == 0x18
PY
aarch64-none-elf-objcopy -I binary -O elf64-littleaarch64 -B aarch64 tee_load_d7.bin tee_load_d7.o
aarch64-none-elf-ld -T tee_load.ld tee_load_d7.o -o tee_load_d7.elf
aarch64-none-elf-readelf -l tee_load_d7.elf | grep -A1 LOAD
sed -e "s#/home/build/wdt_patch/atf_b2_build/#/home/build/wdt_patch/atf_d7_build/#" \
    -e "s#tee_load_b2.elf#tee_load_d7.elf#" boot_b2.bif > boot_d7.bif
diff boot_b2.bif boot_d7.bif || true
bootgen -image boot_d7.bif -arch zynqmp -w -o BOOT_D7.BIN 2>&1 | grep -E "ERROR|WARNING" || true
ls -l BOOT_B2.BIN BOOT_D7.BIN; md5sum BOOT_D7.BIN
echo PACK_DONE
