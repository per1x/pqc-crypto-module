/* 后端行为测试：往返、种子确定性、rnd 语义、负测试。
 * KAT 证明"算得对"，这里证明"接口约定成立" —— 后者才是硬件后端替换时的契约。
 */
#include "testlib.h"
#include "pqchsm/pqc.h"
#include "pqchsm/util.h"

#include <stdlib.h>

static uint8_t *xmalloc(size_t n)
{
	uint8_t *p = malloc(n ? n : 1);
	if (!p) {
		abort();
	}
	return p;
}

static void test_kem(pqc_alg_t alg)
{
	const pqc_alg_info_t *info = pqc_alg_info(alg);
	TCASE(info->name);

	uint8_t *pk = xmalloc(info->pk_len), *sk = xmalloc(info->sk_len);
	uint8_t *ct = xmalloc(info->ct_len);
	uint8_t *ss_a = xmalloc(info->ss_len), *ss_b = xmalloc(info->ss_len);

	CHECK_EQ_INT(pqc_keypair(alg, pk, sk), PQC_OK);
	CHECK_EQ_INT(pqc_encaps(alg, pk, ct, ss_a), PQC_OK);
	CHECK_EQ_INT(pqc_decaps(alg, sk, ct, ss_b), PQC_OK);
	CHECK_EQ_MEM(ss_a, ss_b, info->ss_len);

	/* 种子确定性 */
	uint8_t seed[64];
	for (size_t i = 0; i < sizeof(seed); i++) {
		seed[i] = (uint8_t)(i * 7 + 1);
	}
	uint8_t *pk2 = xmalloc(info->pk_len), *sk2 = xmalloc(info->sk_len);
	CHECK_EQ_INT(pqc_keypair_from_seed(alg, seed, 64, pk, sk), PQC_OK);
	CHECK_EQ_INT(pqc_keypair_from_seed(alg, seed, 64, pk2, sk2), PQC_OK);
	CHECK_EQ_MEM(pk, pk2, info->pk_len);
	CHECK_EQ_MEM(sk, sk2, info->sk_len);
	/* 重展开的密钥必须真能用 */
	CHECK_EQ_INT(pqc_encaps(alg, pk2, ct, ss_a), PQC_OK);
	CHECK_EQ_INT(pqc_decaps(alg, sk2, ct, ss_b), PQC_OK);
	CHECK_EQ_MEM(ss_a, ss_b, info->ss_len);

	/* encaps_derand 对同一 m 必须给出同一密文 */
	uint8_t m[32];
	for (size_t i = 0; i < sizeof(m); i++) {
		m[i] = (uint8_t)(0xA0 ^ i);
	}
	uint8_t *ct2 = xmalloc(info->ct_len);
	CHECK_EQ_INT(pqc_encaps_derand(alg, pk, m, 32, ct, ss_a), PQC_OK);
	CHECK_EQ_INT(pqc_encaps_derand(alg, pk, m, 32, ct2, ss_b), PQC_OK);
	CHECK_EQ_MEM(ct, ct2, info->ct_len);
	CHECK_EQ_MEM(ss_a, ss_b, info->ss_len);

	/* 负测试：种子长度不对必须拒绝 */
	CHECK_EQ_INT(pqc_keypair_from_seed(alg, seed, 32, pk, sk), PQC_ERR_BAD_ARG);
	CHECK_EQ_INT(pqc_encaps_derand(alg, pk, m, 16, ct, ss_a), PQC_ERR_BAD_ARG);

	/* 篡改密文 → 隐式拒绝：仍返回成功，但共享秘密不同（FIPS 203 的设计） */
	CHECK_EQ_INT(pqc_encaps(alg, pk, ct, ss_a), PQC_OK);
	ct[0] ^= 0xff;
	CHECK_EQ_INT(pqc_decaps(alg, sk, ct, ss_b), PQC_OK);
	CHECK(memcmp(ss_a, ss_b, info->ss_len) != 0);

	pqc_secure_zero(sk, info->sk_len);
	pqc_secure_zero(sk2, info->sk_len);
	free(pk); free(sk); free(pk2); free(sk2);
	free(ct); free(ct2); free(ss_a); free(ss_b);
}

