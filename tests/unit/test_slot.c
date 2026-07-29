/* 槽位管理器集成测试：ACL、PIN 锁定、句柄、用途互斥、种子存储、zeroize
 * 对应路线图 Phase 5 的产出/验证清单。
 */
#include "testlib.h"
#include "pqchsm/slot.h"
#include "pqchsm/util.h"

#include <stdlib.h>

#define SO_PIN   "so-secret-0001"
#define USER_PIN "user-pin-4242"

/* 建一个已 init_token、已设 User PIN 的槽位，返回打开的会话 */
static hsm_token_t *fixture(hsm_session_t *sess)
{
	hsm_token_t *tok = hsm_token_new(4);
	if (!tok) {
		return NULL;
	}
	if (hsm_slot_init_token(tok, 0, "slot-0", SO_PIN) != HSM_OK) {
		hsm_token_free(tok);
		return NULL;
	}
	if (hsm_session_open(tok, 0, sess) != HSM_OK) {
		hsm_token_free(tok);
		return NULL;
	}
	if (hsm_session_login(tok, *sess, HSM_ROLE_SO, SO_PIN) != HSM_OK ||
	    hsm_slot_set_user_pin(tok, *sess, USER_PIN) != HSM_OK) {
		hsm_token_free(tok);
		return NULL;
	}
	return tok;
}

static void test_init_and_login(void)
{
	TCASE("init_token 与状态");
	hsm_token_t *tok = hsm_token_new(2);
	CHECK(tok != NULL);
	CHECK_EQ_INT(hsm_token_slot_count(tok), 2);

	slot_state_t st;
	CHECK_EQ_INT(hsm_slot_get_state(tok, 0, &st), HSM_OK);
	CHECK_EQ_INT(st, SLOT_ST_UNINIT);

	/* PIN 长度校验 */
	CHECK_EQ_INT(hsm_slot_init_token(tok, 0, "s0", "ab"), HSM_ERR_BAD_ARG);
	CHECK_EQ_INT(hsm_slot_init_token(tok, 0, NULL, SO_PIN), HSM_ERR_BAD_ARG);
	CHECK_EQ_INT(hsm_slot_init_token(tok, 9, "s0", SO_PIN), HSM_ERR_BAD_ARG);

	CHECK_EQ_INT(hsm_slot_init_token(tok, 0, "slot-zero", SO_PIN), HSM_OK);
	CHECK_EQ_INT(hsm_slot_get_state(tok, 0, &st), HSM_OK);
	CHECK_EQ_INT(st, SLOT_ST_EMPTY);
	/* 重复 init 必须被状态机拒绝 */
	CHECK_EQ_INT(hsm_slot_init_token(tok, 0, "again", SO_PIN), HSM_ERR_BAD_STATE);

	slot_meta_t m;
	CHECK_EQ_INT(hsm_slot_get_meta(tok, 0, &m), HSM_OK);
	CHECK(strcmp(m.label, "slot-zero") == 0);
	CHECK_EQ_INT(m.version, SLOT_META_VERSION);
	CHECK(m.created_at > 0);

	TCASE("登录：角色与错误 PIN");
	hsm_session_t sess;
	CHECK_EQ_INT(hsm_session_open(tok, 0, &sess), HSM_OK);
	hsm_role_t role;
	CHECK_EQ_INT(hsm_session_role(tok, sess, &role), HSM_OK);
	CHECK_EQ_INT(role, HSM_ROLE_PUBLIC);

	CHECK_EQ_INT(hsm_session_login(tok, sess, HSM_ROLE_SO, "wrong-pin"), HSM_ERR_PIN_INCORRECT);
	CHECK_EQ_INT(hsm_session_login(tok, sess, HSM_ROLE_SO, SO_PIN), HSM_OK);
	CHECK_EQ_INT(hsm_session_role(tok, sess, &role), HSM_OK);
	CHECK_EQ_INT(role, HSM_ROLE_SO);
	/* User PIN 尚未设置 */
	CHECK_EQ_INT(hsm_session_login(tok, sess, HSM_ROLE_USER, USER_PIN), HSM_ERR_NOT_AUTHORIZED);

	CHECK_EQ_INT(hsm_slot_set_user_pin(tok, sess, USER_PIN), HSM_OK);
	CHECK_EQ_INT(hsm_session_logout(tok, sess), HSM_OK);
	CHECK_EQ_INT(hsm_session_role(tok, sess, &role), HSM_OK);
	CHECK_EQ_INT(role, HSM_ROLE_PUBLIC);
	CHECK_EQ_INT(hsm_session_login(tok, sess, HSM_ROLE_USER, USER_PIN), HSM_OK);

	/* 未初始化的槽位不能登录 */
	hsm_session_t s1;
	CHECK_EQ_INT(hsm_session_open(tok, 1, &s1), HSM_OK);
	CHECK_EQ_INT(hsm_session_login(tok, s1, HSM_ROLE_SO, SO_PIN), HSM_ERR_BAD_STATE);

	TCASE("会话句柄失效");
	CHECK_EQ_INT(hsm_session_close(tok, sess), HSM_OK);
	CHECK_EQ_INT(hsm_session_role(tok, sess, &role), HSM_ERR_BAD_ARG);
	CHECK_EQ_INT(hsm_session_close(tok, sess), HSM_ERR_BAD_ARG);
	CHECK_EQ_INT(hsm_session_login(tok, 0, HSM_ROLE_SO, SO_PIN), HSM_ERR_BAD_ARG);

	hsm_token_free(tok);
}

