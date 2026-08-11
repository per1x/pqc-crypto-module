/* ta_mlkem768.c —— ML-KEM-768 单编译单元（结构同 ta_mlkem512.c） */
#define MLK_CONFIG_PARAMETER_SET 768
#define MLK_CONFIG_FILE "pqchsm_mlk_config.h"

#include "vendor/mlkem/compress.c"
#include "vendor/mlkem/debug.c"
#include "vendor/mlkem/indcpa.c"
#include "vendor/mlkem/kem.c"
#include "vendor/mlkem/poly.c"
#include "vendor/mlkem/poly_k.c"
#include "vendor/mlkem/sampling.c"
#include "vendor/mlkem/verify.c"

#include "ta_pqc_low.h"

int pqchsm_mlk768_keypair(uint8_t *pk, uint8_t *sk)
{
	return mlk_kem_keypair(pk, sk, 0);
}

int pqchsm_mlk768_keypair_derand(uint8_t *pk, uint8_t *sk,
                                 const uint8_t coins[64])
{
	return mlk_kem_keypair_derand(pk, sk, coins, 0);
}

int pqchsm_mlk768_enc(uint8_t *ct, uint8_t *ss, const uint8_t *pk)
{
	return mlk_kem_enc(ct, ss, pk, 0);
}

int pqchsm_mlk768_enc_derand(uint8_t *ct, uint8_t *ss, const uint8_t *pk,
                             const uint8_t m[32])
{
	return mlk_kem_enc_derand(ct, ss, pk, m, 0);
}

int pqchsm_mlk768_dec(uint8_t *ss, const uint8_t *ct, const uint8_t *sk)
{
	return mlk_kem_dec(ss, ct, sk, 0);
}
