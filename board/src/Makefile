# 上板小程序：在构建机上交叉编译、静态链接，拷到板子直接跑
#
#   make CROSS=aarch64-linux-gnu-            # 交叉编译（构建机上）
#   make                                     # 本机编译，只用来看警告
#
# 板上没有 gcc、没有 python3、也没有动态库可依赖，所以一律 -static。
#
# ⚠️ 这里**只收新写的 mldsa_hwtest**。既有的 hsm_hwtest / hsm_kem3 / trngraw
#    那几个各自有自己的编译行（写在各自文件头），而且其中几个要 board/kmod
#    的 uapi 头 —— 把它们一起塞进来等于把一批已经验过的东西重新变成没验过的。
CROSS  ?=
CC      = $(CROSS)gcc
CFLAGS  = -O2 -Wall -Wextra -static

all: mldsa_hwtest

mldsa_hwtest: mldsa_hwtest.c kat_mldsa.h
	$(CC) $(CFLAGS) -o $@ mldsa_hwtest.c

# 向量头文件是**生成物但要提交**的：vectors/ 在 .gitignore 里，而板上没有
# python3 —— 头文件是这批向量在仓库里唯一的形态。
# 重新生成要在有 vectors/ 的机器上跑（tools/fetch_vectors.sh 拉的那份）。
kat: ../../tools/mldsa_kat_to_c.py
	python3 ../../tools/mldsa_kat_to_c.py > kat_mldsa.h

clean:
	rm -f mldsa_hwtest

.PHONY: all kat clean
