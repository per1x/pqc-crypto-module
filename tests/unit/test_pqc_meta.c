/* 元数据表与 liboqs 运行时值对拍。
 * src/crypto/pqc.c 里那张长度表是上层分配缓冲的唯一依据，一旦与后端脱节
 * 就是缓冲区溢出，所以必须逐项断言，而不是"抄一遍 FIPS 文档就完事"。
 */
#include "testlib.h"
#include "pqchsm/pqc.h"

#include <oqs/oqs.h>

static const pqc_alg_t ALGS[] = {
	PQC_ALG_ML_KEM_512, PQC_ALG_ML_KEM_768, PQC_ALG_ML_KEM_1024,
	PQC_ALG_ML_DSA_44,  PQC_ALG_ML_DSA_65,  PQC_ALG_ML_DSA_87,
};

static const char *OQS_NAME[] = {
	OQS_KEM_alg_ml_kem_512, OQS_KEM_alg_ml_kem_768, OQS_KEM_alg_ml_kem_1024,
	OQS_SIG_alg_ml_dsa_44,  OQS_SIG_alg_ml_dsa_65,  OQS_SIG_alg_ml_dsa_87,
};

int main(void)
{
	TCASE("按名字查算法");
	CHECK_EQ_INT(pqc_alg_by_name("ML-KEM-768"), PQC_ALG_ML_KEM_768);
	CHECK_EQ_INT(pqc_alg_by_name("ML-DSA-87"), PQC_ALG_ML_DSA_87);
	CHECK_EQ_INT(pqc_alg_by_name("Kyber768"), PQC_ALG_NONE);   /* round-3 名字必须查不到 */
	CHECK_EQ_INT(pqc_alg_by_name(NULL), PQC_ALG_NONE);
	CHECK(pqc_alg_info(PQC_ALG_NONE) == NULL);

	for (size_t i = 0; i < sizeof(ALGS) / sizeof(ALGS[0]); i++) {
		const pqc_alg_info_t *info = pqc_alg_info(ALGS[i]);
		CHECK(info != NULL);
		if (!info) {
			continue;
		}
		TCASE(info->name);
		/* 名字必须能反查回来 */
		CHECK_EQ_INT(pqc_alg_by_name(info->name), ALGS[i]);

		if (info->kind == PQC_KIND_KEM) {
			OQS_KEM *k = OQS_KEM_new(OQS_NAME[i]);
			CHECK(k != NULL);
			if (!k) {
				continue;
			}
			CHECK_EQ_INT(info->pk_len, k->length_public_key);
			CHECK_EQ_INT(info->sk_len, k->length_secret_key);
			CHECK_EQ_INT(info->ct_len, k->length_ciphertext);
			CHECK_EQ_INT(info->ss_len, k->length_shared_secret);
			CHECK_EQ_INT(info->seed_len, 64);       /* FIPS 203: d‖z */
			CHECK(k->keypair_derand != NULL);       /* ACVP keyGen 依赖它 */
			CHECK(k->encaps_derand != NULL);        /* ACVP encaps 依赖它 */
			OQS_KEM_free(k);
		} else {
			OQS_SIG *s = OQS_SIG_new(OQS_NAME[i]);
			CHECK(s != NULL);
			if (!s) {
				continue;
			}
			CHECK_EQ_INT(info->pk_len, s->length_public_key);
			CHECK_EQ_INT(info->sk_len, s->length_secret_key);
			CHECK_EQ_INT(info->sig_len, s->length_signature);
			CHECK_EQ_INT(info->seed_len, 32);       /* FIPS 204: ξ */
			CHECK(s->sign_with_ctx_str != NULL);    /* external+pure 接口 */
			CHECK(s->verify_with_ctx_str != NULL);
			OQS_SIG_free(s);
		}
	}

	TCASE("后端已就位");
	CHECK(pqc_get_backend() != NULL);
	CHECK(pqc_get_backend()->name != NULL);

	return test_report("test_pqc_meta");
}
