/* zeroize 验证与 红线检查
 *
 * 这个测试**直接窥视槽位的私有结构**（include slot_internal.h），因为
 * "清零真的落到内存上了吗"这件事从公共 API 是看不出来的 —— 而这正是
 * / 要求验证的东西。这类测试写不出来，就是选 C 而不是
 * Python 的直接理由（Python 里 bytes 不可变、到处是副本，无从断言）。
 *
 * 界限（诚实说明）：本测试能证明"结构体内的秘密字段被清零"和
 * "落盘文件里搜不到明文密钥"。它**不能**证明堆上已释放的页、CPU 寄存器、
 * swap 分区里没有残留 —— 那属于 的"断电即失"与介质残留验证，
 * 必须有板子才能做。
 */
#include "testlib.h"
#include "pqchsm/backup.h"
#include "pqchsm/keystore.h"
#include "pqchsm/slot.h"
#include "pqchsm/util.h"

#include "../../src/slot/slot_internal.h"

#include <stdlib.h>
#include <unistd.h>

#define SO_PIN   "so-secret-0001"
#define USER_PIN "user-pin-4242"

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

static int all_zero(const void *p, size_t n)
{
	const uint8_t *b = (const uint8_t *)p;
	for (size_t i = 0; i < n; i++) {
		if (b[i]) {
			return 0;
		}
	}
	return 1;
}

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

/* 建一个装了密钥的槽位；把私钥前 48 字节抄一份出来供后续搜索 */
static hsm_token_t *fixture(uint8_t *sk_probe, size_t probe_len, uint32_t policy)
{
	hsm_token_t *tok = hsm_token_new(2);
	if (!tok) {
		return NULL;
	}
	hsm_session_t s;
	hsm_handle_t h;
	if (hsm_slot_init_token(tok, 0, "victim", SO_PIN) != HSM_OK ||
	    hsm_session_open(tok, 0, &s) != HSM_OK ||
	    hsm_session_login(tok, s, HSM_ROLE_SO, SO_PIN) != HSM_OK ||
	    hsm_slot_set_user_pin(tok, s, USER_PIN) != HSM_OK ||
	    hsm_session_logout(tok, s) != HSM_OK ||
	    hsm_session_login(tok, s, HSM_ROLE_USER, USER_PIN) != HSM_OK ||
	    hsm_slot_generate(tok, s, PQC_ALG_ML_DSA_65, KEY_USAGE_SIGN,
	                      policy | SLOT_POLICY_BACKUPABLE, &h) != HSM_OK) {
		hsm_token_free(tok);
		return NULL;
	}
	hsm_session_close(tok, s);

	slot_t *slot = &tok->slots[0];
	if (policy & SLOT_POLICY_SEED_STORAGE) {
		memcpy(sk_probe, slot->seed, probe_len);
	} else {
		memcpy(sk_probe, slot->sk, probe_len);
	}
	return tok;
}

