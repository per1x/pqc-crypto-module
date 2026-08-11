/* ta_mlkem512.c —— ML-KEM-512 单编译单元
 *
 * 整个 mlkem-native（ref 后端）编进本文件，命名空间 PQCHSM_MLK512_*。
 * 参数集/配置文件必须先于任何 vendor 头定义。
 */
#define MLK_CONFIG_PARAMETER_SET 512
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

int pqchsm_mlk512_keypair(uint8_t *pk, uint8_t *sk)
{
	return mlk_kem_keypair(pk, sk, 0);
}

int pqchsm_mlk512_keypair_derand(uint8_t *pk, uint8_t *sk,
                                 const uint8_t coins[64])
{
	return mlk_kem_keypair_derand(pk, sk, coins, 0);
}

int pqchsm_mlk512_enc(uint8_t *ct, uint8_t *ss, const uint8_t *pk)
{
	return mlk_kem_enc(ct, ss, pk, 0);
}

int pqchsm_mlk512_enc_derand(uint8_t *ct, uint8_t *ss, const uint8_t *pk,
                             const uint8_t m[32])
{
	return mlk_kem_enc_derand(ct, ss, pk, m, 0);
}

int pqchsm_mlk512_dec(uint8_t *ss, const uint8_t *ct, const uint8_t *sk)
{
	return mlk_kem_dec(ss, ct, sk, 0);
}
