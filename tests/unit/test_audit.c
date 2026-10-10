/* 审计哈希链测试
 *
 * 这里不只验证"自洽"，还**独立重算一遍哈希链**：测试里按 audit.h 声明的公式
 * 自己算 H_0/H_i 并与文件里的逐条比对。否则测试只能证明"实现等于它自己"。
 *
 * 最后两条用例是有意为之的**反向断言**：把 audit.h 里承认的那个洞
 *（能写整个文件的攻击者可以重算整条链并抹平文件头）显式跑出来，断言 verify
 * 返回 0。与其假装没有，不如让它一直在眼前 —— 补上它需要 的
 * "用设备 ML-DSA 身份钥对链头签名固化"，那是本模块之外的事。
 */
#include "testlib.h"
#include "pqchsm/audit.h"
#include "pqchsm/kdf.h"

#include <stdlib.h>
#include <unistd.h>

#define HDR_LEN   64
#define ENTRY_LEN 64
#define REC_LEN   96
#define GENESIS   "pqc-hsm/audit/genesis"

static char g_path[128];

static uint8_t *slurp(const char *p, size_t *n)
{
	FILE *f = fopen(p, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	rewind(f);
	uint8_t *b = malloc((size_t)sz);
	if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
		free(b);
		b = NULL;
	}
	fclose(f);
	*n = (size_t)sz;
	return b;
}

static void spew(const char *p, const uint8_t *b, size_t n)
{
	FILE *f = fopen(p, "wb");
	if (f) {
		fwrite(b, 1, n, f);
		fclose(f);
	}
}

static int mem_contains(const void *hay, size_t hn, const void *needle, size_t nn)
{
	if (nn == 0 || hn < nn) {
		return 0;
	}
	const uint8_t *h = (const uint8_t *)hay;
	for (size_t i = 0; i + nn <= hn; i++) {
		if (memcmp(h + i, needle, nn) == 0) {
			return 1;
		}
	}
	return 0;
}

static uint64_t hdr_count(const uint8_t *f)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; i++) {
		v |= (uint64_t)f[16 + i] << (8 * i);
	}
	return v;
}

/* 按 audit.h 的公式独立重算整条链，写回 out_head */
static void recompute_chain(const uint8_t *f, uint64_t count, uint8_t out_head[32])
{
	uint8_t h[32];
	CHECK_EQ_INT(pqc_sha3_256((const uint8_t *)GENESIS, strlen(GENESIS), h), 0);
	for (uint64_t i = 0; i < count; i++) {
		const uint8_t *entry = f + HDR_LEN + i * REC_LEN;
		uint8_t buf[32 + ENTRY_LEN];
		memcpy(buf, h, 32);
		memcpy(buf + 32, entry, ENTRY_LEN);
		CHECK_EQ_INT(pqc_sha3_256(buf, sizeof(buf), h), 0);
	}
	memcpy(out_head, h, 32);
}

static void fresh(void)
{
	unlink(g_path);
}

/* 写 n 条标准记录 */
static audit_log_t *make_log(int n)
{
	fresh();
	audit_log_t *log = audit_open(g_path);
	if (!log) {
		return NULL;
	}
	for (int i = 0; i < n; i++) {
		char d[64];
		snprintf(d, sizeof(d), "entry-%d", i);
		if (audit_append(log, 1700000000ull + (uint64_t)i, AUDIT_OP_SIGN,
		                 1, (uint32_t)i, 0, d) != 0) {
			audit_close(log);
			return NULL;
		}
	}
	return log;
}