static void test_acl(void)
{
	TCASE("ACL：默认拒绝，角色必须匹配");
	hsm_session_t sess;
	hsm_token_t *tok = fixture(&sess);
	CHECK(tok != NULL);
	hsm_handle_t h;

	/* 此刻是 SO 会话：生成密钥是 User 的活 */
	CHECK_EQ_INT(hsm_slot_generate(tok, sess, PQC_ALG_ML_DSA_65, KEY_USAGE_SIGN, 0, &h),
	             HSM_ERR_NOT_AUTHORIZED);

	CHECK_EQ_INT(hsm_session_logout(tok, sess), HSM_OK);
	/* 未登录同样拒绝 */
	CHECK_EQ_INT(hsm_slot_generate(tok, sess, PQC_ALG_ML_DSA_65, KEY_USAGE_SIGN, 0, &h),
	             HSM_ERR_NOT_AUTHORIZED);
	CHECK_EQ_INT(hsm_slot_zeroize(tok, sess, 0), HSM_ERR_NOT_AUTHORIZED);

	CHECK_EQ_INT(hsm_session_login(tok, sess, HSM_ROLE_USER, USER_PIN), HSM_OK);
	CHECK_EQ_INT(hsm_slot_generate(tok, sess, PQC_ALG_ML_DSA_65, KEY_USAGE_SIGN, 0, &h), HSM_OK);
	/* zeroize 是 SO 专属敏感操作（§7.3） */
	CHECK_EQ_INT(hsm_slot_zeroize(tok, sess, 0), HSM_ERR_NOT_AUTHORIZED);
	/* 设 User PIN 也是 SO 专属 */
	CHECK_EQ_INT(hsm_slot_set_user_pin(tok, sess, "another-pin"), HSM_ERR_NOT_AUTHORIZED);

	hsm_token_free(tok);
}