static void test_sig(pqc_alg_t alg)
{
	const pqc_alg_info_t *info = pqc_alg_info(alg);
	TCASE(info->name);

	uint8_t *pk = xmalloc(info->pk_len), *sk = xmalloc(info->sk_len);
	uint8_t *sig = xmalloc(info->sig_len), *sig2 = xmalloc(info->sig_len);
	size_t slen = info->sig_len, slen2 = info->sig_len;
	const uint8_t msg[] = "pqc-hsm roundtrip";
	const uint8_t ctx[] = { 0x01, 0x02, 0x03 };

	CHECK_EQ_INT(pqc_keypair(alg, pk, sk), PQC_OK);

	/* hedged 签名（rnd = NULL）与验签 */
	CHECK_EQ_INT(pqc_sign(alg, sk, msg, sizeof(msg), NULL, 0, NULL, sig, &slen), PQC_OK);
	CHECK(slen <= info->sig_len);
	CHECK_EQ_INT(pqc_verify(alg, pk, msg, sizeof(msg), NULL, 0, sig, slen), PQC_OK);

	/* 种子确定性 */
	uint8_t seed[32];
	for (size_t i = 0; i < sizeof(seed); i++) {
		seed[i] = (uint8_t)(i * 3 + 5);
	}
	uint8_t *pk2 = xmalloc(info->pk_len), *sk2 = xmalloc(info->sk_len);
	CHECK_EQ_INT(pqc_keypair_from_seed(alg, seed, 32, pk, sk), PQC_OK);
	CHECK_EQ_INT(pqc_keypair_from_seed(alg, seed, 32, pk2, sk2), PQC_OK);
	CHECK_EQ_MEM(pk, pk2, info->pk_len);
	CHECK_EQ_MEM(sk, sk2, info->sk_len);

	/* deterministic 模式（rnd = 0^32）必须可复现 —— ACVP deterministic 组的前提 */
	uint8_t rnd0[PQC_SIG_RND_LEN] = { 0 };
	slen = slen2 = info->sig_len;
	CHECK_EQ_INT(pqc_sign(alg, sk, msg, sizeof(msg), ctx, sizeof(ctx), rnd0, sig, &slen), PQC_OK);
	CHECK_EQ_INT(pqc_sign(alg, sk, msg, sizeof(msg), ctx, sizeof(ctx), rnd0, sig2, &slen2), PQC_OK);
	CHECK_EQ_INT(slen, slen2);
	CHECK_EQ_MEM(sig, sig2, slen);
	CHECK_EQ_INT(pqc_verify(alg, pk, msg, sizeof(msg), ctx, sizeof(ctx), sig, slen), PQC_OK);

	/* rnd 必须真的进入签名：换一个 rnd 结果必须不同 */
	uint8_t rnd1[PQC_SIG_RND_LEN];
	memset(rnd1, 0x5a, sizeof(rnd1));
	slen2 = info->sig_len;
	CHECK_EQ_INT(pqc_sign(alg, sk, msg, sizeof(msg), ctx, sizeof(ctx), rnd1, sig2, &slen2), PQC_OK);
	CHECK(slen != slen2 || memcmp(sig, sig2, slen) != 0);
	CHECK_EQ_INT(pqc_verify(alg, pk, msg, sizeof(msg), ctx, sizeof(ctx), sig2, slen2), PQC_OK);

	/* 负测试：context 必须参与验签（FIPS 204 external 接口的域分隔） */
	CHECK_EQ_INT(pqc_verify(alg, pk, msg, sizeof(msg), NULL, 0, sig, slen), PQC_ERR_VERIFY);
	/* 负测试：篡改签名 */
	sig[0] ^= 0xff;
	CHECK_EQ_INT(pqc_verify(alg, pk, msg, sizeof(msg), ctx, sizeof(ctx), sig, slen), PQC_ERR_VERIFY);
	sig[0] ^= 0xff;
	/* 负测试：篡改消息 */
	uint8_t bad[sizeof(msg)];
	memcpy(bad, msg, sizeof(msg));
	bad[0] ^= 0x01;
	CHECK_EQ_INT(pqc_verify(alg, pk, bad, sizeof(bad), ctx, sizeof(ctx), sig, slen), PQC_ERR_VERIFY);
	/* 负测试：context 超过 255 字节（FIPS 204 上限）必须被前置校验拒绝 */
	uint8_t longctx[256] = { 0 };
	CHECK_EQ_INT(pqc_sign(alg, sk, msg, sizeof(msg), longctx, sizeof(longctx), NULL, sig, &slen),
	             PQC_ERR_BAD_ARG);

	pqc_secure_zero(sk, info->sk_len);
	pqc_secure_zero(sk2, info->sk_len);
	free(pk); free(sk); free(pk2); free(sk2); free(sig); free(sig2);
}

int main(void)
{
	test_use_stub_kdr();   /* 显式装桩：库里没有自动回退了 */
	TCASE("参数校验");
	uint8_t dummy[8] = { 0 };
	CHECK_EQ_INT(pqc_keypair(PQC_ALG_NONE, dummy, dummy), PQC_ERR_BAD_ARG);
	CHECK_EQ_INT(pqc_keypair(PQC_ALG_ML_KEM_768, NULL, dummy), PQC_ERR_BAD_ARG);
	/* 种类不符：对 KEM 调签名、对签名调 KEM */
	CHECK_EQ_INT(pqc_encaps(PQC_ALG_ML_DSA_65, dummy, dummy, dummy), PQC_ERR_BAD_ARG);
	CHECK_EQ_INT(pqc_verify(PQC_ALG_ML_KEM_768, dummy, dummy, 1, NULL, 0, dummy, 1),
	             PQC_ERR_BAD_ARG);

	test_kem(PQC_ALG_ML_KEM_512);
	test_kem(PQC_ALG_ML_KEM_768);
	test_kem(PQC_ALG_ML_KEM_1024);
	test_sig(PQC_ALG_ML_DSA_44);
	test_sig(PQC_ALG_ML_DSA_65);
	test_sig(PQC_ALG_ML_DSA_87);

	return test_report("test_pqc_roundtrip");
}
