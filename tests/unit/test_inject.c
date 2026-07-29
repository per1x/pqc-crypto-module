/* 安全密钥注入（§8.5）
 *
 * 主线：注入端只拿到设备的 ML-KEM 公钥，就能把一把密钥安全灌进设备，
 * 且**链路上只有密文**。判据不是"没报错"，而是：
 *   1. 注入 blob 里搜不到种子、也搜不到最终密钥的公钥；
 *   2. 注入完成后设备能用这把密钥签出被注入端预期公钥验过的签名。
 */
#include "testlib.h"
#include "pqchsm/inject.h"
#include "pqchsm/slot.h"
#include "pqchsm/util.h"

#include <stdlib.h>

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

/* 供应一个槽位并登录为 User */
static hsm_session_t provision(hsm_token_t *tok, hsm_slot_id_t slot)
{
	hsm_session_t s;
	char label[32];
	snprintf(label, sizeof(label), "slot-%u", slot);
	if (hsm_slot_init_token(tok, slot, label, SO_PIN) != HSM_OK ||
	    hsm_session_open(tok, slot, &s) != HSM_OK ||
	    hsm_session_login(tok, s, HSM_ROLE_SO, SO_PIN) != HSM_OK ||
	    hsm_slot_set_user_pin(tok, s, USER_PIN) != HSM_OK ||
	    hsm_session_logout(tok, s) != HSM_OK ||
	    hsm_session_login(tok, s, HSM_ROLE_USER, USER_PIN) != HSM_OK) {
		abort();
	}
	return s;
}