static void test_usage_exclusive(void)
{
	TCASE("用途互斥：禁止一钥多用（§7.2）");
	hsm_session_t sess;
	hsm_token_t *tok = fixture(&sess);
	CHECK(tok != NULL);
	CHECK_EQ_INT(hsm_session_logout(tok, sess), HSM_OK);
	CHECK_EQ_INT(hsm_session_login(tok, sess, HSM_ROLE_USER, USER_PIN), HSM_OK);
	hsm_handle_t h;

	/* 签名算法配 KEM 用途 → 拒绝 */
	CHECK_EQ_INT(hsm_slot_generate(tok, sess, PQC_ALG_ML_DSA_65, KEY_USAGE_DECAP, 0, &h),
	             HSM_ERR_USAGE_DENIED);
	/* KEM 算法配签名用途 → 拒绝 */
	CHECK_EQ_INT(hsm_slot_generate(tok, sess, PQC_ALG_ML_KEM_768, KEY_USAGE_SIGN, 0, &h),
	             HSM_ERR_USAGE_DENIED);
	/* 跨类组合 → 拒绝 */
	CHECK_EQ_INT(hsm_slot_generate(tok, sess, PQC_ALG_ML_DSA_65,
	                               KEY_USAGE_SIGN | KEY_USAGE_DECAP, 0, &h),
	             HSM_ERR_USAGE_DENIED);
	/* 空用途 → 拒绝 */
	CHECK_EQ_INT(hsm_slot_generate(tok, sess, PQC_ALG_ML_DSA_65, 0, 0, &h), HSM_ERR_BAD_ARG);
	/* 失败后槽位必须还停在 EMPTY，不能半装载 */
	slot_state_t st;
	CHECK_EQ_INT(hsm_slot_get_state(tok, 0, &st), HSM_OK);
	CHECK_EQ_INT(st, SLOT_ST_EMPTY);

	/* 正确组合 */
	CHECK_EQ_INT(hsm_slot_generate(tok, sess, PQC_ALG_ML_KEM_768, KEY_USAGE_DECAP, 0, &h), HSM_OK);

	/* 只有 DECAP 用途的密钥不能拿去签名 */
	uint8_t sig[8192];
	size_t sig_len = 0;
	CHECK_EQ_INT(hsm_object_sign(tok, sess, h, (const uint8_t *)"m", 1, NULL, 0,
	                             sig, sizeof(sig), &sig_len),
	             HSM_ERR_USAGE_DENIED);

	hsm_token_free(tok);
}

static void test_sign_and_handles(void)
{
	TCASE("签名往返 + 句柄语义");
	hsm_session_t sess;
	hsm_token_t *tok = fixture(&sess);
	CHECK(tok != NULL);
	CHECK_EQ_INT(hsm_session_logout(tok, sess), HSM_OK);
	CHECK_EQ_INT(hsm_session_login(tok, sess, HSM_ROLE_USER, USER_PIN), HSM_OK);

	hsm_handle_t h = HSM_INVALID_HANDLE;
	CHECK_EQ_INT(hsm_slot_generate(tok, sess, PQC_ALG_ML_DSA_65, KEY_USAGE_SIGN, 0, &h), HSM_OK);
	CHECK(h != HSM_INVALID_HANDLE);

	const pqc_alg_info_t *info = pqc_alg_info(PQC_ALG_ML_DSA_65);
	uint8_t *pk = malloc(info->pk_len);
	size_t pk_len = 0;
	CHECK_EQ_INT(hsm_object_public_key(tok, sess, h, pk, info->pk_len, &pk_len), HSM_OK);
	CHECK_EQ_INT(pk_len, info->pk_len);
	/* 缓冲不足必须拒绝而非截断 */
	CHECK_EQ_INT(hsm_object_public_key(tok, sess, h, pk, 10, &pk_len), HSM_ERR_BAD_ARG);

	uint8_t *sig = malloc(info->sig_len);
	size_t sig_len = 0;
	const uint8_t msg[] = "hello pqc-hsm";
	CHECK_EQ_INT(hsm_object_sign(tok, sess, h, msg, sizeof(msg), NULL, 0,
	                             sig, info->sig_len, &sig_len), HSM_OK);
	/* 签名必须能被导出的公钥验证 —— 证明句柄背后确实是配套的密钥对 */
	CHECK_EQ_INT(pqc_verify(PQC_ALG_ML_DSA_65, pk, msg, sizeof(msg), NULL, 0, sig, sig_len),
	             PQC_OK);

	/* 使用后状态回到 LOADED（USE_BEGIN→USE_END 成对） */
	slot_state_t st;
	CHECK_EQ_INT(hsm_slot_get_state(tok, 0, &st), HSM_OK);
	CHECK_EQ_INT(st, SLOT_ST_LOADED);
	slot_meta_t m;
	CHECK_EQ_INT(hsm_slot_get_meta(tok, 0, &m), HSM_OK);
	CHECK_EQ_INT(m.use_count, 1);
	CHECK(m.last_used_at > 0);

	TCASE("伪造/过期句柄");
	CHECK_EQ_INT(hsm_object_sign(tok, sess, HSM_INVALID_HANDLE, msg, sizeof(msg), NULL, 0,
	                             sig, info->sig_len, &sig_len), HSM_ERR_BAD_HANDLE);
	CHECK_EQ_INT(hsm_object_sign(tok, sess, h ^ 0x1000000000ULL, msg, sizeof(msg), NULL, 0,
	                             sig, info->sig_len, &sig_len), HSM_ERR_BAD_HANDLE);

	/* destroy 后旧句柄必须立刻失效（generation 递增） */
	CHECK_EQ_INT(hsm_object_destroy(tok, sess, h), HSM_OK);
	CHECK_EQ_INT(hsm_slot_get_state(tok, 0, &st), HSM_OK);
	CHECK_EQ_INT(st, SLOT_ST_EMPTY);
	CHECK_EQ_INT(hsm_object_sign(tok, sess, h, msg, sizeof(msg), NULL, 0,
	                             sig, info->sig_len, &sig_len), HSM_ERR_BAD_HANDLE);
	CHECK_EQ_INT(hsm_object_destroy(tok, sess, h), HSM_ERR_BAD_HANDLE);

	/* 重新生成得到的新句柄必须与旧句柄不同 */
	hsm_handle_t h2;
	CHECK_EQ_INT(hsm_slot_generate(tok, sess, PQC_ALG_ML_DSA_65, KEY_USAGE_SIGN, 0, &h2), HSM_OK);
	CHECK(h2 != h);

	free(pk);
	free(sig);
	hsm_token_free(tok);
}

