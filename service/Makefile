# 服务层：daemon + SDF 风格客户端库 + 示例应用
#
# 交叉编译到板子：
#     make CROSS=aarch64-linux-gnu- SYSROOT=<目标 sysroot> SSLROOT=<openssl 组件目录>
#
# ⚠️ 远程口是 mTLS（见 pqcs_tls.h），所以 daemon 与客户端都要链 OpenSSL。
#    板上没有 openssl 的共享库，也没有 openssl 命令行 —— 全部静态链接，
#    二进制拷过去就能跑（这也是原来 -static 的理由，没有改变）。
#
# 【两个 root 是两件事，别合成一个】
#   SYSROOT —— 目标的 **libc**（libc.a / libpthread.a / 头文件）。
#              Xilinx 2020.1 的 aarch64-linux-gnu-gcc **自带的那份没有静态库**，
#              所以 -static 会以 `cannot find -lc` 失败。要显式指到 PetaLinux
#              的镜像 sysroot：
#                petalinux/build/tmp/work/zynqmp_generic-xilinx-linux/\
#                  petalinux-image-minimal/1.0-r0/recipe-sysroot
#   SSLROOT —— 交叉编好的 **OpenSSL**。PetaLinux 2020.1 的那份在
#                petalinux/build/tmp/sysroots-components/aarch64/openssl/usr/{include,lib}
#              它**不在**上面那个 sysroot 里，所以必须分开给。
#
# 【为什么给板子的那版不是全静态 —— 2026-08-18 实测，别改回去】
# 原来 daemon 是 -static 的，好处是"拷过去就能跑，不用管板上的 libc"。
# 加 OpenSSL 之后**静态链接会在板上段错误，而且崩在 main 之前**：
# 自己挂一个 __attribute__((constructor)) 打一行字都执行不到，
# 只要 libssl.a 进链接就必崩（只链 libcrypto.a 不崩）。
# 判据是拿 write(1,...) 打的标记 —— 用 printf 的话会与 stdio 缓冲没刷出来混淆。
#
# 这是这份预编译 OpenSSL 1.1.1d 与这条工具链的打包问题，不是本项目的代码问题；
# 而 PetaLinux 本来就是按**共享库**来打包 OpenSSL 的。所以给板子的那版改成动态链，
# 并用 -rpath 把查找路径钉在 SD 上那个目录：
#
#     BOARD_SSL_DIR = /media/sd-mmcblk1p2/hsm/lib   （放 libssl.so.1.1 / libcrypto.so.1.1）
#
# 用 rpath 而不是 LD_LIBRARY_PATH：二进制自己知道去哪儿找，
# 无论谁怎么把它拉起来（hsm-boot.sh、ssh、手敲）都一样，
# 少一个"忘了设环境变量"的失败模式 —— 而那个失败模式的症状是开机没有远程口。
CROSS   ?=
SYSROOT ?=
SSLROOT ?=
CC       = $(CROSS)gcc
UNAME   := $(shell uname -s)

# -static 只在 Linux 上给：macOS 不支持全静态链接（没有 crt0.o，
# 链接器会直接报错）。板子那侧是 Linux，静态仍然照旧。
BOARD_SSL_DIR ?= /media/sd-mmcblk1p2/hsm/lib

ifeq ($(UNAME),Darwin)
STATIC =
else ifneq ($(SSLROOT),)
STATIC =          # 交给板子的那版：动态链 OpenSSL，理由见上
else
STATIC = -static
endif
CFLAGS   = -O2 -Wall -Wextra $(STATIC)
ifneq ($(SYSROOT),)
CFLAGS  += --sysroot=$(SYSROOT)
endif

SSL_CFLAGS =
SSL_LIBS   = -lssl -lcrypto
SSL_EXTRA  =

ifneq ($(SSLROOT),)
  # 交叉：动态链 SSLROOT 里的 OpenSSL，运行时从板上的 BOARD_SSL_DIR 找。
  # -rpath-link 只影响链接期的依赖解析，-rpath 才是写进二进制里的运行时路径。
  SSL_CFLAGS = -I$(SSLROOT)/usr/include
  SSL_LIBS   = -L$(SSLROOT)/usr/lib -lssl -lcrypto \
               -Wl,-rpath-link,$(SSLROOT)/usr/lib \
               -Wl,-rpath,$(BOARD_SSL_DIR)
  SSL_EXTRA  = -lpthread -ldl
else ifeq ($(UNAME),Darwin)
  # macOS 自带的是 LibreSSL，头文件不全且没有 TLS 1.3 的那套 API ——
  # 用 Homebrew 的 openssl@3。找不到就让编译当场失败并说清楚，
  # 不要退回系统那份（那会在链接期报一堆看不懂的符号错）。
  BREW_SSL := $(shell brew --prefix openssl@3 2>/dev/null)
  SSL_CFLAGS = -I$(BREW_SSL)/include
  SSL_LIBS   = -L$(BREW_SSL)/lib -lssl -lcrypto
else
  SSL_EXTRA  = -lpthread -ldl
endif

SSL_LIBS += $(SSL_EXTRA)

all: pqchsm_fpgad libsdfe.a sdf_demo

pqchsm_fpgad: pqchsm_fpgad.c pqcs_tls.c pqcs_tls.h wire.h ../board/kmod/secmmio_uapi.h
	$(CC) $(CFLAGS) $(SSL_CFLAGS) -o $@ pqchsm_fpgad.c pqcs_tls.c $(SSL_LIBS)

libsdfe.a: libsdfe.c pqcs_tls.c sdfe.h wire.h pqcs_tls.h
	$(CC) $(CFLAGS) $(SSL_CFLAGS) -c -o libsdfe.o libsdfe.c
	$(CC) $(CFLAGS) $(SSL_CFLAGS) -c -o pqcs_tls.o pqcs_tls.c
	$(AR) rcs $@ libsdfe.o pqcs_tls.o

# ⚠️ 示例程序只链接 libsdfe（与 OpenSSL），**不链接任何密码库** ——
#    它算不出任何 PQC/分组密码的结果，那些数值只能来自 FPGA。
#    OpenSSL 在这里只做 TLS 传输，不参与任何被演示的运算。
sdf_demo: sdf_demo.c libsdfe.a sdfe.h
	$(CC) $(CFLAGS) $(SSL_CFLAGS) -o $@ sdf_demo.c libsdfe.a $(SSL_LIBS)

clean:
	rm -f pqchsm_fpgad libsdfe.a libsdfe.o pqcs_tls.o sdf_demo
