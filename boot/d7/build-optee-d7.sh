#!/bin/bash
# D7：optee_os = 批 2 的 optee_os_b2 + PTA 第二条命令（CMD_SYMKEY → SMC 0x8200ff16）。
# 增量构建：out/ 一起复制过来，命令行若与 b2 一致，只会重编 pqchsm_seed.c 并重链 ——
# 重编的数量本身就是"配置没漂"的判据。
set -e
export PATH=/tools/Xilinx/Vitis/2020.1/gnu/aarch64/lin/aarch64-none/bin:$PATH
cd /home/build/p4_optee
[ -d optee_os_d7 ] || cp -a optee_os_b2 optee_os_d7
python3 /home/build/pqc-fpga/d7/src/boot/optee/patch_optee_seedpta.py /home/build/p4_optee/optee_os_d7
cd optee_os_d7
make -j8 O=out/arm-plat-zynqmp PLATFORM=zynqmp PLATFORM_FLAVOR=zcu102 \
  CROSS_COMPILE64=aarch64-none-elf- CFG_ARM64_core=y CFG_RPMB_FS=n \
  CFG_TEE_CORE_LOG_LEVEL=1 CFG_TEE_TA_LOG_LEVEL=1 CFG_USER_TA_TARGETS=ta_arm64 \
  CFG_ZYNQMP_HUK=n CFG_ZYNQMP_HUK_ASSUME_NO_AUTH=y CFG_CORE_ASLR=n 2>&1 | grep -E "^\s+(CC|LD|GEN)\b|error|Error" | tail -5
echo "== conf.mk 与 b2 的差异（应为空）"
diff ../optee_os_b2/out/arm-plat-zynqmp/conf.mk out/arm-plat-zynqmp/conf.mk && echo "(一致)"
ls -l out/arm-plat-zynqmp/core/tee.bin ../optee_os_b2/out/arm-plat-zynqmp/core/tee.bin
strings out/arm-plat-zynqmp/core/tee.bin | grep -c "pqchsm_seed.pta" || true
echo OPTEE_DONE
