/* 上电自测与错误状态
 *
 * 三件事各自成立才算这一层有意义：
 *   1. 全部五项已知答案测试通过；
 *   2. 错误状态**确实**阻断密码运算 —— 没有这一条，自测只是个不影响任何东西的
 *      布尔值；
 *   3. 从错误状态恢复的唯一途径是重跑自测并通过。
 *
 * 另加一条反证：把期望值本身改坏是做不到的（它们是编译进去的常量），
 * 因此改用强制错误状态来验证"闸门真的会拦"，并确认闸门放行时运算又能正常做完。
 */
#include "testlib.h"
#include "pqchsm/pqc.h"
#include "pqchsm/selftest.h"

#include <stdlib.h>

static void test_all_pass(void)
{
	TCASE("五项已知答案测试全部通过");
	uint32_t fails = pqc_self_test();
	if (fails) {
		for (int i = 0; i < PQC_ST__COUNT; i++) {
			if (fails & (1u << i)) {
				fprintf(stderr, "  失败项: %s\n",
				        pqc_selftest_name((pqc_selftest_id_t)i));
			}
		}
	}
	CHECK_EQ_INT(fails, 0);
	CHECK_EQ_INT(pqc_self_test_passed(), 1);
	CHECK_EQ_INT(pqc_self_test_failures(), 0);
}

static void test_names_are_distinct(void)
{
	TCASE("每一项都有名字，且互不相同");
	for (int i = 0; i < PQC_ST__COUNT; i++) {
		const char *a = pqc_selftest_name((pqc_selftest_id_t)i);
		CHECK(a != NULL && a[0] != '\0');
		for (int j = i + 1; j < PQC_ST__COUNT; j++) {
			CHECK(strcmp(a, pqc_selftest_name((pqc_selftest_id_t)j)) != 0);
		}
	}
}

/* 闸门有没有效，只能靠"进入错误状态之后运算必须失败"来判定 */
static void test_error_state_blocks_operations(void)
{
	const pqc_alg_info_t *kem = pqc_alg_info(PQC_ALG_ML_KEM_768);
	const pqc_alg_info_t *sig = pqc_alg_info(PQC_ALG_ML_DSA_65);
	uint8_t *pk = malloc(kem->pk_len);
	uint8_t *sk = malloc(kem->sk_len);
	uint8_t *ct = malloc(kem->ct_len);
	uint8_t  ss[32];
	uint8_t  seed[64] = { 0 };
	uint8_t *spk = malloc(sig->pk_len);
	uint8_t *ssk = malloc(sig->sk_len);
	uint8_t *sigbuf = malloc(sig->sig_len);
	size_t   siglen = sig->sig_len;

	TCASE("正常状态下运算可以完成");
	CHECK_EQ_INT(pqc_keypair_from_seed(PQC_ALG_ML_KEM_768, seed, kem->seed_len, pk, sk),
	             PQC_OK);
	CHECK_EQ_INT(pqc_encaps(PQC_ALG_ML_KEM_768, pk, ct, ss), PQC_OK);
	CHECK_EQ_INT(pqc_keypair_from_seed(PQC_ALG_ML_DSA_65, seed, sig->seed_len, spk, ssk),
	             PQC_OK);
	CHECK_EQ_INT(pqc_sign(PQC_ALG_ML_DSA_65, ssk, (const uint8_t *)"m", 1,
	                      NULL, 0, NULL, sigbuf, &siglen),
	             PQC_OK);

	TCASE("错误状态下所有密码运算一律拒绝");
	pqc_self_test_force_error(1);
	CHECK_EQ_INT(pqc_self_test_passed(), 0);
	CHECK_EQ_INT(pqc_keypair(PQC_ALG_ML_KEM_768, pk, sk), PQC_ERR_SELF_TEST);
	CHECK_EQ_INT(pqc_keypair_from_seed(PQC_ALG_ML_KEM_768, seed, kem->seed_len, pk, sk),
	             PQC_ERR_SELF_TEST);
	CHECK_EQ_INT(pqc_encaps(PQC_ALG_ML_KEM_768, pk, ct, ss), PQC_ERR_SELF_TEST);
	CHECK_EQ_INT(pqc_encaps_derand(PQC_ALG_ML_KEM_768, pk, ss, 32, ct, ss),
	             PQC_ERR_SELF_TEST);
	CHECK_EQ_INT(pqc_decaps(PQC_ALG_ML_KEM_768, sk, ct, ss), PQC_ERR_SELF_TEST);
	CHECK_EQ_INT(pqc_sign(PQC_ALG_ML_DSA_65, ssk, (const uint8_t *)"m", 1,
	                      NULL, 0, NULL, sigbuf, &siglen),
	             PQC_ERR_SELF_TEST);
	CHECK_EQ_INT(pqc_verify(PQC_ALG_ML_DSA_65, spk, (const uint8_t *)"m", 1,
	                        NULL, 0, sigbuf, siglen),
	             PQC_ERR_SELF_TEST);

	TCASE("错误状态下参数校验也不先行 —— 不能借非法参数绕过闸门");
	CHECK_EQ_INT(pqc_keypair(PQC_ALG_NONE, NULL, NULL), PQC_ERR_SELF_TEST);
	CHECK_EQ_INT(pqc_decaps(PQC_ALG_ML_DSA_65, NULL, NULL, NULL), PQC_ERR_SELF_TEST);

	TCASE("重跑自测通过后恢复服务");
	pqc_self_test_force_error(0);
	CHECK_EQ_INT(pqc_self_test_passed(), 1);
	siglen = sig->sig_len;
	CHECK_EQ_INT(pqc_sign(PQC_ALG_ML_DSA_65, ssk, (const uint8_t *)"m", 1,
	                      NULL, 0, NULL, sigbuf, &siglen),
	             PQC_OK);
	CHECK_EQ_INT(pqc_verify(PQC_ALG_ML_DSA_65, spk, (const uint8_t *)"m", 1,
	                        NULL, 0, sigbuf, siglen),
	             PQC_OK);

	TCASE("错误状态有独立的错误码，不与既有状态混淆");
	CHECK(pqc_strerror(PQC_ERR_SELF_TEST) != NULL);
	CHECK(strcmp(pqc_strerror(PQC_ERR_SELF_TEST),
	             pqc_strerror(PQC_ERR_BACKEND)) != 0);
	CHECK(strcmp(pqc_strerror(PQC_ERR_SELF_TEST), "unknown") != 0);

	free(pk); free(sk); free(ct); free(spk); free(ssk); free(sigbuf);
}

static void test_repeatable(void)
{
	TCASE("自测可重复运行，结果稳定");
	for (int i = 0; i < 3; i++) {
		CHECK_EQ_INT(pqc_self_test(), 0);
	}
}

int main(void)
{
	test_use_stub_kdr();   /* 显式装桩：库里没有自动回退了 */
	test_all_pass();
	test_names_are_distinct();
	test_error_state_blocks_operations();
	test_repeatable();
	return test_report("test_selftest");
}
