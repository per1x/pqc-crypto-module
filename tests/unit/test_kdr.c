/* KDR provider 抽象与派生层级
 *
 * 重点：
 *   1. **域分隔真的生效** —— 不同 label 在同一盐下必须派生出不同子密钥。
 *      这条不成立的话，"存储 KEK"和"元数据密钥"会是同一个值，
 *      整个密钥层级就塌成一层了。
 *   2. **provider 可替换** —— 换 provider 时上层调用点一行不改，
 *      这是 把根密钥迁到 eFUSE/BBRAM/PUF 的前提。
 *   3. **根密钥不出模块** —— 接口里没有 getter；结构性回归由 CTest 的
 *      kdr_no_readback 用例扫头文件保证（见 CMakeLists.txt）。
 */
#include "testlib.h"
#include "pqchsm/kdr.h"
#include "pqchsm/keystore.h"
#include "pqchsm/slot.h"
#include "pqchsm/util.h"

#include <stdlib.h>
#include <unistd.h>

#define SO_PIN   "so-secret-0001"
#define USER_PIN "user-pin-4242"

/* 一个假的硬件 provider，用来验证 provider 分发确实生效 */
static int g_fake_calls;

static int fake_derive(const char *label, const uint8_t *salt, size_t salt_len,
                       uint8_t *out, size_t out_len)
{
	(void)label;
	(void)salt;
	(void)salt_len;
	g_fake_calls++;
	memset(out, 0x5A, out_len);
	return 0;
}

static const pqc_kdr_provider_t g_fake = {
	.name            = "fake-hw",
	.derive          = fake_derive,
	.hardware_backed = 1,
};

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

static void test_provider(void)
{
	TCASE("默认 provider 是桩，且明确标记为无硬件保证");
	const pqc_kdr_provider_t *p = pqc_kdr_get_provider();
	CHECK(p != NULL);
	CHECK(p == pqc_kdr_provider_stub());
	CHECK_EQ_INT(p->hardware_backed, 0);
	CHECK_EQ_INT(pqc_kdr_is_hardware_backed(), 0);
	CHECK(strstr(p->name, "NOT hardware-backed") != NULL);

	TCASE("provider 可替换，pqc_kdr_derive 分发到新实现");
	uint8_t out[32];
	g_fake_calls = 0;
	pqc_kdr_set_provider(&g_fake);
	CHECK_EQ_INT(pqc_kdr_is_hardware_backed(), 1);
	CHECK_EQ_INT(pqc_kdr_derive("any", NULL, 0, out, sizeof(out)), 0);
	CHECK_EQ_INT(g_fake_calls, 1);
	CHECK_EQ_INT(out[0], 0x5A);

	TCASE("挂着硬件 provider 时，set_test_root 不生效（根密钥改不了才是重点）");
	pqc_kdr_set_test_root((const uint8_t *)"whatever", 8);
	g_fake_calls = 0;
	CHECK_EQ_INT(pqc_kdr_derive("any", NULL, 0, out, sizeof(out)), 0);
	CHECK_EQ_INT(g_fake_calls, 1);
	CHECK_EQ_INT(out[0], 0x5A);

	pqc_kdr_set_provider(NULL);
	CHECK(pqc_kdr_get_provider() == pqc_kdr_provider_stub());
	CHECK_EQ_INT(pqc_kdr_is_hardware_backed(), 0);
}

