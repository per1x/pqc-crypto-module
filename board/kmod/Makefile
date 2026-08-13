# 交叉编 pmsec.ko（构建机上，用 petalinux 的内核构建产物）
obj-m += pmsec.o
# Ubuntu 的 aarch64-linux-gnu-gcc 默认开 -fstack-protector，而内核里没有
# __stack_chk_guard 这个符号（内核用自己的那套），modpost 会报 undefined。
ccflags-y += -fno-stack-protector
KDIR ?= /home/build/petalinux/build/tmp/work-shared/zynqmp-generic/kernel-build-artifacts
CROSS ?= aarch64-linux-gnu-
all:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=arm64 CROSS_COMPILE=$(CROSS) modules
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=arm64 CROSS_COMPILE=$(CROSS) clean