static void test_struct_wiped(void)
{
	TCASE("zeroize 后槽位结构体里的秘密字段全为 0");
	uint8_t probe[48];
	hsm_token_t *tok = fixture(probe, sizeof(probe), 0);
	CHECK(tok != NULL);
	slot_t *s = &tok->slots[0];

	/* 清零前：确实有东西 */
	CHECK(s->sk != NULL);
	CHECK(!all_zero(s->pin_key, sizeof(s->pin_key)));
	CHECK(!all_zero(s->so_verifier, sizeof(s->so_verifier)));
	CHECK(!all_zero(probe, sizeof(probe)));

	CHECK_EQ_INT(hsm_slot_zeroize_forced(tok, 0), HSM_OK);

	/* 清零后：指针归零、内联秘密字段全 0 */
	CHECK(s->sk == NULL);
	CHECK(s->pk == NULL);
	CHECK_EQ_INT(s->sk_len, 0);
	CHECK_EQ_INT(s->pk_len, 0);
	CHECK_EQ_INT(s->has_seed, 0);
	CHECK_EQ_INT(s->seed_len, 0);
	CHECK(all_zero(s->seed, sizeof(s->seed)));
	CHECK(all_zero(s->pin_key, sizeof(s->pin_key)));
	CHECK(all_zero(s->so_salt, sizeof(s->so_salt)));
	CHECK(all_zero(s->so_verifier, sizeof(s->so_verifier)));
	CHECK(all_zero(s->user_salt, sizeof(s->user_salt)));
	CHECK(all_zero(s->user_verifier, sizeof(s->user_verifier)));
	CHECK_EQ_INT(s->has_so_pin, 0);
	CHECK_EQ_INT(s->has_user_pin, 0);
	/* 整个结构体里搜不到原私钥的任何片段 */
	CHECK_EQ_INT(mem_contains(s, sizeof(*s), probe, sizeof(probe)), 0);
	/* 元数据也清了（除了 slot_id / generation / version 这些非秘密字段） */
	CHECK_EQ_INT(s->meta.alg, PQC_ALG_NONE);
	CHECK_EQ_INT(s->meta.usage, 0);
	CHECK_EQ_INT(s->meta.use_count, 0);
	CHECK(all_zero(s->meta.label, SLOT_LABEL_MAX));

	hsm_token_free(tok);

	TCASE("种子存储槽位：zeroize 后种子也搜不到");
	uint8_t seed_probe[48];
	hsm_token_t *t2 = fixture(seed_probe, sizeof(seed_probe), SLOT_POLICY_SEED_STORAGE);
	CHECK(t2 != NULL);
	slot_t *s2 = &t2->slots[0];
	CHECK_EQ_INT(s2->has_seed, 1);
	CHECK_EQ_INT(hsm_slot_zeroize_forced(t2, 0), HSM_OK);
	CHECK(all_zero(s2->seed, sizeof(s2->seed)));
	CHECK_EQ_INT(mem_contains(s2, sizeof(*s2), seed_probe, sizeof(seed_probe)), 0);
	hsm_token_free(t2);
}

static void test_zeroize_from_every_state(void)
{
	TCASE("zeroize 从任意状态可达，且之后结构体都是干净的");
	for (int st = SLOT_ST_UNINIT; st < SLOT_ST__COUNT; st++) {
		uint8_t probe[48];
		hsm_token_t *tok = fixture(probe, sizeof(probe), 0);
		CHECK(tok != NULL);
		CHECK_EQ_INT(hsm_slot_force_state(tok, 0, (slot_state_t)st), HSM_OK);
		CHECK_EQ_INT(hsm_slot_zeroize_forced(tok, 0), HSM_OK);
		slot_t *s = &tok->slots[0];
		CHECK(s->sk == NULL);
		CHECK(all_zero(s->pin_key, sizeof(s->pin_key)));
		CHECK_EQ_INT(mem_contains(s, sizeof(*s), probe, sizeof(probe)), 0);
		slot_state_t got;
		CHECK_EQ_INT(hsm_slot_get_state(tok, 0, &got), HSM_OK);
		CHECK_EQ_INT(got, SLOT_ST_UNINIT);
		hsm_token_free(tok);
	}
}