int main(void)
{
	test_use_stub_kdr();   /* 显式装桩：库里没有自动回退了 */
	snprintf(g_path, sizeof(g_path), "/tmp/pqchsm_audit_%d.log", (int)getpid());

	/* ---- 基本追加与读回 ---- */
	TCASE("追加 N 条后链完好，字段读回一致");
	audit_log_t *log = make_log(8);
	CHECK(log != NULL);
	CHECK_EQ_INT(audit_count(log), 8);
	CHECK_EQ_INT(audit_verify_file(g_path, NULL), 0);

	for (uint64_t i = 0; i < 8; i++) {
		uint64_t ts;
		uint32_t op, role, slot, result;
		char detail[AUDIT_DETAIL_LEN + 1];
		CHECK_EQ_INT(audit_read(g_path, i, &ts, &op, &role, &slot, &result, detail), 0);
		CHECK_EQ_INT(ts, 1700000000ull + i);
		CHECK_EQ_INT(op, AUDIT_OP_SIGN);
		CHECK_EQ_INT(role, 1);
		CHECK_EQ_INT(slot, (uint32_t)i);
		CHECK_EQ_INT(result, 0);
		char want[32];
		snprintf(want, sizeof(want), "entry-%llu", (unsigned long long)i);
		CHECK(strcmp(detail, want) == 0);
	}
	CHECK_EQ_INT(audit_read(g_path, 8, NULL, NULL, NULL, NULL, NULL, NULL), -1);

	TCASE("独立重算哈希链：与文件里的链头逐字节相符");
	{
		uint8_t head[AUDIT_HASH_LEN], mine[32];
		CHECK_EQ_INT(audit_head(log, head), 0);
		size_t n = 0;
		uint8_t *f = slurp(g_path, &n);
		CHECK(f != NULL);
		/* 文件大小恒等式 */
		CHECK_EQ_INT(n, HDR_LEN + 8 * REC_LEN);
		CHECK_EQ_INT(hdr_count(f), 8);
		recompute_chain(f, 8, mine);
		CHECK_EQ_MEM(mine, head, 32);
		/* 文件头里的 head 也应当等于它 */
		CHECK_EQ_MEM(f + 24, mine, 32);
		/* 每条记录自带的 H_i 也逐条对 */
		{
			uint8_t h[32];
			CHECK_EQ_INT(pqc_sha3_256((const uint8_t *)GENESIS, strlen(GENESIS), h), 0);
			for (int i = 0; i < 8; i++) {
				uint8_t buf[32 + ENTRY_LEN];
				memcpy(buf, h, 32);
				memcpy(buf + 32, f + HDR_LEN + (size_t)i * REC_LEN, ENTRY_LEN);
				CHECK_EQ_INT(pqc_sha3_256(buf, sizeof(buf), h), 0);
				CHECK_EQ_MEM(f + HDR_LEN + (size_t)i * REC_LEN + ENTRY_LEN, h, 32);
			}
		}
		free(f);
	}
	audit_close(log);

	/* ---- 篡改检测 ---- */
	TCASE("篡改任意一条记录的任意字段 → verify 失败且 bad_seq 指向该条");
	{
		log = make_log(8);
		CHECK(log != NULL);
		audit_close(log);
		size_t n = 0;
		uint8_t *orig = slurp(g_path, &n);
		CHECK(orig != NULL);
		uint8_t *copy = malloc(n);
		int escaped = 0, wrong_seq = 0, checked = 0;
		/* 改第 3 条的每一个 entry 字节 */
		for (int b = 0; b < ENTRY_LEN; b++) {
			memcpy(copy, orig, n);
			copy[HDR_LEN + 3 * REC_LEN + b] ^= 0x01;
			spew(g_path, copy, n);
			uint64_t bad = 999;
			if (audit_verify_file(g_path, &bad) == 0) {
				escaped++;
			} else if (bad != 3) {
				wrong_seq++;
			}
			checked++;
		}
		CHECK_EQ_INT(checked, ENTRY_LEN);
		CHECK_EQ_INT(escaped, 0);
		CHECK_EQ_INT(wrong_seq, 0);

		TCASE("篡改记录自带的哈希字段 → 同样被检出");
		memcpy(copy, orig, n);
		copy[HDR_LEN + 5 * REC_LEN + ENTRY_LEN] ^= 0x80;
		spew(g_path, copy, n);
		{
			uint64_t bad = 999;
			CHECK_EQ_INT(audit_verify_file(g_path, &bad), -1);
			CHECK_EQ_INT(bad, 5);
		}

		TCASE("删掉中间一条 → 失败");
		memcpy(copy, orig, HDR_LEN + 4 * REC_LEN);
		memcpy(copy + HDR_LEN + 4 * REC_LEN, orig + HDR_LEN + 5 * REC_LEN, 3 * REC_LEN);
		spew(g_path, copy, n - REC_LEN);
		CHECK_EQ_INT(audit_verify_file(g_path, NULL), -1);

		TCASE("调换两条的顺序 → 失败");
		memcpy(copy, orig, n);
		memcpy(copy + HDR_LEN + 2 * REC_LEN, orig + HDR_LEN + 6 * REC_LEN, REC_LEN);
		memcpy(copy + HDR_LEN + 6 * REC_LEN, orig + HDR_LEN + 2 * REC_LEN, REC_LEN);
		spew(g_path, copy, n);
		{
			uint64_t bad = 999;
			CHECK_EQ_INT(audit_verify_file(g_path, &bad), -1);
			CHECK_EQ_INT(bad, 2);
		}

		TCASE("截断尾部 → 失败（文件头里的 count 兜住）");
		for (int k = 1; k <= 3; k++) {
			spew(g_path, orig, n - (size_t)k * REC_LEN);
			uint64_t bad = 999;
			CHECK_EQ_INT(audit_verify_file(g_path, &bad), -1);
			CHECK_EQ_INT(bad, (uint64_t)(8 - k));
		}

		TCASE("截断到半条记录 → 失败");
		spew(g_path, orig, n - 17);
		CHECK_EQ_INT(audit_verify_file(g_path, NULL), -1);

		TCASE("朴素的尾部伪造追加（不动文件头）→ 失败");
		{
			uint8_t *big = malloc(n + REC_LEN);
			memcpy(big, orig, n);
			memcpy(big + n, orig + HDR_LEN, REC_LEN);   /* 抄一条老记录贴到尾巴 */
			spew(g_path, big, n + REC_LEN);
			uint64_t bad = 999;
			CHECK_EQ_INT(audit_verify_file(g_path, &bad), -1);
			CHECK_EQ_INT(bad, 8);
			free(big);
		}

		/* ---- 已知洞：反向断言，让它一直可见 ---- */
		TCASE("【已知洞】能重写整个文件的攻击者可以抹平 —— 断言 verify 返回 0");
		{
			/* 砍掉最后 3 条，并把文件头的 count 与 head 改成自洽的值 */
			size_t keep = 5;
			size_t sz = HDR_LEN + keep * REC_LEN;
			uint8_t *forged = malloc(sz);
			memcpy(forged, orig, sz);
			for (int i = 0; i < 8; i++) {
				forged[16 + i] = (uint8_t)(keep >> (8 * i));
			}
			/* 第 keep-1 条记录自带的哈希就是新的链头 */
			memcpy(forged + 24, forged + HDR_LEN + (keep - 1) * REC_LEN + ENTRY_LEN, 32);
			spew(g_path, forged, sz);
			CHECK_EQ_INT(audit_verify_file(g_path, NULL), 0);   /* 洞确实存在 */
			CHECK_EQ_INT(audit_read(g_path, 4, NULL, NULL, NULL, NULL, NULL, NULL), 0);
			CHECK_EQ_INT(audit_read(g_path, 5, NULL, NULL, NULL, NULL, NULL, NULL), -1);
			free(forged);
		}

		free(copy);
		free(orig);
	}

	/* ---- 续写 ---- */
	TCASE("关闭再打开后继续追加，链依然有效");
	{
		log = make_log(4);
		CHECK(log != NULL);
		uint8_t h1[AUDIT_HASH_LEN];
		CHECK_EQ_INT(audit_head(log, h1), 0);
		audit_close(log);

		audit_log_t *l2 = audit_open(g_path);
		CHECK(l2 != NULL);
		CHECK_EQ_INT(audit_count(l2), 4);
		uint8_t h2[AUDIT_HASH_LEN];
		CHECK_EQ_INT(audit_head(l2, h2), 0);
		CHECK_EQ_MEM(h1, h2, AUDIT_HASH_LEN);   /* 重开不改变链头 */
		CHECK_EQ_INT(audit_append(l2, 1800000000ull, AUDIT_OP_ZEROIZE, 2, 7, 0, "after-reopen"), 0);
		CHECK_EQ_INT(audit_count(l2), 5);
		CHECK_EQ_INT(audit_verify_file(g_path, NULL), 0);
		CHECK(memcmp(h1, h2, AUDIT_HASH_LEN) == 0);
		audit_close(l2);

		/* 独立重算也要对得上 */
		size_t n = 0;
		uint8_t *f = slurp(g_path, &n);
		uint8_t mine[32];
		recompute_chain(f, 5, mine);
		CHECK_EQ_MEM(f + 24, mine, 32);
		free(f);
	}

	/* ---- detail 处理 ---- */
	TCASE("detail：NULL、超长截断、非可打印字符净化");
	{
		fresh();
		log = audit_open(g_path);
		CHECK(log != NULL);
		CHECK_EQ_INT(audit_append(log, 1, AUDIT_OP_LOGIN, 1, 0, 0, NULL), 0);
		const char *longd = "0123456789012345678901234567890123456789012345678901234567890123456789";
		CHECK_EQ_INT(audit_append(log, 2, AUDIT_OP_LOGIN, 1, 0, 0, longd), 0);
		const char evil[] = { 'a', 0x1b, '[', '3', '1', 'm', 0x07, 'b', '\0' };
		CHECK_EQ_INT(audit_append(log, 3, AUDIT_OP_LOGIN, 1, 0, 0, evil), 0);
		audit_close(log);
		CHECK_EQ_INT(audit_verify_file(g_path, NULL), 0);

		char d[AUDIT_DETAIL_LEN + 1];
		CHECK_EQ_INT(audit_read(g_path, 0, NULL, NULL, NULL, NULL, NULL, d), 0);
		CHECK_EQ_INT(strlen(d), 0);
		CHECK_EQ_INT(audit_read(g_path, 1, NULL, NULL, NULL, NULL, NULL, d), 0);
		CHECK(strlen(d) <= AUDIT_DETAIL_LEN);           /* 截断而不溢出 */
		CHECK_EQ_INT(audit_read(g_path, 2, NULL, NULL, NULL, NULL, NULL, d), 0);
		/* 控制字符必须已被换成 '.'，防日志阅读器注入 */
		CHECK(strchr(d, 0x1b) == NULL);
		CHECK(strchr(d, 0x07) == NULL);
		CHECK(d[0] == 'a' && d[1] == '.');
	}

	/* ---- 红线 ---- */
	TCASE("红线：API 无记录密钥的入口，日志里搜不到密钥材料");
	{
		uint8_t fake_key[48];
		for (size_t i = 0; i < sizeof(fake_key); i++) {
			fake_key[i] = (uint8_t)(0xC0 ^ i);
		}
		fresh();
		log = audit_open(g_path);
		CHECK(log != NULL);
		for (int i = 0; i < 20; i++) {
			CHECK_EQ_INT(audit_append(log, (uint64_t)i, AUDIT_OP_GENERATE, 2,
			                          (uint32_t)i, 0, "ML-DSA-65"), 0);
		}
		audit_close(log);
		size_t n = 0;
		uint8_t *f = slurp(g_path, &n);
		CHECK(f != NULL);
		CHECK_EQ_INT(mem_contains(f, n, fake_key, sizeof(fake_key)), 0);
		CHECK_EQ_INT(mem_contains(f, n, "so-secret", 9), 0);
		free(f);
	}

	/* ---- 非法参数 ---- */
	TCASE("非法参数");
	{
		CHECK(audit_open(NULL) == NULL);
		CHECK(audit_open("/nonexistent/dir/a.log") == NULL);
		audit_close(NULL);
		CHECK_EQ_INT(audit_append(NULL, 1, AUDIT_OP_LOGIN, 1, 0, 0, "x"), -1);
		CHECK_EQ_INT(audit_head(NULL, NULL), -1);
		CHECK_EQ_INT(audit_count(NULL), 0);
		CHECK_EQ_INT(audit_verify_file("/nonexistent/a.log", NULL), -1);
		CHECK_EQ_INT(audit_verify_file(NULL, NULL), -1);
		CHECK_EQ_INT(audit_read("/nonexistent/a.log", 0, NULL, NULL, NULL, NULL, NULL, NULL), -1);
		/* 空文件 / 只有半个头 */
		spew(g_path, (const uint8_t *)"short", 5);
		CHECK_EQ_INT(audit_verify_file(g_path, NULL), -1);
	}

	unlink(g_path);
	return test_report("test_audit");
}
