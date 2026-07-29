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

#define CHECK_EQ_INT(a, b) do {                                             \
	long _a = (long)(a), _b = (long)(b);                                \
	g_checks++;                                                         \
	if (_a != _b) {                                                     \
		g_fails++;                                                  \
		fprintf(stderr, "FAIL %s:%d [%s] %s == %s (%ld vs %ld)\n",  \
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
