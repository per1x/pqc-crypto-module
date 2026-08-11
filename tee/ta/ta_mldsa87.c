/* ta_mldsa87.c —— ML-DSA-87 单编译单元（结构同 ta_mldsa44.c） */
#define MLD_CONFIG_PARAMETER_SET 87
#define MLD_CONFIG_FILE "pqchsm_mld_config.h"

#include "vendor/mldsa/ct.c"
#include "vendor/mldsa/debug.c"
#include "vendor/mldsa/packing.c"
#include "vendor/mldsa/poly.c"
#include "vendor/mldsa/poly_kl.c"
#include "vendor/mldsa/polyvec.c"
#include "vendor/mldsa/polyvec_lazy.c"
#include "vendor/mldsa/sign.c"

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