static void test_no_plaintext_on_disk(void)
{
	char ks[128], bk[128];
	snprintf(ks, sizeof(ks), "/tmp/pqchsm_z_ks_%d.bin", (int)getpid());
	snprintf(bk, sizeof(bk), "/tmp/pqchsm_z_bk_%d.bin", (int)getpid());

	TCASE("红线：密钥库文件里搜不到明文私钥 / PIN 密钥 / 验证值");
	uint8_t probe[48];
	hsm_token_t *tok = fixture(probe, sizeof(probe), 0);
	CHECK(tok != NULL);
	slot_t *s = &tok->slots[0];
	uint8_t pin_key_copy[32], so_ver_copy[32];
	memcpy(pin_key_copy, s->pin_key, sizeof(pin_key_copy));
	memcpy(so_ver_copy, s->so_verifier, sizeof(so_ver_copy));

	CHECK_EQ_INT(hsm_keystore_save(tok, ks), HSM_OK);
	{
		size_t n = 0;
		uint8_t *f = slurp(ks, &n);
		CHECK(f != NULL);
		CHECK(n > 0);
		CHECK_EQ_INT(mem_contains(f, n, probe, sizeof(probe)), 0);
		CHECK_EQ_INT(mem_contains(f, n, pin_key_copy, sizeof(pin_key_copy)), 0);
		CHECK_EQ_INT(mem_contains(f, n, so_ver_copy, sizeof(so_ver_copy)), 0);
		/* 公钥也在包裹内，同样搜不到 */
		CHECK_EQ_INT(mem_contains(f, n, s->pk, 48), 0);
		free(f);
	}

	TCASE("红线：备份文件里同样搜不到");
	{
		hsm_session_t so;
		CHECK_EQ_INT(hsm_session_open(tok, 0, &so), HSM_OK);
		CHECK_EQ_INT(hsm_session_login(tok, so, HSM_ROLE_SO, SO_PIN), HSM_OK);
		uint8_t shares[5 * HSM_SHARE_CAP];
		size_t lens[5];
		CHECK_EQ_INT(hsm_backup_export(tok, so, bk, 3, 5, shares, HSM_SHARE_CAP, lens, NULL),
		             HSM_OK);
		size_t n = 0;
		uint8_t *f = slurp(bk, &n);
		CHECK(f != NULL);
		CHECK_EQ_INT(mem_contains(f, n, probe, sizeof(probe)), 0);
		CHECK_EQ_INT(mem_contains(f, n, pin_key_copy, sizeof(pin_key_copy)), 0);
		free(f);
		/* 分片里也不能直接出现 RMK 派生物之外的东西 —— 至少不能有私钥 */
		CHECK_EQ_INT(mem_contains(shares, sizeof(shares), probe, sizeof(probe)), 0);
		hsm_session_close(tok, so);
	}

	TCASE("清零后重新落盘：文件里彻底没有旧密钥");
	CHECK_EQ_INT(hsm_slot_zeroize_forced(tok, 0), HSM_OK);
	CHECK_EQ_INT(hsm_keystore_save(tok, ks), HSM_OK);
	{
		size_t n = 0;
		uint8_t *f = slurp(ks, &n);
		CHECK(f != NULL);
		CHECK_EQ_INT(mem_contains(f, n, probe, sizeof(probe)), 0);
		free(f);
		/* 重新装载得到的是一个 UNINIT 槽位，不是旧密钥 */
		hsm_token_t *t2 = hsm_token_new(2);
		CHECK_EQ_INT(hsm_keystore_load(t2, ks), HSM_OK);
		slot_meta_t m;
		CHECK_EQ_INT(hsm_slot_get_meta(t2, 0, &m), HSM_OK);
		CHECK_EQ_INT(m.state, SLOT_ST_UNINIT);
		CHECK_EQ_INT(m.alg, PQC_ALG_NONE);
		CHECK(all_zero(t2->slots[0].pin_key, 32));
		CHECK_EQ_INT(mem_contains(&t2->slots[0], sizeof(slot_t), probe, sizeof(probe)), 0);
		hsm_token_free(t2);
	}

	hsm_token_free(tok);
	unlink(ks);
	unlink(bk);
}

static void test_secure_alloc(void)
{
	TCASE("pqc_secure_alloc 返回清零内存；secure_zero 真的写下去了");
	uint8_t *p = pqc_secure_alloc(4096);
	CHECK(p != NULL);
	CHECK(all_zero(p, 4096));
	memset(p, 0xA5, 4096);
	CHECK(!all_zero(p, 4096));
	pqc_secure_zero(p, 4096);
	CHECK(all_zero(p, 4096));
	pqc_secure_free(p, 4096);
	CHECK(pqc_secure_alloc(0) == NULL);
	pqc_secure_free(NULL, 0);   /* 不应崩溃 */
}

int main(void)
{
	test_struct_wiped();
	test_zeroize_from_every_state();
	test_no_plaintext_on_disk();
	test_secure_alloc();
	return test_report("test_zeroize");
}