static void test_domain_separation(void)
{
	TCASE("域分隔：同盐不同 label → 子密钥必须不同");
	uint8_t salt[16] = { 1, 2, 3 };
	const char *labels[] = {
		"pqc-hsm/storage-kek",
		"pqc-hsm/keystore-filemac",
		"pqc-hsm/slot-meta-key",
		"pqc-hsm/pin-verifier",
	};
	enum { N = 4 };
	uint8_t k[N][32];
	for (int i = 0; i < N; i++) {
		CHECK_EQ_INT(pqc_kdr_derive(labels[i], salt, sizeof(salt), k[i], 32), 0);
	}
	int collisions = 0;
	for (int i = 0; i < N; i++) {
		for (int j = i + 1; j < N; j++) {
			if (memcmp(k[i], k[j], 32) == 0) {
				collisions++;
			}
		}
	}
	CHECK_EQ_INT(collisions, 0);

	TCASE("同 label 不同盐 → 也必须不同");
	uint8_t a[32], b[32];
	CHECK_EQ_INT(pqc_kdr_derive("x", (const uint8_t *)"s1", 2, a, 32), 0);
	CHECK_EQ_INT(pqc_kdr_derive("x", (const uint8_t *)"s2", 2, b, 32), 0);
	CHECK(memcmp(a, b, 32) != 0);

	TCASE("确定性：同 label 同盐 → 逐字节相同");
	CHECK_EQ_INT(pqc_kdr_derive("x", (const uint8_t *)"s1", 2, b, 32), 0);
	CHECK_EQ_MEM(a, b, 32);

	TCASE("label 为空必须拒绝（否则域分隔形同虚设）");
	CHECK_EQ_INT(pqc_kdr_derive(NULL, salt, sizeof(salt), a, 32), -1);
	CHECK_EQ_INT(pqc_kdr_derive("", salt, sizeof(salt), a, 32), -1);
	CHECK_EQ_INT(pqc_kdr_derive("x", salt, sizeof(salt), NULL, 32), -1);
	CHECK_EQ_INT(pqc_kdr_derive("x", salt, sizeof(salt), a, 0), -1);

	TCASE("换设备根密钥 → 所有派生结果都变");
	uint8_t before[32], after[32];
	CHECK_EQ_INT(pqc_kdr_derive("pqc-hsm/storage-kek", salt, sizeof(salt), before, 32), 0);
	pqc_kdr_set_test_root((const uint8_t *)"device-B", 8);
	CHECK_EQ_INT(pqc_kdr_derive("pqc-hsm/storage-kek", salt, sizeof(salt), after, 32), 0);
	CHECK(memcmp(before, after, 32) != 0);
	pqc_kdr_set_test_root(NULL, 0);
	CHECK_EQ_INT(pqc_kdr_derive("pqc-hsm/storage-kek", salt, sizeof(salt), after, 32), 0);
	CHECK_EQ_MEM(before, after, 32);

	TCASE("派生输出里搜不到桩根密钥的字面量");
	{
		/* 桩根密钥就是这串 ASCII；任何派生输出都不该含有它 */
		const char *stub_lit = "PQC-HSM STUB KDR -- NOT SECRET!!";
		uint8_t big[256];
		CHECK_EQ_INT(pqc_kdr_derive("probe", salt, sizeof(salt), big, sizeof(big)), 0);
		CHECK_EQ_INT(mem_contains(big, sizeof(big), stub_lit, 32), 0);
	}
}