static void test_kem_and_seed_storage(void)
{
	TCASE("KEM 解封装 + 种子存储（§7.6）");
	hsm_session_t sess;
	hsm_token_t *tok = fixture(&sess);
	CHECK(tok != NULL);
	CHECK_EQ_INT(hsm_session_logout(tok, sess), HSM_OK);
	CHECK_EQ_INT(hsm_session_login(tok, sess, HSM_ROLE_USER, USER_PIN), HSM_OK);

	const pqc_alg_info_t *info = pqc_alg_info(PQC_ALG_ML_KEM_768);
	hsm_handle_t h;
	/* 用 SEED_STORAGE 策略：槽位里只留 64 B 种子，私钥用时重展开 */
	CHECK_EQ_INT(hsm_slot_generate(tok, sess, PQC_ALG_ML_KEM_768, KEY_USAGE_DECAP,
	                               SLOT_POLICY_SEED_STORAGE, &h), HSM_OK);

	uint8_t *pk = malloc(info->pk_len);
	size_t pk_len = 0;
	CHECK_EQ_INT(hsm_object_public_key(tok, sess, h, pk, info->pk_len, &pk_len), HSM_OK);

	uint8_t *ct = malloc(info->ct_len);
	uint8_t ss_host[64], ss_hsm[64];
	size_t ss_len = 0;
	CHECK_EQ_INT(pqc_encaps(PQC_ALG_ML_KEM_768, pk, ct, ss_host), PQC_OK);
	CHECK_EQ_INT(hsm_object_decaps(tok, sess, h, ct, info->ct_len, ss_hsm, sizeof(ss_hsm), &ss_len),
	             HSM_OK);
	CHECK_EQ_INT(ss_len, info->ss_len);
	/* 从种子重展开的私钥必须解出与主机一致的共享秘密 */
	CHECK_EQ_MEM(ss_host, ss_hsm, info->ss_len);

	/* 再来一次，确认重展开路径可重复（不是靠某个残留副本） */
	CHECK_EQ_INT(hsm_object_decaps(tok, sess, h, ct, info->ct_len, ss_hsm, sizeof(ss_hsm), &ss_len),
	             HSM_OK);
	CHECK_EQ_MEM(ss_host, ss_hsm, info->ss_len);

	/* 密文长度不对必须拒绝 */
	CHECK_EQ_INT(hsm_object_decaps(tok, sess, h, ct, info->ct_len - 1, ss_hsm, sizeof(ss_hsm),
	                               &ss_len), HSM_ERR_BAD_ARG);

	TCASE("由外部种子装载：同种子必须得到同一把密钥");
	hsm_token_t *tok2 = NULL;
	hsm_session_t s2;
	tok2 = fixture(&s2);
	CHECK(tok2 != NULL);
	CHECK_EQ_INT(hsm_session_logout(tok2, s2), HSM_OK);
	CHECK_EQ_INT(hsm_session_login(tok2, s2, HSM_ROLE_USER, USER_PIN), HSM_OK);

	uint8_t seed[64];
	for (size_t i = 0; i < sizeof(seed); i++) {
		seed[i] = (uint8_t)(i ^ 0x3c);
	}
	hsm_handle_t ha, hb;
	uint8_t *pka = malloc(info->pk_len), *pkb = malloc(info->pk_len);
	size_t na = 0, nb = 0;
	CHECK_EQ_INT(hsm_slot_load_seed(tok2, s2, PQC_ALG_ML_KEM_768, KEY_USAGE_DECAP, 0,
	                                seed, sizeof(seed), &ha), HSM_OK);
	CHECK_EQ_INT(hsm_object_public_key(tok2, s2, ha, pka, info->pk_len, &na), HSM_OK);
	CHECK_EQ_INT(hsm_object_destroy(tok2, s2, ha), HSM_OK);
	CHECK_EQ_INT(hsm_slot_load_seed(tok2, s2, PQC_ALG_ML_KEM_768, KEY_USAGE_DECAP,
	                                SLOT_POLICY_SEED_STORAGE, seed, sizeof(seed), &hb), HSM_OK);
	CHECK_EQ_INT(hsm_object_public_key(tok2, s2, hb, pkb, info->pk_len, &nb), HSM_OK);
	CHECK_EQ_MEM(pka, pkb, info->pk_len);
	/* 种子长度不对必须拒绝 */
	CHECK_EQ_INT(hsm_slot_load_seed(tok2, s2, PQC_ALG_ML_KEM_768, KEY_USAGE_DECAP, 0,
	                                seed, 32, &ha), HSM_ERR_BAD_ARG);

	free(pk); free(ct); free(pka); free(pkb);
	hsm_token_free(tok);
	hsm_token_free(tok2);
}

