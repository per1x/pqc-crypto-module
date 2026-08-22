/* 常量时间原语的统计式验证
 *
 * 源码审计（tools/ct_audit.py）能说明代码里没有数据相关的分支与下标，但说明
 * 不了编译器与流水线实际跑出来是什么样。这个用例按 dudect 的思路直接量执行
 * 时间：构造两类输入 —— 一类固定、一类随机 —— 交错采样大量执行时间，再做
 * Welch t 检验。判据与数据无关时两类样本同分布，t 值应当留在噪声范围内。
 *
 * 【为什么必须带反证】
 * "t 值很小"这件事，一个坏掉的测量装置也能给出来。所以同一套采样与检验会再
 * 跑一遍 leaky_equal() —— 一个故意写成提前退出的比较。它必须被判为有数据依赖；
 * 判不出来就说明这套装置没有区分能力，此时用例失败，而不是报告"通过"。
 *
 * 【为什么还要跑空对照】
 * 第三条腿是空对照：两类输入都取随机。这时即使是 leaky_equal 也应当同分布。
 * 它排除的是"装置对任何东西都报有差异"这种反向失效。
 *
 * 【非 flaky 的做法】
 *   · 门限取得保守（|t| <= 25，dudect 的常用值是 10）；
 *   · 每轮丢掉长尾样本 —— 长尾来自调度与中断，不是被测代码；
 *   · 跑多轮取中位数，单轮被别的进程干扰不会翻盘。
 *
 * 【结论的边界】
 * 本用例量的是本仓库自己的比较原语。liboqs 与 OpenSSL 内部的时序特性不在这里
 * 的量程之内，见 docs/reference/constant-time.md。
 */
#include "testlib.h"
#include "pqchsm/util.h"

#include <math.h>
#include <stdint.h>
#include <time.h>

#define CT_LEN   4096     /* 比较长度：够长，单次耗时远高于计时器分辨率 */
#define SAMPLES  20000    /* 每类每轮的采样数 */
#define ROUNDS   7        /* 轮数，取中位数 */
#define CROP     0.90     /* 丢掉最慢的 10% —— 那是调度噪声 */
#define T_MAX    25.0     /* 判"有数据依赖"的门限 */

static volatile int g_sink;

/* 反证用：故意写成一发现不同就返回。这正是 memcmp 的形状，也正是密钥/tag
 * 比较绝对不能用它的原因。noinline 防止被内联进采样循环后改变形状。 */
__attribute__((noinline))
static int leaky_equal(const void *a, const void *b, size_t n)
{
	const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
	for (size_t i = 0; i < n; i++) {
		if (x[i] != y[i]) {
			return 0;
		}
	}
	return 1;
}

/* ---- 随机与计时 ---------------------------------------------------------- */
static uint64_t g_state = 0x243f6a8885a308d3ULL;

static uint64_t xorshift(void)
{
	g_state ^= g_state << 13;
	g_state ^= g_state >> 7;
	g_state ^= g_state << 17;
	return g_state;
}

static void fill_random(uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		p[i] = (uint8_t)(xorshift() >> 24);
	}
}

static double now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* ---- Welch t 检验 -------------------------------------------------------- */
static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

static void mean_var(const double *v, size_t n, double *mean, double *var)
{
	double m = 0.0;
	for (size_t i = 0; i < n; i++) {
		m += v[i];
	}
	m /= (double)n;
	double s = 0.0;
	for (size_t i = 0; i < n; i++) {
		double d = v[i] - m;
		s += d * d;
	}
	*mean = m;
	*var = n > 1 ? s / (double)(n - 1) : 0.0;
}

/* 两类样本的 Welch t 值。先按合并样本的分位数裁掉长尾：调度与中断只会让某次
 * 测量变慢，不会变快，所以长尾是噪声而不是信号；两类用同一个阈值裁，不引入偏置。 */
static double welch_t(double *c0, size_t n0, double *c1, size_t n1)
{
	size_t total = n0 + n1;
	double *pool = (double *)malloc(total * sizeof(double));
	if (!pool) {
		return 0.0;
	}
	memcpy(pool, c0, n0 * sizeof(double));
	memcpy(pool + n0, c1, n1 * sizeof(double));
	qsort(pool, total, sizeof(double), cmp_double);
	double limit = pool[(size_t)(CROP * (double)total)];
	free(pool);

	double *k0 = (double *)malloc(n0 * sizeof(double));
	double *k1 = (double *)malloc(n1 * sizeof(double));
	if (!k0 || !k1) {
		free(k0);
		free(k1);
		return 0.0;
	}
	size_t m0 = 0, m1 = 0;
	for (size_t i = 0; i < n0; i++) {
		if (c0[i] <= limit) {
			k0[m0++] = c0[i];
		}
	}
	for (size_t i = 0; i < n1; i++) {
		if (c1[i] <= limit) {
			k1[m1++] = c1[i];
		}
	}

	double t = 0.0;
	if (m0 > 100 && m1 > 100) {
		double mean0, var0, mean1, var1;
		mean_var(k0, m0, &mean0, &var0);
		mean_var(k1, m1, &mean1, &var1);
		double denom = sqrt(var0 / (double)m0 + var1 / (double)m1);
		if (denom > 0.0) {
			t = (mean0 - mean1) / denom;
		}
	}
	free(k0);
	free(k1);
	return t;
}

/* ---- 采样 ---------------------------------------------------------------- */
typedef int (*cmp_fn)(const void *, const void *, size_t);

