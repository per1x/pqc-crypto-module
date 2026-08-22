#include "testlib.h"
#include "pqchsm/shamir.h"
#include "pqchsm/util.h"

#include <stdint.h>

/* 每行按最大秘密算，留够 overhead */
#define LINE_CAP (SHAMIR_MAX_SECRET + SHAMIR_SHARE_OVERHEAD)

typedef struct {
	uint8_t buf[SHAMIR_MAX_SHARES][LINE_CAP];
	size_t  lens[SHAMIR_MAX_SHARES];
} share_set;

static uint8_t *rows(share_set *s)
{
	return &s->buf[0][0];
}

/* 从 src 里挑出 idx[] 指定的 k 行，拷成一组连续分片交给 combine */
static void pick(const share_set *src, const int *idx, uint8_t k, share_set *dst)
{
	for (uint8_t i = 0; i < k; i++) {
		memcpy(dst->buf[i], src->buf[idx[i]], LINE_CAP);
		dst->lens[i] = src->lens[idx[i]];
	}
}

/* 造一个可辨认的秘密：不要全 0，也不要单调递增以外的花样 */
static void make_secret(uint8_t *s, size_t n, uint8_t seed)
{
	for (size_t i = 0; i < n; i++) {
		s[i] = (uint8_t)(seed ^ (i * 31u + 7u));
	}
}

/* split → 用 idx 指定的 k 片 combine → 必须逐字节等于原秘密 */
static void roundtrip_ok(const char *what, const uint8_t *secret, size_t secret_len,
                         uint8_t m, uint8_t n, const int *idx, uint8_t k)
{
	share_set all, sub;
	uint8_t got[SHAMIR_MAX_SECRET];
	size_t got_len = 0;

	TCASE(what);
	memset(&all, 0, sizeof(all));
	memset(&sub, 0, sizeof(sub));
	memset(got, 0, sizeof(got));

	CHECK_EQ_INT(shamir_split(secret, secret_len, m, n,
	                          rows(&all), LINE_CAP, all.lens), 0);
	pick(&all, idx, k, &sub);
	CHECK_EQ_INT(shamir_combine(rows(&sub), LINE_CAP, sub.lens, k,
	                            got, sizeof(got), &got_len), 0);
	CHECK_EQ_INT(got_len, (long)secret_len);
	CHECK_EQ_MEM(got, secret, secret_len);
}

