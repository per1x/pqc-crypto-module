/* 元数据表与**后端库自己算出来的**尺寸对拍。
 * src/crypto/pqc.c 里那张长度表是上层分配缓冲的唯一依据，一旦与后端脱节
 * 就是缓冲区溢出，所以必须逐项断言，而不是"抄一遍 FIPS 文档就完事"。
 *
 * ⚠️ 这条对拍以前拿的是 liboqs 的运行时值。liboqs 去掉之后（FINAL-PLAN
 *    §8 第 10 项）必须另找一个**不是我们手抄的**来源 —— 否则就变成"两张
 *    手写表互相比"，两处一起抄错时它一声不吭。
 *    现在取的是 vendored 库 params.h 里**按参数集推导出来的宏**
 *    （见 tee/ta/ta_mlkem512.c 的 pqchsm_mlk512_sizes），比 liboqs 那一版
 *    更近一层：它就是真正决定字节数的那份代码。
 */
#include "testlib.h"
#include "pqchsm/pqc.h"

#include "ta_pqc.h"
#include "ta_pqc_low.h"

static const pqc_alg_t ALGS[] = {
	PQC_ALG_ML_KEM_512, PQC_ALG_ML_KEM_768, PQC_ALG_ML_KEM_1024,
	PQC_ALG_ML_DSA_44,  PQC_ALG_ML_DSA_65,  PQC_ALG_ML_DSA_87,
};

/* 每个参数集向库要一次它自己算出来的尺寸 */
typedef void (*kem_sizes_fn)(size_t *, size_t *, size_t *, size_t *);
typedef void (*sig_sizes_fn)(size_t *, size_t *, size_t *, size_t *);

static const kem_sizes_fn KEM_SIZES[] = {
	pqchsm_mlk512_sizes, pqchsm_mlk768_sizes, pqchsm_mlk1024_sizes,
};
static const sig_sizes_fn SIG_SIZES[] = {
	pqchsm_mld44_sizes, pqchsm_mld65_sizes, pqchsm_mld87_sizes,
};

int main(void)
{
	test_use_stub_kdr();   /* 显式装桩：库里没有自动回退了 */
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

		/* 第三张表：TA 那一侧调度层的 dims。它与 pqc.c 的表是两份手写的
		 * 东西，而现在两个世界跑的是同一个后端 —— 两份对不上就意味着
		 * TA 与普通世界对同一把密钥的长度理解不同。 */
		const ta_pqc_dims_t *d = ta_pqc_dims((uint32_t)ALGS[i]);
		CHECK(d != NULL);

		size_t pk = 0, sk = 0, x = 0, y = 0;

		if (info->kind == PQC_KIND_KEM) {
			KEM_SIZES[i](&pk, &sk, &x, &y);
			CHECK_EQ_INT(info->pk_len, pk);
			CHECK_EQ_INT(info->sk_len, sk);
			CHECK_EQ_INT(info->ct_len, x);
			CHECK_EQ_INT(info->ss_len, y);
			CHECK_EQ_INT(info->seed_len, 64);       /* FIPS 203: d‖z */
			if (d) {
				CHECK_EQ_INT(d->pk_len, pk);
				CHECK_EQ_INT(d->sk_len, sk);
				CHECK_EQ_INT(d->ct_len, x);
				CHECK_EQ_INT(d->ss_len, y);
				CHECK_EQ_INT(d->is_kem, 1);
			}
		} else {
			SIG_SIZES[i - 3](&pk, &sk, &x, &y);
			CHECK_EQ_INT(info->pk_len, pk);
			CHECK_EQ_INT(info->sk_len, sk);
			CHECK_EQ_INT(info->sig_len, x);
			CHECK_EQ_INT(info->seed_len, y);        /* FIPS 204: ξ = 32 */
			CHECK_EQ_INT(y, 32);
			if (d) {
				CHECK_EQ_INT(d->pk_len, pk);
				CHECK_EQ_INT(d->sk_len, sk);
				CHECK_EQ_INT(d->sig_len, x);
				CHECK_EQ_INT(d->is_kem, 0);
			}
		}
	}

	TCASE("后端已就位，而且不再是 liboqs");
	CHECK(pqc_get_backend() != NULL);
	CHECK(pqc_get_backend()->name != NULL);
	/* 名字里不许再出现 liboqs —— 依赖已经去掉，一个说谎的名字比没有注释更贵 */
	CHECK(strstr(pqc_get_backend()->name, "liboqs") == NULL);

	return test_report("test_pqc_meta");
}