static void test_kek_rotation(void)
{
	TCASE("显式 KEK 轮换：盐与密文全变，内容不变");
	char path[128];
	snprintf(path, sizeof(path), "/tmp/pqchsm_rot_%d.ks", (int)getpid());

	hsm_token_t *tok = hsm_token_new(2);
	CHECK(tok != NULL);
	hsm_session_t s;
	hsm_handle_t h;
	CHECK_EQ_INT(hsm_slot_init_token(tok, 0, "rotate-me", SO_PIN), HSM_OK);
	CHECK_EQ_INT(hsm_session_open(tok, 0, &s), HSM_OK);
	CHECK_EQ_INT(hsm_session_login(tok, s, HSM_ROLE_SO, SO_PIN), HSM_OK);
	CHECK_EQ_INT(hsm_slot_set_user_pin(tok, s, USER_PIN), HSM_OK);
	CHECK_EQ_INT(hsm_session_logout(tok, s), HSM_OK);
	CHECK_EQ_INT(hsm_session_login(tok, s, HSM_ROLE_USER, USER_PIN), HSM_OK);
	CHECK_EQ_INT(hsm_slot_generate(tok, s, PQC_ALG_ML_DSA_65, KEY_USAGE_SIGN, 0, &h), HSM_OK);

	const pqc_alg_info_t *info = pqc_alg_info(PQC_ALG_ML_DSA_65);
	uint8_t *pk = malloc(info->pk_len);
	size_t pk_len = 0;
	CHECK_EQ_INT(hsm_object_public_key(tok, s, h, pk, info->pk_len, &pk_len), HSM_OK);

	CHECK_EQ_INT(hsm_keystore_save(tok, path), HSM_OK);
	size_t n1 = 0, n2 = 0;
	uint8_t *f1 = slurp(path, &n1);
	CHECK(f1 != NULL);

	CHECK_EQ_INT(hsm_keystore_rotate_kek(tok, path), HSM_OK);
	uint8_t *f2 = slurp(path, &n2);
	CHECK(f2 != NULL);
	CHECK_EQ_INT(n1, n2);                       /* 长度不变 */
	CHECK(memcmp(f1 + 16, f2 + 16, 16) != 0);   /* kek_salt 变了 */
	CHECK(memcmp(f1, f2, n1) != 0);             /* 整体密文变了 */
	/* 明文元数据段（每条记录开头 92 字节）应当仍然相同 —— 内容没变 */
	CHECK_EQ_MEM(f1 + 32 + 4, f2 + 32 + 4, 92);

	TCASE("轮换后仍能装载，密钥仍可用");
	{
		hsm_token_t *t2 = hsm_token_new(2);
		CHECK_EQ_INT(hsm_keystore_load(t2, path), HSM_OK);
		hsm_session_t s2;
		CHECK_EQ_INT(hsm_session_open(t2, 0, &s2), HSM_OK);
		CHECK_EQ_INT(hsm_session_login(t2, s2, HSM_ROLE_USER, USER_PIN), HSM_OK);
		slot_meta_t m;
		CHECK_EQ_INT(hsm_slot_get_meta(t2, 0, &m), HSM_OK);
		hsm_handle_t h2 = ((hsm_handle_t)m.generation << 32) | 1;
		uint8_t *sig = malloc(info->sig_len);
		size_t sl = 0;
		const uint8_t msg[] = "after-rotate";
		CHECK_EQ_INT(hsm_object_sign(t2, s2, h2, msg, sizeof(msg), NULL, 0,
		                             sig, info->sig_len, &sl), HSM_OK);
		CHECK_EQ_INT(pqc_verify(PQC_ALG_ML_DSA_65, pk, msg, sizeof(msg), NULL, 0, sig, sl),
		             PQC_OK);
		free(sig);
		hsm_token_free(t2);
	}

	TCASE("多次轮换每次都换新盐");
	{
		uint8_t prev[16];
		memcpy(prev, f2 + 16, 16);
		int same = 0;
		for (int i = 0; i < 5; i++) {
			CHECK_EQ_INT(hsm_keystore_rotate_kek(tok, path), HSM_OK);
			size_t nn = 0;
			uint8_t *ff = slurp(path, &nn);
			if (memcmp(prev, ff + 16, 16) == 0) {
				same++;
			}
			memcpy(prev, ff + 16, 16);
			free(ff);
		}
		CHECK_EQ_INT(same, 0);
	}

	CHECK_EQ_INT(hsm_keystore_rotate_kek(NULL, path), HSM_ERR_BAD_ARG);
	CHECK_EQ_INT(hsm_keystore_rotate_kek(tok, NULL), HSM_ERR_BAD_ARG);

	free(pk);
	free(f1);
	free(f2);
	hsm_token_free(tok);
	unlink(path);
}

int main(void)
{
	test_provider();
	test_domain_separation();
	test_kek_rotation();
	return test_report("test_kdr");
}