int main(void)
{
	test_use_stub_kdr();   /* 显式装桩：库里没有自动回退了 */
	uint8_t secret[SHAMIR_MAX_SECRET];
	uint8_t got[SHAMIR_MAX_SECRET];
	size_t got_len = 0;
	share_set all, sub;

	/* ---------------------------------------------- 3-of-5 穷举 C(5,3)=10 */
	make_secret(secret, 16, 0xA5);
	{
		share_set five;
		int combos = 0;

		TCASE("3-of-5 拆分");
		memset(&five, 0, sizeof(five));
		CHECK_EQ_INT(shamir_split(secret, 16, 3, 5,
		                          rows(&five), LINE_CAP, five.lens), 0);

		TCASE("3-of-5 分片头部与长度");
		for (int i = 0; i < 5; i++) {
			CHECK_EQ_INT(five.lens[i], (long)(16 + SHAMIR_SHARE_OVERHEAD));
			CHECK_EQ_INT(five.buf[i][0], i + 1);   /* 索引 1..5，绝不为 0 */
			CHECK_EQ_INT(five.buf[i][1], 16);
		}

		TCASE("3-of-5 任意 3 片都能恢复（穷举 10 种组合）");
		for (int a = 0; a < 5; a++) {
			for (int b = a + 1; b < 5; b++) {
				for (int c = b + 1; c < 5; c++) {
					int idx[3] = { a, b, c };
					memset(&sub, 0, sizeof(sub));
					memset(got, 0, sizeof(got));
					pick(&five, idx, 3, &sub);
					CHECK_EQ_INT(shamir_combine(rows(&sub), LINE_CAP, sub.lens,
					                            3, got, sizeof(got), &got_len), 0);
					CHECK_EQ_INT(got_len, 16);
					CHECK_EQ_MEM(got, secret, 16);
					combos++;
				}
			}
		}
		CHECK_EQ_INT(combos, 10);

		/* 4 片、5 片（多于门限）同样要对 */
		TCASE("3-of-5 用 4 片 / 5 片恢复");
		{
			int idx4[4] = { 0, 2, 3, 4 };
			int idx5[5] = { 0, 1, 2, 3, 4 };
			memset(&sub, 0, sizeof(sub));
			pick(&five, idx4, 4, &sub);
			CHECK_EQ_INT(shamir_combine(rows(&sub), LINE_CAP, sub.lens, 4,
			                            got, sizeof(got), &got_len), 0);
			CHECK_EQ_MEM(got, secret, 16);
			memset(&sub, 0, sizeof(sub));
			pick(&five, idx5, 5, &sub);
			CHECK_EQ_INT(shamir_combine(rows(&sub), LINE_CAP, sub.lens, 5,
			                            got, sizeof(got), &got_len), 0);
			CHECK_EQ_MEM(got, secret, 16);
		}

		/* ------------------------------------------ 篡改任意一片的任意一字节 */
		TCASE("篡改分片任意一字节 → combine 必须报错");
		{
			size_t line_len = 16 + SHAMIR_SHARE_OVERHEAD;
			for (int victim = 0; victim < 3; victim++) {
				for (size_t pos = 0; pos < line_len; pos++) {
					int idx[3] = { 0, 1, 2 };
					memset(&sub, 0, sizeof(sub));
					pick(&five, idx, 3, &sub);
					sub.buf[victim][pos] ^= 0x40;   /* 翻一位就得被拦住 */
					got_len = 0;
					CHECK(shamir_combine(rows(&sub), LINE_CAP, sub.lens, 3,
					                     got, sizeof(got), &got_len) < 0);
				}
			}
		}

		TCASE("索引重复的两片 → 报错");
		{
			int idx[3] = { 0, 0, 1 };   /* 第 1 片给了两遍 */
			memset(&sub, 0, sizeof(sub));
			pick(&five, idx, 3, &sub);
			CHECK(shamir_combine(rows(&sub), LINE_CAP, sub.lens, 3,
			                     got, sizeof(got), &got_len) < 0);
		}
		TCASE("三片全同 → 报错");
		{
			int idx[3] = { 4, 4, 4 };
			memset(&sub, 0, sizeof(sub));
			pick(&five, idx, 3, &sub);
			CHECK(shamir_combine(rows(&sub), LINE_CAP, sub.lens, 3,
			                     got, sizeof(got), &got_len) < 0);
		}

		TCASE("share_lens 与分片内长度字段不一致 → 报错");
		{
			int idx[3] = { 0, 1, 2 };
			memset(&sub, 0, sizeof(sub));
			pick(&five, idx, 3, &sub);
			sub.lens[1] -= 1;
			CHECK(shamir_combine(rows(&sub), LINE_CAP, sub.lens, 3,
			                     got, sizeof(got), &got_len) < 0);
		}
		TCASE("share_lens 超过 share_cap → 报错");
		{
			int idx[3] = { 0, 1, 2 };
			memset(&sub, 0, sizeof(sub));
			pick(&five, idx, 3, &sub);
			sub.lens[2] = LINE_CAP + 1;
			CHECK(shamir_combine(rows(&sub), LINE_CAP, sub.lens, 3,
			                     got, sizeof(got), &got_len) < 0);
		}
		TCASE("输出缓冲不足 → 报错");
		{
			int idx[3] = { 0, 1, 2 };
			memset(&sub, 0, sizeof(sub));
			pick(&five, idx, 3, &sub);
			CHECK(shamir_combine(rows(&sub), LINE_CAP, sub.lens, 3,
			                     got, 15, &got_len) < 0);
			CHECK_EQ_INT(shamir_combine(rows(&sub), LINE_CAP, sub.lens, 3,
			                            got, 16, &got_len), 0);
		}

		/* ------------------------------------------------ 同一秘密两次 split */
		TCASE("同一秘密两次 split 的分片必须不同（系数随机）");
		{
			share_set again;
			int same = 0;
			memset(&again, 0, sizeof(again));
			CHECK_EQ_INT(shamir_split(secret, 16, 3, 5,
			                          rows(&again), LINE_CAP, again.lens), 0);
			for (int i = 0; i < 5; i++) {
				/* 头部（索引+长度）当然一样，比的是 data 段 */
				if (memcmp(&again.buf[i][2], &five.buf[i][2], 16) == 0) {
					same++;
				}
			}
			CHECK_EQ_INT(same, 0);

			/* 但两批分片各自都能还原出同一个秘密 */
			{
				int idx[3] = { 1, 3, 4 };
				memset(&sub, 0, sizeof(sub));
				pick(&again, idx, 3, &sub);
				CHECK_EQ_INT(shamir_combine(rows(&sub), LINE_CAP, sub.lens, 3,
				                            got, sizeof(got), &got_len), 0);
				CHECK_EQ_MEM(got, secret, 16);
			}
		}
	}

	/* -------------------------------------------------------- 边界门限组合 */
	{
		int i2[2] = { 0, 1 };
		int i3[3] = { 0, 1, 2 };
		int i5[5] = { 0, 1, 2, 3, 4 };
		int last2[2] = { 1, 2 };

		make_secret(secret, 32, 0x3C);
		roundtrip_ok("2-of-2", secret, 32, 2, 2, i2, 2);
		roundtrip_ok("2-of-3（取前两片）", secret, 32, 2, 3, i2, 2);
		roundtrip_ok("2-of-3（取后两片）", secret, 32, 2, 3, last2, 2);
		roundtrip_ok("5-of-5", secret, 32, 5, 5, i5, 5);
		roundtrip_ok("3-of-16（分片数上限）", secret, 32, 3, SHAMIR_MAX_SHARES, i3, 3);
		roundtrip_ok("16-of-16", secret, 32, SHAMIR_MAX_SHARES, SHAMIR_MAX_SHARES,
		             (int[16]){ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
		             SHAMIR_MAX_SHARES);

		make_secret(secret, 1, 0x11);
		roundtrip_ok("1 字节秘密（长度下界）", secret, 1, 2, 3, i2, 2);

		make_secret(secret, SHAMIR_MAX_SECRET, 0x77);
		roundtrip_ok("64 字节秘密（长度上界）", secret, SHAMIR_MAX_SECRET, 3, 5, i3, 3);

		/* 全 0 秘密也得能原样回来（常数项为 0 不是特例） */
		memset(secret, 0, 32);
		roundtrip_ok("全 0 秘密", secret, 32, 3, 5, i3, 3);

		/* 全 0xFF */
		memset(secret, 0xFF, 32);
		roundtrip_ok("全 0xFF 秘密", secret, 32, 2, 4, i2, 2);
	}

	/* ------------------------------------------ m-1 片：得到错值而不是报错 */
	/* 这是 Shamir 的信息论安全，不是错误处理：分片里不存 m，
	 * combine 无从判断门限是否凑够，只能"合法地"插出一个无关的值。
	 * 秘密取 32 字节，撞上原值的概率 2^-256。 */
	TCASE("m-1 片恢复：返回 0 但结果 != 原秘密（Shamir 固有性质）");
	{
		make_secret(secret, 32, 0x5E);
		memset(&all, 0, sizeof(all));
		CHECK_EQ_INT(shamir_split(secret, 32, 3, 5,
		                          rows(&all), LINE_CAP, all.lens), 0);
		{
			int idx[2] = { 0, 1 };
			memset(&sub, 0, sizeof(sub));
			memset(got, 0, sizeof(got));
			pick(&all, idx, 2, &sub);
			/* 注意断言的是"成功返回但值不对"，而不是断言它报错 */
			CHECK_EQ_INT(shamir_combine(rows(&sub), LINE_CAP, sub.lens, 2,
			                            got, sizeof(got), &got_len), 0);
			CHECK_EQ_INT(got_len, 32);
			CHECK(memcmp(got, secret, 32) != 0);
		}
		/* 只给 1 片同理（对 2-of-2 而言 m-1 = 1） */
		memset(&all, 0, sizeof(all));
		CHECK_EQ_INT(shamir_split(secret, 32, 2, 2,
		                          rows(&all), LINE_CAP, all.lens), 0);
		{
			int idx[1] = { 0 };
			memset(&sub, 0, sizeof(sub));
			memset(got, 0, sizeof(got));
			pick(&all, idx, 1, &sub);
			CHECK_EQ_INT(shamir_combine(rows(&sub), LINE_CAP, sub.lens, 1,
			                            got, sizeof(got), &got_len), 0);
			CHECK(memcmp(got, secret, 32) != 0);
		}
	}

	/* -------------------------------------------------- split 非法参数 */
	TCASE("split 非法参数");
	{
		make_secret(secret, 16, 0x01);
		memset(&all, 0, sizeof(all));

		CHECK(shamir_split(NULL, 16, 3, 5, rows(&all), LINE_CAP, all.lens) < 0);
		CHECK(shamir_split(secret, 16, 3, 5, NULL, LINE_CAP, all.lens) < 0);
		CHECK(shamir_split(secret, 16, 3, 5, rows(&all), LINE_CAP, NULL) < 0);

		CHECK(shamir_split(secret, 0, 3, 5, rows(&all), LINE_CAP, all.lens) < 0);
		CHECK(shamir_split(secret, SHAMIR_MAX_SECRET + 1, 3, 5,
		                   rows(&all), LINE_CAP, all.lens) < 0);

		CHECK(shamir_split(secret, 16, 0, 5, rows(&all), LINE_CAP, all.lens) < 0);
		CHECK(shamir_split(secret, 16, 1, 5, rows(&all), LINE_CAP, all.lens) < 0);   /* m < 2 */
		CHECK(shamir_split(secret, 16, 6, 5, rows(&all), LINE_CAP, all.lens) < 0);   /* m > n */
		CHECK(shamir_split(secret, 16, 3, 0, rows(&all), LINE_CAP, all.lens) < 0);
		CHECK(shamir_split(secret, 16, 3, SHAMIR_MAX_SHARES + 1,
		                   rows(&all), LINE_CAP, all.lens) < 0);                     /* n 超上限 */

		/* 缓冲不足：正好差 1 字节 */
		CHECK(shamir_split(secret, 16, 3, 5, rows(&all),
		                   16 + SHAMIR_SHARE_OVERHEAD - 1, all.lens) < 0);
		CHECK(shamir_split(secret, 16, 3, 5, rows(&all), 0, all.lens) < 0);

		/* 边界正好够用则必须成功 */
		CHECK_EQ_INT(shamir_split(secret, 16, 3, 5, rows(&all),
		                          16 + SHAMIR_SHARE_OVERHEAD, all.lens), 0);
		CHECK_EQ_INT(all.lens[4], (long)(16 + SHAMIR_SHARE_OVERHEAD));
	}

	/* ------------------------------------------------ combine 非法参数 */
	TCASE("combine 非法参数");
	{
		make_secret(secret, 16, 0x02);
		memset(&all, 0, sizeof(all));
		CHECK_EQ_INT(shamir_split(secret, 16, 3, 5,
		                          rows(&all), LINE_CAP, all.lens), 0);
		{
			int idx[3] = { 0, 1, 2 };
			memset(&sub, 0, sizeof(sub));
			pick(&all, idx, 3, &sub);

			CHECK(shamir_combine(NULL, LINE_CAP, sub.lens, 3,
			                     got, sizeof(got), &got_len) < 0);
			CHECK(shamir_combine(rows(&sub), LINE_CAP, NULL, 3,
			                     got, sizeof(got), &got_len) < 0);
			CHECK(shamir_combine(rows(&sub), LINE_CAP, sub.lens, 3,
			                     NULL, sizeof(got), &got_len) < 0);
			CHECK(shamir_combine(rows(&sub), LINE_CAP, sub.lens, 3,
			                     got, sizeof(got), NULL) < 0);
			CHECK(shamir_combine(rows(&sub), LINE_CAP, sub.lens, 0,
			                     got, sizeof(got), &got_len) < 0);   /* k = 0 */
			CHECK(shamir_combine(rows(&sub), LINE_CAP, sub.lens,
			                     SHAMIR_MAX_SHARES + 1,
			                     got, sizeof(got), &got_len) < 0);   /* k 超上限 */
			CHECK(shamir_combine(rows(&sub), SHAMIR_SHARE_OVERHEAD, sub.lens, 3,
			                     got, sizeof(got), &got_len) < 0);   /* share_cap 过小 */
		}
		TCASE("索引字段为 0 的分片 → 报错");
		{
			int idx[3] = { 0, 1, 2 };
			memset(&sub, 0, sizeof(sub));
			pick(&all, idx, 3, &sub);
			sub.buf[0][0] = 0;   /* x=0 就是秘密本身，永远非法 */
			CHECK(shamir_combine(rows(&sub), LINE_CAP, sub.lens, 3,
			                     got, sizeof(got), &got_len) < 0);
		}
		TCASE("两组不同秘密长度的分片混用 → 报错");
		{
			share_set other;
			uint8_t other_secret[8];
			make_secret(other_secret, 8, 0x99);
			memset(&other, 0, sizeof(other));
			CHECK_EQ_INT(shamir_split(other_secret, 8, 3, 5,
			                          rows(&other), LINE_CAP, other.lens), 0);
			memset(&sub, 0, sizeof(sub));
			memcpy(sub.buf[0], all.buf[0], LINE_CAP);
			sub.lens[0] = all.lens[0];
			memcpy(sub.buf[1], all.buf[1], LINE_CAP);
			sub.lens[1] = all.lens[1];
			memcpy(sub.buf[2], other.buf[2], LINE_CAP);
			sub.lens[2] = other.lens[2];
			CHECK(shamir_combine(rows(&sub), LINE_CAP, sub.lens, 3,
			                     got, sizeof(got), &got_len) < 0);
		}
	}

	/* ------------------------------------------- 分片长度与 overhead 的关系 */
	TCASE("SHAMIR_SHARE_OVERHEAD 与分片长度的关系");
	{
		CHECK_EQ_INT(SHAMIR_SHARE_OVERHEAD, 6);
		for (size_t len = 1; len <= SHAMIR_MAX_SECRET; len++) {
			make_secret(secret, len, (uint8_t)len);
			memset(&all, 0, sizeof(all));
			CHECK_EQ_INT(shamir_split(secret, len, 2, 3,
			                          rows(&all), LINE_CAP, all.lens), 0);
			for (int i = 0; i < 3; i++) {
				CHECK_EQ_INT(all.lens[i], (long)(len + SHAMIR_SHARE_OVERHEAD));
				CHECK_EQ_INT(all.buf[i][1], (long)len);
			}
			{
				int idx[2] = { 0, 2 };
				memset(&sub, 0, sizeof(sub));
				memset(got, 0, sizeof(got));
				pick(&all, idx, 2, &sub);
				CHECK_EQ_INT(shamir_combine(rows(&sub), LINE_CAP, sub.lens, 2,
				                            got, sizeof(got), &got_len), 0);
				CHECK_EQ_INT(got_len, (long)len);
				CHECK_EQ_MEM(got, secret, len);
			}
		}
	}

	/* ------------------------------------------------------ 秘密不出现在明处 */
	TCASE("分片 data 段不等于秘密本身");
	{
		make_secret(secret, 32, 0x6B);
		memset(&all, 0, sizeof(all));
		CHECK_EQ_INT(shamir_split(secret, 32, 3, 5,
		                          rows(&all), LINE_CAP, all.lens), 0);
		for (int i = 0; i < 5; i++) {
			CHECK(memcmp(&all.buf[i][2], secret, 32) != 0);
		}
	}

	pqc_secure_zero(secret, sizeof(secret));
	pqc_secure_zero(got, sizeof(got));
	return test_report("test_shamir");
}