/* 采一轮。两个待比较缓冲都常驻且长度相同，交替访问，缓存状态对两类是对称的；
 * 类别顺序随机，避免频率调节或热漂移被算成类间差异。
 * null_test 非 0 时两类都取随机输入 —— 这时任何实现都不该被判出差异。 */
static double sample_round(cmp_fn fn, int null_test)
{
	static uint8_t a[CT_LEN], b_fix[CT_LEN], b_rnd[CT_LEN];
	double *c0 = (double *)malloc(SAMPLES * sizeof(double));
	double *c1 = (double *)malloc(SAMPLES * sizeof(double));
	if (!c0 || !c1) {
		free(c0);
		free(c1);
		return 0.0;
	}

	fill_random(a, sizeof(a));
	memcpy(b_fix, a, sizeof(a));       /* 第 0 类：与 a 逐字节相同 */
	fill_random(b_rnd, sizeof(b_rnd)); /* 第 1 类：随机 */
	if (null_test) {
		fill_random(b_fix, sizeof(b_fix));
	}

	for (int w = 0; w < 200; w++) {    /* 预热：把缓冲与分支预测器带进稳态 */
		g_sink += fn(a, b_fix, CT_LEN);
		g_sink += fn(a, b_rnd, CT_LEN);
	}

	size_t n0 = 0, n1 = 0;
	while (n0 < SAMPLES || n1 < SAMPLES) {
		int cls = (int)(xorshift() & 1u);
		if (cls == 0 ? n0 >= SAMPLES : n1 >= SAMPLES) {
			cls ^= 1;
		}
		const uint8_t *b = cls ? b_rnd : b_fix;
		double t0 = now_ns();
		g_sink += fn(a, b, CT_LEN);
		double dt = now_ns() - t0;
		if (cls) {
			c1[n1++] = dt;
		} else {
			c0[n0++] = dt;
		}
	}

	double t = welch_t(c0, n0, c1, n1);
	free(c0);
	free(c1);
	return fabs(t);
}

/* 多轮的中位数：单轮被别的进程干扰不影响结论 */
static double median_abs_t(cmp_fn fn, int null_test)
{
	double ts[ROUNDS];
	for (int r = 0; r < ROUNDS; r++) {
		ts[r] = sample_round(fn, null_test);
	}
	qsort(ts, ROUNDS, sizeof(double), cmp_double);
	return ts[ROUNDS / 2];
}

static int ct_equal_wrapper(const void *a, const void *b, size_t n)
{
	return pqc_ct_equal(a, b, n);
}

/* ---- 用例 ---------------------------------------------------------------- */
static void test_correctness(void)
{
	TCASE("pqc_ct_equal 的功能语义");
	uint8_t x[64], y[64];
	fill_random(x, sizeof(x));
	memcpy(y, x, sizeof(x));
	CHECK_EQ_INT(pqc_ct_equal(x, y, sizeof(x)), 1);
	y[63] ^= 0x01;
	CHECK_EQ_INT(pqc_ct_equal(x, y, sizeof(x)), 0);
	y[63] ^= 0x01;
	y[0] ^= 0x80;
	CHECK_EQ_INT(pqc_ct_equal(x, y, sizeof(x)), 0);
	CHECK_EQ_INT(pqc_ct_equal(x, y, 0), 1);          /* 长度 0：无字节可差 */
	CHECK_EQ_INT(pqc_ct_equal(NULL, y, sizeof(x)), 0);
	CHECK_EQ_INT(pqc_ct_equal(x, NULL, sizeof(x)), 0);

	/* 反证用的比较必须在功能上与它等价，否则两者的时序不可比 */
	CHECK_EQ_INT(leaky_equal(x, x, sizeof(x)), 1);
	CHECK_EQ_INT(leaky_equal(x, y, sizeof(x)), 0);
}

static void test_timing(void)
{
	printf("    比较长度 %d 字节，每类每轮 %d 次采样，%d 轮取中位数，"
	       "裁掉最慢的 %d%%\n",
	       CT_LEN, SAMPLES, ROUNDS, (int)lround((1.0 - CROP) * 100));

	double t_leaky = median_abs_t(leaky_equal, 0);
	printf("    反证   leaky_equal（提前退出）     |t| = %8.1f  门限 %.0f\n",
	       t_leaky, T_MAX);
	TCASE("反证：提前退出的比较必须被判为有数据依赖");
	CHECK(t_leaky > T_MAX);

	double t_ct = median_abs_t(ct_equal_wrapper, 0);
	printf("    被测   pqc_ct_equal                |t| = %8.1f  门限 %.0f\n",
	       t_ct, T_MAX);
	TCASE("pqc_ct_equal 的执行时间与被比较的数据无关");
	CHECK(t_ct <= T_MAX);

	/* 空对照：两类都随机。此时连 leaky_equal 都不该被判出差异 ——
	 * 判出来就说明这套装置对任何输入都报"有差异"，前两条结论都不作数。 */
	double t_null_leaky = median_abs_t(leaky_equal, 1);
	double t_null_ct = median_abs_t(ct_equal_wrapper, 1);
	printf("    空对照 两类均随机：leaky |t| = %6.1f   ct |t| = %6.1f\n",
	       t_null_leaky, t_null_ct);
	TCASE("空对照：同分布输入不应被判出差异");
	CHECK(t_null_leaky <= T_MAX);
	CHECK(t_null_ct <= T_MAX);
}

int main(void)
{
	test_use_stub_kdr();   /* 显式装桩：库里没有自动回退了 */
	test_correctness();
	test_timing();
	return test_report("test_ct_timing");
}
