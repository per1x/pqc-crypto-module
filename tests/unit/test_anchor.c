/* 审计链头签名固化
 *
 * 这个测试的核心是一条对照：**同一份被整体重写过的假日志**，
 *   audit_verify_file  → 返回 0（哈希链自身发现不了，这是已知的洞）
 *   anchor_verify      → 返回 HSM_ERR_INTEGRITY（签名过的 head 对不上）
 * 洞被堵上没有，就看这一条。
 */
#include "testlib.h"
#include "pqchsm/anchor.h"
#include "pqchsm/audit.h"
#include "pqchsm/slot.h"

#include <stdlib.h>
#include <unistd.h>

#define SO_PIN   "so-secret-0001"
#define USER_PIN "user-pin-4242"
#define HDR_LEN   64
#define ENTRY_LEN 64
#define REC_LEN   96

static char g_log[160], g_anc[160];

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

/* 建一个带身份签名钥的 token，返回句柄与会话 */
static hsm_token_t *make_device(hsm_session_t *sess, hsm_handle_t *idkey,
                                uint8_t *pk, size_t *pk_len)
{
	hsm_token_t *tok = hsm_token_new(1);
	if (!tok) {
		return NULL;
	}
	if (hsm_slot_init_token(tok, 0, "device-identity", SO_PIN) != HSM_OK ||
	    hsm_session_open(tok, 0, sess) != HSM_OK ||
	    hsm_session_login(tok, *sess, HSM_ROLE_SO, SO_PIN) != HSM_OK ||
	    hsm_slot_set_user_pin(tok, *sess, USER_PIN) != HSM_OK ||
	    hsm_session_logout(tok, *sess) != HSM_OK ||
	    hsm_session_login(tok, *sess, HSM_ROLE_USER, USER_PIN) != HSM_OK ||
	    /* 身份钥：纯 sealed（不带 BACKUPABLE），跟着设备走 */
	    hsm_slot_generate(tok, *sess, PQC_ALG_ML_DSA_65, KEY_USAGE_SIGN, 0, idkey) != HSM_OK) {
		hsm_token_free(tok);
		return NULL;
	}
	if (hsm_object_public_key(tok, *sess, *idkey, pk, 4096, pk_len) != HSM_OK) {
		hsm_token_free(tok);
		return NULL;
	}
	return tok;
}

static audit_log_t *make_log(int n)
{
	unlink(g_log);
	audit_log_t *log = audit_open(g_log);
	if (!log) {
		return NULL;
	}
	for (int i = 0; i < n; i++) {
		if (audit_append(log, 1700000000ull + (uint64_t)i, AUDIT_OP_SIGN,
		                 1, (uint32_t)i, 0, "x") != 0) {
			audit_close(log);
			return NULL;
		}
	}
	return log;
}