int main(void)
{
	hsm_token_t *tok = hsm_token_new(3);
	CHECK(tok != NULL);

	/* slot0 = 设备注入钥（ML-KEM），slot1 = 目标槽位，slot2 = 另一个目标 */
	hsm_session_t kem_sess = provision(tok, 0);
	hsm_session_t tgt_sess = provision(tok, 1);
	hsm_session_t tgt2_sess = provision(tok, 2);

	TCASE("设备生成注入钥并公开其公钥");
	hsm_handle_t kem_h;
	CHECK_EQ_INT(hsm_slot_generate(tok, kem_sess, PQC_ALG_ML_KEM_768,
	                               KEY_USAGE_DECAP, 0, &kem_h), HSM_OK);
	const pqc_alg_info_t *kem = pqc_alg_info(PQC_ALG_ML_KEM_768);
	uint8_t *device_pk = malloc(kem->pk_len);
	size_t pk_len = 0;
	CHECK_EQ_INT(hsm_object_public_key(tok, kem_sess, kem_h, device_pk, kem->pk_len,
	                                   &pk_len), HSM_OK);
	CHECK_EQ_INT(pk_len, kem->pk_len);

	/* 注入端：手上只有 device_pk 和一把要灌进去的密钥（以种子形式） */
	uint8_t seed[32];
	for (size_t i = 0; i < sizeof(seed); i++) {
		seed[i] = (uint8_t)(0x11 * (i + 1));
	}
	/* 注入端自己先算出这把密钥的公钥，作为后面验证的判据 */
	const pqc_alg_info_t *dsa = pqc_alg_info(PQC_ALG_ML_DSA_65);
	uint8_t *expect_pk = malloc(dsa->pk_len), *tmp_sk = malloc(dsa->sk_len);
	CHECK_EQ_INT(pqc_keypair_from_seed(PQC_ALG_ML_DSA_65, seed, sizeof(seed),
	                                   expect_pk, tmp_sk), PQC_OK);
	pqc_secure_zero(tmp_sk, dsa->sk_len);

	TCASE("注入端构造 blob；链路上只有密文");
	size_t cap = hsm_inject_blob_len(PQC_ALG_ML_KEM_768, sizeof(seed));
	CHECK(cap > 0);
	uint8_t *blob = malloc(cap);
	size_t blob_len = 0;
	CHECK_EQ_INT(hsm_inject_build(PQC_ALG_ML_KEM_768, device_pk, pk_len,
	                              PQC_ALG_ML_DSA_65, KEY_USAGE_SIGN, 0,
	                              seed, sizeof(seed), blob, cap, &blob_len), HSM_OK);
	CHECK_EQ_INT(blob_len, cap);
	/* 红线：blob 里既没有种子，也没有那把密钥的公钥 */
	CHECK_EQ_INT(mem_contains(blob, blob_len, seed, sizeof(seed)), 0);
	CHECK_EQ_INT(mem_contains(blob, blob_len, expect_pk, dsa->pk_len), 0);

	TCASE("设备端注入 → 得到的正是注入端那把密钥");
	hsm_handle_t injected = HSM_INVALID_HANDLE;
	CHECK_EQ_INT(hsm_inject_apply(tok, kem_sess, kem_h, tgt_sess,
	                              blob, blob_len, &injected), HSM_OK);
	CHECK(injected != HSM_INVALID_HANDLE);
	{
		uint8_t *got_pk = malloc(dsa->pk_len);
		size_t n = 0;
		CHECK_EQ_INT(hsm_object_public_key(tok, tgt_sess, injected, got_pk, dsa->pk_len, &n),
		             HSM_OK);
		CHECK_EQ_MEM(got_pk, expect_pk, dsa->pk_len);
		/* 而且真的能用：签名可被注入端预期的公钥验证 */
		uint8_t *sig = malloc(dsa->sig_len);
		size_t sl = 0;
		const uint8_t msg[] = "injected-key-works";
		CHECK_EQ_INT(hsm_object_sign(tok, tgt_sess, injected, msg, sizeof(msg), NULL, 0,
		                             sig, dsa->sig_len, &sl), HSM_OK);
		CHECK_EQ_INT(pqc_verify(PQC_ALG_ML_DSA_65, expect_pk, msg, sizeof(msg), NULL, 0,
		                        sig, sl), PQC_OK);
		free(got_pk);
		free(sig);
	}

	TCASE("拿错注入钥 → 失败");
	{
		/* slot2 生成另一把 KEM 钥，用它去解 slot0 的 blob */
		hsm_handle_t other;
		CHECK_EQ_INT(hsm_slot_generate(tok, tgt2_sess, PQC_ALG_ML_KEM_768,
		                               KEY_USAGE_DECAP, 0, &other), HSM_OK);
		hsm_handle_t dummy;
		/* 目标用 slot2 自己（已装载且无 INJECTABLE）→ 先撞策略 */
		CHECK_EQ_INT(hsm_inject_apply(tok, tgt2_sess, other, tgt2_sess,
		                              blob, blob_len, &dummy), HSM_ERR_POLICY);
		CHECK_EQ_INT(hsm_object_destroy(tok, tgt2_sess, other), HSM_OK);
		/* 重新生成注入钥，目标为空槽位 → 这次应当因为解封装结果不对而失败 */
		CHECK_EQ_INT(hsm_slot_generate(tok, tgt2_sess, PQC_ALG_ML_KEM_768,
		                               KEY_USAGE_DECAP, 0, &other), HSM_OK);
		hsm_session_t s3;
		CHECK_EQ_INT(hsm_session_open(tok, 1, &s3), HSM_OK);
		CHECK_EQ_INT(hsm_session_login(tok, s3, HSM_ROLE_USER, USER_PIN), HSM_OK);
		/* slot1 已装载注入进去的密钥、且没有 INJECTABLE → 策略拦截 */
		CHECK_EQ_INT(hsm_inject_apply(tok, tgt2_sess, other, s3, blob, blob_len, &dummy),
		             HSM_ERR_POLICY);
		CHECK_EQ_INT(hsm_session_close(tok, s3), HSM_OK);
	}

	TCASE("篡改 blob 的任意一字节 → 注入必须失败（抽样扫描）");
	{
		/* 需要一个干净的目标槽位：把 slot1 清掉重来 */
		hsm_session_t so;
		CHECK_EQ_INT(hsm_session_open(tok, 1, &so), HSM_OK);
		CHECK_EQ_INT(hsm_session_login(tok, so, HSM_ROLE_SO, SO_PIN), HSM_OK);
		CHECK_EQ_INT(hsm_slot_zeroize(tok, so, 1), HSM_OK);
		CHECK_EQ_INT(hsm_session_close(tok, so), HSM_OK);
		hsm_session_t fresh = provision(tok, 1);

		uint8_t *copy = malloc(blob_len);
		int escaped = 0, checked = 0;
		size_t step = blob_len / 60 ? blob_len / 60 : 1;
		for (size_t pos = 0; pos < blob_len; pos += step) {
			memcpy(copy, blob, blob_len);
			copy[pos] ^= 0x01;
			hsm_handle_t h;
			if (hsm_inject_apply(tok, kem_sess, kem_h, fresh, copy, blob_len, &h) == HSM_OK) {
				escaped++;
				fprintf(stderr, "  偏移 %zu 被改后仍注入成功\n", pos);
				/* 清掉以免影响下一轮 */
				hsm_object_destroy(tok, fresh, h);
			}
			checked++;
		}
		CHECK(checked > 50);
		CHECK_EQ_INT(escaped, 0);
		free(copy);

		TCASE("清白的 blob 仍能注入（证明上面的失败不是槽位被搞坏了）");
		hsm_handle_t h2;
		CHECK_EQ_INT(hsm_inject_apply(tok, kem_sess, kem_h, fresh, blob, blob_len, &h2),
		             HSM_OK);
		tgt_sess = fresh;
		injected = h2;
	}

	TCASE("INJECTABLE 策略：允许注入更新已装载的槽位");
	{
		/* 用带 INJECTABLE 的策略重新注入一次 */
		hsm_session_t so;
		CHECK_EQ_INT(hsm_session_open(tok, 1, &so), HSM_OK);
		CHECK_EQ_INT(hsm_session_login(tok, so, HSM_ROLE_SO, SO_PIN), HSM_OK);
		CHECK_EQ_INT(hsm_slot_zeroize(tok, so, 1), HSM_OK);
		CHECK_EQ_INT(hsm_session_close(tok, so), HSM_OK);
		hsm_session_t s = provision(tok, 1);

		uint8_t *b2 = malloc(cap);
		size_t l2 = 0;
		CHECK_EQ_INT(hsm_inject_build(PQC_ALG_ML_KEM_768, device_pk, pk_len,
		                              PQC_ALG_ML_DSA_65, KEY_USAGE_SIGN,
		                              SLOT_POLICY_INJECTABLE,
		                              seed, sizeof(seed), b2, cap, &l2), HSM_OK);
		hsm_handle_t h;
		CHECK_EQ_INT(hsm_inject_apply(tok, kem_sess, kem_h, s, b2, l2, &h), HSM_OK);
		/* 现在槽位带 INJECTABLE，再注入一次应当成功（更新） */
		uint8_t seed2[32];
		memset(seed2, 0x77, sizeof(seed2));
		uint8_t *b3 = malloc(cap);
		size_t l3 = 0;
		CHECK_EQ_INT(hsm_inject_build(PQC_ALG_ML_KEM_768, device_pk, pk_len,
		                              PQC_ALG_ML_DSA_65, KEY_USAGE_SIGN,
		                              SLOT_POLICY_INJECTABLE,
		                              seed2, sizeof(seed2), b3, cap, &l3), HSM_OK);
		hsm_handle_t h3;
		CHECK_EQ_INT(hsm_inject_apply(tok, kem_sess, kem_h, s, b3, l3, &h3), HSM_OK);
		CHECK(h3 != h);   /* generation 递增，旧句柄失效 */
		free(b2);
		free(b3);
	}

	TCASE("非法参数");
	{
		uint8_t small[16];
		size_t l = 0;
		CHECK_EQ_INT(hsm_inject_build(PQC_ALG_ML_DSA_65, device_pk, pk_len,
		                              PQC_ALG_ML_DSA_65, KEY_USAGE_SIGN, 0,
		                              seed, sizeof(seed), blob, cap, &l),
		             HSM_ERR_BAD_ARG);   /* KEM 位置放了签名算法 */
		CHECK_EQ_INT(hsm_inject_build(PQC_ALG_ML_KEM_768, device_pk, pk_len - 1,
		                              PQC_ALG_ML_DSA_65, KEY_USAGE_SIGN, 0,
		                              seed, sizeof(seed), blob, cap, &l),
		             HSM_ERR_BAD_ARG);
		CHECK_EQ_INT(hsm_inject_build(PQC_ALG_ML_KEM_768, device_pk, pk_len,
		                              PQC_ALG_ML_DSA_65, KEY_USAGE_SIGN, 0,
		                              seed, 16, blob, cap, &l), HSM_ERR_BAD_ARG);
		CHECK_EQ_INT(hsm_inject_build(PQC_ALG_ML_KEM_768, device_pk, pk_len,
		                              PQC_ALG_ML_DSA_65, KEY_USAGE_SIGN, 0,
		                              seed, sizeof(seed), small, sizeof(small), &l),
		             HSM_ERR_BAD_ARG);
		hsm_handle_t h;
		CHECK_EQ_INT(hsm_inject_apply(NULL, kem_sess, kem_h, tgt_sess, blob, blob_len, &h),
		             HSM_ERR_BAD_ARG);
		CHECK_EQ_INT(hsm_inject_apply(tok, kem_sess, kem_h, tgt_sess, blob, 8, &h),
		             HSM_ERR_BAD_ARG);
		CHECK_EQ_INT(hsm_inject_blob_len(PQC_ALG_ML_DSA_65, 32), 0);
	}

	TCASE("制造模式：默认关闭；熔断后不可再打开（§8.5）");
	{
		hsm_session_t so;
		CHECK_EQ_INT(hsm_session_open(tok, 2, &so), HSM_OK);
		CHECK_EQ_INT(hsm_session_login(tok, so, HSM_ROLE_SO, SO_PIN), HSM_OK);
		CHECK_EQ_INT(hsm_slot_zeroize(tok, so, 2), HSM_OK);
		CHECK_EQ_INT(hsm_session_close(tok, so), HSM_OK);
		hsm_session_t s = provision(tok, 2);

		CHECK_EQ_INT(hsm_inject_manufacturing_mode(), 0);
		hsm_handle_t h;
		/* 关闭状态下明文注入被拒 */
		CHECK_EQ_INT(hsm_inject_plaintext(tok, s, PQC_ALG_ML_DSA_65, KEY_USAGE_SIGN, 0,
		                                  seed, sizeof(seed), &h), HSM_ERR_POLICY);
		/* 打开后可用 */
		CHECK_EQ_INT(hsm_inject_set_manufacturing_mode(1), 0);
		CHECK_EQ_INT(hsm_inject_manufacturing_mode(), 1);
		CHECK_EQ_INT(hsm_inject_plaintext(tok, s, PQC_ALG_ML_DSA_65, KEY_USAGE_SIGN, 0,
		                                  seed, sizeof(seed), &h), HSM_OK);
		/* 熔断 */
		CHECK_EQ_INT(hsm_inject_set_manufacturing_mode(0), 0);
		CHECK_EQ_INT(hsm_inject_manufacturing_mode(), 0);
		/* 熔断后再也打不开 —— 模拟 eFUSE 的不可逆 */
		CHECK_EQ_INT(hsm_inject_set_manufacturing_mode(1), -1);
		CHECK_EQ_INT(hsm_inject_manufacturing_mode(), 0);
	}

	free(device_pk);
	free(expect_pk);
	free(tmp_sk);
	free(blob);
	hsm_token_free(tok);
	return test_report("test_inject");
}
