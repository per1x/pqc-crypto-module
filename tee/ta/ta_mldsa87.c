/* ta_mldsa87.c —— ML-DSA-87 单编译单元（结构同 ta_mldsa44.c） */
#define MLD_CONFIG_PARAMETER_SET 87
#define MLD_CONFIG_FILE "pqchsm_mld_config.h"

#include "pqc-native/mldsa/ct.c"
#include "pqc-native/mldsa/debug.c"
#include "pqc-native/mldsa/packing.c"
#include "pqc-native/mldsa/poly.c"
#include "pqc-native/mldsa/poly_kl.c"
#include "pqc-native/mldsa/polyvec.c"
#include "pqc-native/mldsa/polyvec_lazy.c"
#include "pqc-native/mldsa/sign.c"

#include <string.h>

#include "ta_pqc_low.h"

int pqchsm_mld87_keypair(uint8_t *pk, uint8_t *sk)
{
	return mld_sign_keypair(pk, sk, 0);
}

int pqchsm_mld87_keypair_internal(uint8_t *pk, uint8_t *sk,
                                  const uint8_t seed[32])
{
	return mld_sign_keypair_internal(pk, sk, seed, 0);
}

int pqchsm_mld87_sign(uint8_t *sig, size_t *siglen,
                      const uint8_t *m, size_t mlen,
                      const uint8_t *ctx, size_t ctxlen,
                      const uint8_t *sk)
{
	return mld_sign_signature(sig, siglen, m, mlen, ctx, ctxlen, sk, 0);
}

int pqchsm_mld87_verify(const uint8_t *sig, size_t siglen,
                        const uint8_t *m, size_t mlen,
                        const uint8_t *ctx, size_t ctxlen,
                        const uint8_t *pk)
{
	return mld_sign_verify(sig, siglen, m, mlen, ctx, ctxlen, pk, 0);
}

/* 带显式 rnd 的签名（FIPS 204 §Algorithm 2 的那 32 字节）。
 *
 * 为什么必须有：ACVP 的 siggen **确定性**条目就是 rnd = 0³²，没有这个入口
 * 就没法对固定期望值验签名 —— hedged 每次取新随机数，签出来必然对不上。
 * 上层 pqc_backend_t.sign 的 rnd 参数正是喂给它。
 *
 * pre = 0x00 ‖ |ctx| ‖ ctx，与 mld_sign_signature 内部构造的前缀一致
 * （FIPS 204 的 M′）。externalmu=0：m 是原始消息，mu 由库内部算。
 */
int pqchsm_mld87_sign_rnd(uint8_t *sig, size_t *siglen,
                      const uint8_t *m, size_t mlen,
                      const uint8_t *ctx, size_t ctxlen,
                      const uint8_t rnd[32], const uint8_t *sk)
{
	uint8_t pre[257];

	if (ctxlen > 255)
		return -1;
	pre[0] = 0;
	pre[1] = (uint8_t)ctxlen;
	if (ctxlen)
		memcpy(pre + 2, ctx, ctxlen);

	return mld_sign_signature_internal(sig, siglen, m, mlen,
	                                   pre, 2 + ctxlen, rnd, sk, 0, 0);
}

/* 库**自己算出来的**尺寸（params.h 里由参数集推导，不是手抄的常量）。
 * 理由见 ta_mlkem512.c 里同名函数上的说明。 */
void pqchsm_mld87_sizes(size_t *pk, size_t *sk, size_t *sig, size_t *seed)
{
	if (pk)   *pk   = MLDSA_CRYPTO_PUBLICKEYBYTES;
	if (sk)   *sk   = MLDSA_CRYPTO_SECRETKEYBYTES;
	if (sig)  *sig  = MLDSA_CRYPTO_BYTES;
	if (seed) *seed = MLDSA_SEEDBYTES;
}