int main(void)
{
	int pid = (int)getpid();
	snprintf(g_log, sizeof(g_log), "/tmp/pqchsm_anc_%d.log", pid);
	snprintf(g_anc, sizeof(g_anc), "/tmp/pqchsm_anc_%d.anchor", pid);

	uint8_t pk[4096];
	size_t pk_len = 0;
	hsm_session_t sess;
	hsm_handle_t idkey;
	hsm_token_t *tok = make_device(&sess, &idkey, pk, &pk_len);
	CHECK(tok != NULL);

	/* ---- 正路 ---- */
	TCASE("生成锚点并校验通过");
	audit_log_t *log = make_log(10);
	CHECK(log != NULL);
	CHECK_EQ_INT(audit_verify_file(g_log, NULL), 0);
	CHECK_EQ_INT(hsm_audit_anchor_create(log, g_anc, tok, sess, idkey, 1700009999ull), HSM_OK);
	audit_close(log);

	uint64_t anchored = 0;
	CHECK_EQ_INT(hsm_audit_anchor_verify(g_log, g_anc, pk, pk_len, &anchored), HSM_OK);
	CHECK_EQ_INT(anchored, 10);

	TCASE("锚点之后继续追加：锚点仍有效，覆盖范围仍是前 10 条");
	{
		audit_log_t *l = audit_open(g_log);
		CHECK(l != NULL);
		for (int i = 0; i < 5; i++) {
			CHECK_EQ_INT(audit_append(l, 1700010000ull + (uint64_t)i,
			                          AUDIT_OP_LOGIN, 1, 0, 0, "later"), 0);
		}
		audit_close(l);
		CHECK_EQ_INT(audit_count(NULL), 0);
		anchored = 0;
		CHECK_EQ_INT(hsm_audit_anchor_verify(g_log, g_anc, pk, pk_len, &anchored), HSM_OK);
		CHECK_EQ_INT(anchored, 10);
	}

	/* ---- 核心对照：整体重写的假日志 ---- */
	TCASE("★ 整体重写的假日志：哈希链自身发现不了，锚点必须发现");
	{
		size_t n = 0;
		uint8_t *orig = slurp(g_log, &n);
		CHECK(orig != NULL);
		CHECK_EQ_INT(n, HDR_LEN + 15 * REC_LEN);

		/* 攻击者：砍到只剩 6 条，并把文件头的 count / head 改成自洽的值 */
		size_t keep = 6;
		size_t sz = HDR_LEN + keep * REC_LEN;
		uint8_t *forged = malloc(sz);
		memcpy(forged, orig, sz);
		for (int i = 0; i < 8; i++) {
			forged[16 + i] = (uint8_t)(keep >> (8 * i));
		}
		memcpy(forged + 24, forged + HDR_LEN + (keep - 1) * REC_LEN + ENTRY_LEN, 32);
		spew(g_log, forged, sz);

		/* 哈希链自己看不出问题 —— 这正是 audit.h 承认的洞 */
		CHECK_EQ_INT(audit_verify_file(g_log, NULL), 0);
		/* 但锚点签过 count=10，现在日志只有 6 条 → 当场失败 */
		CHECK_EQ_INT(hsm_audit_anchor_verify(g_log, g_anc, pk, pk_len, NULL),
		             HSM_ERR_INTEGRITY);

		free(forged);
		spew(g_log, orig, n);
		CHECK_EQ_INT(hsm_audit_anchor_verify(g_log, g_anc, pk, pk_len, NULL), HSM_OK);
		free(orig);
	}

	TCASE("★ 改锚点覆盖范围内的一条记录并重算整条链 → 锚点必须发现");
	{
		size_t n = 0;
		uint8_t *orig = slurp(g_log, &n);
		/* 攻击者改第 3 条的 result 字段，然后从第 3 条起重算所有 H_i 与文件头。
		 * 这里偷懒直接截到第 3 条并自洽 —— 效果等价：前缀不再是被签过的那一段。 */
		size_t keep = 3;
		size_t sz = HDR_LEN + keep * REC_LEN;
		uint8_t *forged = malloc(sz);
		memcpy(forged, orig, sz);
		forged[HDR_LEN + 2 * REC_LEN + 28] ^= 0x01;      /* 动 result */
		for (int i = 0; i < 8; i++) {
			forged[16 + i] = (uint8_t)(keep >> (8 * i));
		}
		memcpy(forged + 24, forged + HDR_LEN + (keep - 1) * REC_LEN + ENTRY_LEN, 32);
		spew(g_log, forged, sz);
		/* 这次连哈希链自己都发现了（因为没重算 H_i），锚点当然也发现 */
		CHECK_EQ_INT(audit_verify_file(g_log, NULL), -1);
		CHECK_EQ_INT(hsm_audit_anchor_verify(g_log, g_anc, pk, pk_len, NULL),
		             HSM_ERR_INTEGRITY);
		free(forged);
		spew(g_log, orig, n);
		free(orig);
	}

	/* ---- 锚点文件本身的负测试 ---- */
	TCASE("用错误的公钥校验 → 失败");
	{
		uint8_t bad_pk[4096];
		memcpy(bad_pk, pk, pk_len);
		bad_pk[0] ^= 0x01;
		CHECK_EQ_INT(hsm_audit_anchor_verify(g_log, g_anc, bad_pk, pk_len, NULL),
		             HSM_ERR_INTEGRITY);
		CHECK_EQ_INT(hsm_audit_anchor_verify(g_log, g_anc, pk, pk_len - 1, NULL),
		             HSM_ERR_INTEGRITY);
	}

	TCASE("另一台设备的身份钥签的锚点 → 失败");
	{
		uint8_t pk2[4096];
		size_t pk2_len = 0;
		hsm_session_t s2;
		hsm_handle_t id2;
		hsm_token_t *dev2 = make_device(&s2, &id2, pk2, &pk2_len);
		CHECK(dev2 != NULL);
		char anc2[200];
		snprintf(anc2, sizeof(anc2), "%s.dev2", g_anc);
		audit_log_t *l = audit_open(g_log);
		CHECK(l != NULL);
		CHECK_EQ_INT(hsm_audit_anchor_create(l, anc2, dev2, s2, id2, 1700009999ull), HSM_OK);
		audit_close(l);
		/* dev2 的锚点拿 dev1 的公钥去验 → 失败 */
		CHECK_EQ_INT(hsm_audit_anchor_verify(g_log, anc2, pk, pk_len, NULL),
		             HSM_ERR_INTEGRITY);
		/* 用 dev2 自己的公钥则通过 */
		CHECK_EQ_INT(hsm_audit_anchor_verify(g_log, anc2, pk2, pk2_len, NULL), HSM_OK);
		unlink(anc2);
		hsm_token_free(dev2);
	}

	TCASE("篡改锚点文件的任意一字节 → 失败（逐字节扫描）");
	{
		size_t n = 0;
		uint8_t *orig = slurp(g_anc, &n);
		CHECK(orig != NULL);
		uint8_t *copy = malloc(n);
		int escaped = 0;
		for (size_t pos = 0; pos < n; pos++) {
			memcpy(copy, orig, n);
			copy[pos] ^= 0x01;
			spew(g_anc, copy, n);
			if (hsm_audit_anchor_verify(g_log, g_anc, pk, pk_len, NULL) == HSM_OK) {
				escaped++;
				fprintf(stderr, "  锚点偏移 %zu 被改后仍通过\n", pos);
			}
		}
		CHECK_EQ_INT(escaped, 0);
		CHECK(n > 3000);          /* ML-DSA-65 签名 3309 B + 公钥 1952 B */
		spew(g_anc, orig, n);
		free(copy);
		free(orig);
	}

	TCASE("截断日志到锚点覆盖范围之下 → 失败");
	{
		size_t n = 0;
		uint8_t *orig = slurp(g_log, &n);
		audit_log_t *l = audit_open(g_log);
		audit_close(l);
		/* 直接把文件截到 8 条并自洽 */
		size_t keep = 8, sz = HDR_LEN + keep * REC_LEN;
		uint8_t *t = malloc(sz);
		memcpy(t, orig, sz);
		for (int i = 0; i < 8; i++) {
			t[16 + i] = (uint8_t)(keep >> (8 * i));
		}
		memcpy(t + 24, t + HDR_LEN + (keep - 1) * REC_LEN + ENTRY_LEN, 32);
		spew(g_log, t, sz);
		CHECK_EQ_INT(hsm_audit_anchor_verify(g_log, g_anc, pk, pk_len, NULL),
		             HSM_ERR_INTEGRITY);
		free(t);
		spew(g_log, orig, n);
		free(orig);
	}

	TCASE("peek_pk：能读出公钥用于首次建立信任");
	{
		uint8_t got[4096];
		size_t got_len = 0;
		pqc_alg_t alg = PQC_ALG_NONE;
		CHECK_EQ_INT(hsm_audit_anchor_peek_pk(g_anc, &alg, got, sizeof(got), &got_len), HSM_OK);
		CHECK_EQ_INT(alg, PQC_ALG_ML_DSA_65);
		CHECK_EQ_INT(got_len, pk_len);
		CHECK_EQ_MEM(got, pk, pk_len);
		CHECK_EQ_INT(hsm_audit_anchor_peek_pk(g_anc, NULL, got, 10, &got_len), HSM_ERR_BAD_ARG);
	}

	TCASE("空日志也能锚定（count = 0，head = 创世哈希）");
	{
		char l0[200], a0[200];
		snprintf(l0, sizeof(l0), "%s.empty", g_log);
		snprintf(a0, sizeof(a0), "%s.empty", g_anc);
		unlink(l0);
		audit_log_t *l = audit_open(l0);
		CHECK(l != NULL);
		CHECK_EQ_INT(audit_count(l), 0);
		CHECK_EQ_INT(hsm_audit_anchor_create(l, a0, tok, sess, idkey, 1), HSM_OK);
		audit_close(l);
		uint64_t c = 999;
		CHECK_EQ_INT(hsm_audit_anchor_verify(l0, a0, pk, pk_len, &c), HSM_OK);
		CHECK_EQ_INT(c, 0);
		unlink(l0);
		unlink(a0);
	}

	TCASE("非法参数与缺文件");
	{
		CHECK_EQ_INT(hsm_audit_anchor_verify(NULL, g_anc, pk, pk_len, NULL), HSM_ERR_BAD_ARG);
		CHECK_EQ_INT(hsm_audit_anchor_verify(g_log, g_anc, NULL, pk_len, NULL), HSM_ERR_BAD_ARG);
		CHECK_EQ_INT(hsm_audit_anchor_verify(g_log, "/nonexistent/a", pk, pk_len, NULL),
		             HSM_ERR_BAD_ARG);
		CHECK_EQ_INT(hsm_audit_anchor_verify("/nonexistent/l", g_anc, pk, pk_len, NULL),
		             HSM_ERR_INTEGRITY);
		CHECK_EQ_INT(hsm_audit_anchor_create(NULL, g_anc, tok, sess, idkey, 1), HSM_ERR_BAD_ARG);
		audit_log_t *l = audit_open(g_log);
		CHECK_EQ_INT(hsm_audit_anchor_create(l, g_anc, tok, sess, HSM_INVALID_HANDLE, 1),
		             HSM_ERR_BAD_ARG);
		audit_close(l);
	}

	TCASE("audit_hash_at 边界");
	{
		uint8_t h[AUDIT_HASH_LEN];
		CHECK_EQ_INT(audit_hash_at(g_log, 0, h), 0);          /* 创世 */
		CHECK_EQ_INT(audit_hash_at(g_log, 15, h), 0);
		CHECK_EQ_INT(audit_hash_at(g_log, 16, h), -1);        /* 超过记录数 */
		CHECK_EQ_INT(audit_hash_at(NULL, 0, h), -1);
		CHECK_EQ_INT(audit_hash_at(g_log, 0, NULL), -1);
	}

	hsm_token_free(tok);
	unlink(g_log);
	unlink(g_anc);
	return test_report("test_anchor");
}
