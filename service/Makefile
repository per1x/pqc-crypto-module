# 服务层：daemon + SDF 风格客户端库 + 示例应用
#
# 交叉编译到板子：make CROSS=aarch64-linux-gnu-
CROSS ?=
CC     = $(CROSS)gcc
CFLAGS = -O2 -Wall -Wextra -static

all: pqchsm_fpgad libsdfe.a sdf_demo

pqchsm_fpgad: pqchsm_fpgad.c wire.h ../board/kmod/secmmio_uapi.h
	$(CC) $(CFLAGS) -o $@ pqchsm_fpgad.c

libsdfe.a: libsdfe.c sdfe.h wire.h
	$(CC) $(CFLAGS) -c -o libsdfe.o libsdfe.c
	$(AR) rcs $@ libsdfe.o

# ⚠️ 示例程序只链接 libsdfe，**不链接任何密码库** ——
#    它算不出任何东西，结果只能来自 FPGA。
sdf_demo: sdf_demo.c libsdfe.a sdfe.h
	$(CC) $(CFLAGS) -o $@ sdf_demo.c libsdfe.a

clean:
	rm -f pqchsm_fpgad libsdfe.a libsdfe.o sdf_demo