static void test_pin_lockout(void)
{
	TCASE("PIN 错误计数 → 锁定 → SO 解锁（Phase 5 验收项）");
	hsm_session_t sess;
	hsm_token_t *tok = fixture(&sess);
	CHECK(tok != NULL);
	CHECK_EQ_INT(hsm_session_logout(tok, sess), HSM_OK);
	CHECK_EQ_INT(hsm_session_login(tok, sess, HSM_ROLE_USER, USER_PIN), HSM_OK);
	hsm_handle_t h;
	CHECK_EQ_INT(hsm_slot_generate(tok, sess, PQC_ALG_ML_DSA_65, KEY_USAGE_SIGN, 0, &h), HSM_OK);
	CHECK_EQ_INT(hsm_session_logout(tok, sess), HSM_OK);

	slot_meta_t m;
	/* 前 MAX-1 次：计数递增但不锁 */
	for (uint32_t i = 1; i < HSM_PIN_MAX_FAILS; i++) {
		CHECK_EQ_INT(hsm_session_login(tok, sess, HSM_ROLE_USER, "bad-pin"),
		             HSM_ERR_PIN_INCORRECT);
		CHECK_EQ_INT(hsm_slot_get_meta(tok, 0, &m), HSM_OK);
		CHECK_EQ_INT(m.user_pin_fails, i);
		CHECK_EQ_INT(m.state, SLOT_ST_LOADED);
	}
	/* 第 MAX 次：锁定 */
	CHECK_EQ_INT(hsm_session_login(tok, sess, HSM_ROLE_USER, "bad-pin"), HSM_ERR_PIN_LOCKED);
	CHECK_EQ_INT(hsm_slot_get_meta(tok, 0, &m), HSM_OK);
	CHECK_EQ_INT(m.state, SLOT_ST_LOCKED);
	CHECK_EQ_INT(m.user_pin_fails, HSM_PIN_MAX_FAILS);

	/* 锁定后即使 PIN 正确也不许登录 */
	CHECK_EQ_INT(hsm_session_login(tok, sess, HSM_ROLE_USER, USER_PIN), HSM_ERR_PIN_LOCKED);
	/* 锁定后密钥操作全部拒绝：句柄已因状态不再是 LOADED 而失效 */
	uint8_t sig[8192];
	size_t sig_len = 0;
	CHECK_EQ_INT(hsm_object_sign(tok, sess, h, (const uint8_t *)"m", 1, NULL, 0,
	                             sig, sizeof(sig), &sig_len), HSM_ERR_NOT_AUTHORIZED);

	TCASE("只有 SO 能解锁");
	CHECK_EQ_INT(hsm_session_login(tok, sess, HSM_ROLE_SO, SO_PIN), HSM_OK);
	CHECK_EQ_INT(hsm_slot_unlock(tok, sess, 0), HSM_OK);
	CHECK_EQ_INT(hsm_slot_get_meta(tok, 0, &m), HSM_OK);
	/* 锁定前是 LOADED，解锁后回 LOADED，且失败计数清零 */
	CHECK_EQ_INT(m.state, SLOT_ST_LOADED);
	CHECK_EQ_INT(m.user_pin_fails, 0);
	/* 非 LOCKED 状态再解锁 → 非法转移 */
	CHECK_EQ_INT(hsm_slot_unlock(tok, sess, 0), HSM_ERR_BAD_STATE);

	/* 解锁后 User 可以正常登录并继续用原句柄 */
	CHECK_EQ_INT(hsm_session_logout(tok, sess), HSM_OK);
	CHECK_EQ_INT(hsm_session_login(tok, sess, HSM_ROLE_USER, USER_PIN), HSM_OK);
	CHECK_EQ_INT(hsm_object_sign(tok, sess, h, (const uint8_t *)"m", 1, NULL, 0,
	                             sig, sizeof(sig), &sig_len), HSM_OK);

	TCASE("成功登录会清零失败计数");
	CHECK_EQ_INT(hsm_session_logout(tok, sess), HSM_OK);
	CHECK_EQ_INT(hsm_session_login(tok, sess, HSM_ROLE_USER, "bad"), HSM_ERR_PIN_INCORRECT);
	CHECK_EQ_INT(hsm_slot_get_meta(tok, 0, &m), HSM_OK);
	CHECK_EQ_INT(m.user_pin_fails, 1);
	CHECK_EQ_INT(hsm_session_login(tok, sess, HSM_ROLE_USER, USER_PIN), HSM_OK);
	CHECK_EQ_INT(hsm_slot_get_meta(tok, 0, &m), HSM_OK);
	CHECK_EQ_INT(m.user_pin_fails, 0);

	TCASE("SO 连续错误只计数不锁槽位（否则设备变砖）");
	CHECK_EQ_INT(hsm_session_logout(tok, sess), HSM_OK);
	for (uint32_t i = 1; i <= HSM_PIN_MAX_FAILS + 2; i++) {
		CHECK_EQ_INT(hsm_session_login(tok, sess, HSM_ROLE_SO, "bad-so"),
		             HSM_ERR_PIN_INCORRECT);
	}
	CHECK_EQ_INT(hsm_slot_get_meta(tok, 0, &m), HSM_OK);
	CHECK_EQ_INT(m.state, SLOT_ST_LOADED);
	CHECK_EQ_INT(m.so_pin_fails, HSM_PIN_MAX_FAILS + 2);
	CHECK_EQ_INT(hsm_session_login(tok, sess, HSM_ROLE_SO, SO_PIN), HSM_OK);

	hsm_token_free(tok);
}

