#!/bin/bash
# D7：BL31 = 批 2 的 atf_b2 + D7 补丁（槽 1 整槽排除 + PQC_SYMKEY 0x8200ff16）。
# 参数与批 1/2 逐字相同（SPD=opteed DEBUG=1 LOG_LEVEL=20）。只落盘，不刷卡。
set -e
source /tools/Xilinx/Vitis/2020.1/settings64.sh
export PATH=/tools/Xilinx/Vitis/2020.1/gnu/aarch64/lin/aarch64-none/bin:$PATH
ATF=/home/build/wdt_patch/atf_d7
OUT=/home/build/wdt_patch/atf_d7_build
cd "$ATF"
make CROSS_COMPILE=aarch64-none-elf- PLAT=zynqmp SPD=opteed DEBUG=1 \
     LOG_LEVEL=20 BUILD_BASE="$OUT" bl31 -j8 2>&1 | tail -5
NEW="$OUT/zynqmp/debug/bl31/bl31.elf"
OLD=/home/build/wdt_patch/atf_b2_build/zynqmp/debug/bl31/bl31.elf
ls -l "$NEW" "$OLD"
echo "== 装载布局（与 b2 对比，起止要一致）"
aarch64-none-elf-readelf -l "$OLD" | grep -A1 LOAD | head -6
echo "  ---- d7 ----"
aarch64-none-elf-readelf -l "$NEW" | grep -A1 LOAD | head -6
echo "== 符号"
for s in pl_rd_ok pl_wr_ok pl_off_max pl_permit; do printf "  %-12s %s\n" $s "$(aarch64-none-elf-nm "$NEW" | grep -c " $s\$" || true)"; done
echo BL31_DONE
