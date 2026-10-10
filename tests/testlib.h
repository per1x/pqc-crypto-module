/* testlib.h —— 极简断言框架
 * 不引第三方测试框架：本项目最终要交叉编译到 ARM 裸机/PetaLinux，
 * 依赖越少越好，而 ctest 已经提供了用例组织与并行调度。
 */
#ifndef PQCHSM_TESTLIB_H
#define PQCHSM_TESTLIB_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int  g_checks;
static int  g_fails;
static const char *g_case = "";

#define TCASE(name) do { g_case = (name); } while (0)

#define CHECK(cond) do {                                                    \
	g_checks++;                                                         \
	if (!(cond)) {                                                      \
		g_fails++;                                                  \
		fprintf(stderr, "FAIL %s:%d [%s] %s\n",                     \
		        __FILE__, __LINE__, g_case, #cond);                 \
	}                                                                   \
} while (0)

/* 比较用 long long 而不是 long：long 在 32 位 ABI（armv7l）上只有 4 字节，
 * 拿它去接 uint64_t 的句柄、计数器、时间戳会被截断，于是"高 32 位不同、
 * 低 32 位相同"的两个值会被判成相等 —— 断言变成假通过。 */
#define CHECK_EQ_INT(a, b) do {                                             \
	long long _a = (long long)(a), _b = (long long)(b);                 \
	g_checks++;                                                         \
	if (_a != _b) {                                                     \
		g_fails++;                                                  \
		fprintf(stderr, "FAIL %s:%d [%s] %s == %s (%lld vs %lld)\n", \
		        __FILE__, __LINE__, g_case, #a, #b, _a, _b);        \
	}                                                                   \
} while (0)

#define CHECK_EQ_MEM(a, b, n) do {                                          \
	g_checks++;                                                         \
	if (memcmp((a), (b), (n)) != 0) {                                   \
		g_fails++;                                                  \
		fprintf(stderr, "FAIL %s:%d [%s] bytes differ: %s vs %s\n", \
		        __FILE__, __LINE__, g_case, #a, #b);                \
	}                                                                   \
} while (0)

/* ---- 单元测试的信任根：**显式**装桩 ----------------------------------- */
/* 库里已经没有"没装 provider 就悄悄用桩"那条自动回退了（PS-04），所以这一行
 * 不是仪式：不调它，任何走 KDR 的用例都会在第一次派生时拿到 NULL provider。
 *
 * 写成一个要在 main() 里显式调的具名函数，而不是藏进构造函数或 testlib 的
 * 初始化里 —— 藏起来的话就等于把刚删掉的那个"没有人做过决定"原样搬进测试。
 * 每个 main() 顶上那一行，本身就是"这个进程的根是一个公开常量"的书面记录。 */
#include "pqchsm/kdr.h"
static inline void test_use_stub_kdr(void)
{
	if (pqc_kdr_install_stub() != 0) {
		fprintf(stderr, "装不上桩 KDR（PRODUCTION 形态？）—— 用例无法运行\n");
		exit(2);
	}
}

static inline int test_report(const char *suite)
{
	if (g_fails) {
		fprintf(stderr, "%s: %d/%d 断言失败\n", suite, g_fails, g_checks);
		return 1;
	}
	printf("%s: %d 断言全部通过\n", suite, g_checks);
	return 0;
}

#endif