static void test_zeroize(void)
{
	TCASE("zeroize：任意状态可达、不可逆、清得干净");
	hsm_session_t sess;
	hsm_token_t *tok = fixture(&sess);
	CHECK(tok != NULL);
	CHECK_EQ_INT(hsm_session_logout(tok, sess), HSM_OK);
	CHECK_EQ_INT(hsm_session_login(tok, sess, HSM_ROLE_USER, USER_PIN), HSM_OK);
	hsm_handle_t h;
	CHECK_EQ_INT(hsm_slot_generate(tok, sess, PQC_ALG_ML_DSA_65, KEY_USAGE_SIGN, 0, &h), HSM_OK);
	CHECK_EQ_INT(hsm_session_logout(tok, sess), HSM_OK);
	CHECK_EQ_INT(hsm_session_login(tok, sess, HSM_ROLE_SO, SO_PIN), HSM_OK);
	CHECK_EQ_INT(hsm_slot_zeroize(tok, sess, 0), HSM_OK);

	slot_meta_t m;
	CHECK_EQ_INT(hsm_slot_get_meta(tok, 0, &m), HSM_OK);
	CHECK_EQ_INT(m.state, SLOT_ST_UNINIT);
	CHECK_EQ_INT(m.alg, PQC_ALG_NONE);
	CHECK_EQ_INT(m.usage, 0);
	CHECK_EQ_INT(m.use_count, 0);
	CHECK_EQ_INT(m.label[0], 0);
	/* 句柄失效、登录态失效 */
	CHECK_EQ_INT(hsm_object_destroy(tok, sess, h), HSM_ERR_NOT_AUTHORIZED);
	hsm_role_t role;
	CHECK_EQ_INT(hsm_session_role(tok, sess, &role), HSM_OK);
	CHECK_EQ_INT(role, HSM_ROLE_PUBLIC);
	/* PIN 也被清掉了 —— 清零后无法用旧 PIN 登录 */
	CHECK_EQ_INT(hsm_session_login(tok, sess, HSM_ROLE_SO, SO_PIN), HSM_ERR_BAD_STATE);
	/* 只能重新 init_token */
	CHECK_EQ_INT(hsm_slot_init_token(tok, 0, "reborn", SO_PIN), HSM_OK);

	TCASE("强制清零（对应硬件 tamper 线）可从任意状态触发");
	for (int s = SLOT_ST_UNINIT; s < SLOT_ST__COUNT; s++) {
		CHECK_EQ_INT(hsm_slot_force_state(tok, 1, (slot_state_t)s), HSM_OK);
		CHECK_EQ_INT(hsm_slot_zeroize_forced(tok, 1), HSM_OK);
		slot_state_t got;
		CHECK_EQ_INT(hsm_slot_get_state(tok, 1, &got), HSM_OK);
		CHECK_EQ_INT(got, SLOT_ST_UNINIT);
	}

	hsm_token_free(tok);
}

