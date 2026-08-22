# tee/tests —— TA 密码核的 native（x86_64 Linux/macOS）测试
#
# 同一套 vendor + fips202 + kdf + wrap 源码，用主机 gcc 编成普通 Linux
# 进程跑，是无板阶段最接近 KAT 的验证。OpenSSL 仅作 KMAC/AES-GCM 对拍
# 参照（与被测代码无共享实现）。
#
# 用法：make && ./run_tests

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Werror
CPPFLAGS += -I../ta -I../ta/config -I../include -I../../include

# ---- OpenSSL：只用来做对拍参照，不是被测代码 ----------------------------
# macOS 上它不在默认搜索路径里（brew keg-only），不问 pkg-config 的话
# 这个目标在 mac 上根本编不起来 —— 而 FINAL-PLAN §8 第 9 项正是拿它当
# "纯 host 可验" 的验证手段。问不到就退回裸 -lcrypto（Linux 上够了）。
OSSL_CFLAGS := $(shell pkg-config --cflags libcrypto 2>/dev/null)
OSSL_LIBS   := $(shell pkg-config --libs   libcrypto 2>/dev/null)
CPPFLAGS += $(OSSL_CFLAGS)
LDLIBS   += $(if $(OSSL_LIBS),$(OSSL_LIBS),-lcrypto)

# ---- 新编译器的一条诊断 -------------------------------------------------
# clang 19 / gcc 15 起有 -Wunterminated-string-initialization：
# `uint8_t k[32] = "……32 个字符……"` 会因为放不下结尾的 NUL 而告警。
# 本目录里那几个 KAT 常量**正是**故意不带 NUL 的定长字节串（它们是密钥和
# 盐，不是字符串），所以这条诊断在这里是误报。用探测的方式关掉它：
# 直接写进 CFLAGS 的话，不认识这个选项的老编译器会当场报错。
WNO_UNTERM := $(shell $(CC) -Wno-unterminated-string-initialization -x c -c /dev/null \
                      -o /dev/null 2>/dev/null && echo -Wno-unterminated-string-initialization)
CFLAGS += $(WNO_UNTERM)

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
