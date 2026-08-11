# tee/tests —— TA 密码核的 native（x86_64 Linux/macOS）测试
#
# 同一套 vendor + fips202 + kdf + wrap 源码，用主机 gcc 编成普通 Linux
# 进程跑，是无板阶段最接近 KAT 的验证。OpenSSL 仅作 KMAC/AES-GCM 对拍
# 参照（与被测代码无共享实现）。
#
# 用法：make && ./run_tests

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Werror
CPPFLAGS += -I../ta -I../ta/config -I../include
LDLIBS  += -lcrypto

TA_SRCS := ../ta/ta_fips202.c ../ta/ta_kdf.c ../ta/ta_wrap.c \
           ../ta/ta_pqc.c ../ta/ta_random.c \
           ../ta/ta_mlkem512.c ../ta/ta_mlkem768.c ../ta/ta_mlkem1024.c \
           ../ta/ta_mldsa44.c ../ta/ta_mldsa65.c ../ta/ta_mldsa87.c

TEST_SRCS := test_fips202.c test_kdf.c test_pqc.c test_wrap.c main.c

run_tests: $(TA_SRCS) $(TEST_SRCS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(TA_SRCS) $(TEST_SRCS) $(LDLIBS)

clean:
	rm -f run_tests

.PHONY: clean