static void test_state_gating(void)
{
	TCASE("全状态 × 关键 API：非法组合必须被拒");
	hsm_session_t sess;
	hsm_token_t *tok = fixture(&sess);
	CHECK(tok != NULL);
	CHECK_EQ_INT(hsm_session_logout(tok, sess), HSM_OK);
	CHECK_EQ_INT(hsm_session_login(tok, sess, HSM_ROLE_USER, USER_PIN), HSM_OK);

	hsm_handle_t h;
	/* generate 只在 EMPTY 合法 */
	for (int s = SLOT_ST_UNINIT; s < SLOT_ST__COUNT; s++) {
		CHECK_EQ_INT(hsm_slot_force_state(tok, 0, (slot_state_t)s), HSM_OK);
		hsm_status_t got = hsm_slot_generate(tok, sess, PQC_ALG_ML_DSA_65,
		                                     KEY_USAGE_SIGN, 0, &h);
		if (s == SLOT_ST_EMPTY) {
			CHECK_EQ_INT(got, HSM_OK);
		} else {
			CHECK_EQ_INT(got, HSM_ERR_BAD_STATE);
		}
	}
	hsm_token_free(tok);
}

int main(void)
{
	test_init_and_login();
	test_acl();
	test_usage_exclusive();
	test_sign_and_handles();
	test_kem_and_seed_storage();
	test_pin_lockout();
	test_zeroize();
	test_state_gating();
	return test_report("test_slot");
}
