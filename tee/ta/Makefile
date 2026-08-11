# pqc-hsm TA 构建（OP-TEE TA dev kit）
#
# 默认路径指向 WSL 构建机上的 optee_os 导出与 Vitis 工具链，
# 可用 make 变量覆盖：
#   make TA_DEV_KIT_DIR=/path/to/export-ta_arm64 CROSS_COMPILE=aarch64-linux-gnu-

export V ?= 0

# OP-TEE 约定：TA 文件名即 UUID（加载时按文件名查找）
BINARY  ?= 4e2d9c1a-7b35-4f68-9a2c-d15e88406fa3
O       ?= out

TA_DEV_KIT_DIR ?= /home/build/p4_optee/optee_os/out/arm-plat-zynqmp/export-ta_arm64
CROSS_COMPILE  ?= /tools/Xilinx/Vitis/2020.1/gnu/aarch64/lin/aarch64-linux/bin/aarch64-linux-gnu-

.PHONY: all clean

all:
	$(MAKE) -f $(TA_DEV_KIT_DIR)/mk/ta_dev_kit.mk \
		TA_DEV_KIT_DIR=$(TA_DEV_KIT_DIR) \
		CROSS_COMPILE="$(CROSS_COMPILE)" \
		BINARY=$(BINARY) O=$(CURDIR)/$(O) all

clean:
	$(MAKE) -f $(TA_DEV_KIT_DIR)/mk/ta_dev_kit.mk \
		TA_DEV_KIT_DIR=$(TA_DEV_KIT_DIR) \
		CROSS_COMPILE="$(CROSS_COMPILE)" \
		BINARY=$(BINARY) O=$(CURDIR)/$(O) clean
