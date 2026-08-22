/* ta_mlkem1024.c —— ML-KEM-1024 单编译单元（结构同 ta_mlkem512.c） */
#define MLK_CONFIG_PARAMETER_SET 1024
#define MLK_CONFIG_FILE "pqchsm_mlk_config.h"

#include "pqc-native/mlkem/compress.c"
#include "pqc-native/mlkem/debug.c"
#include "pqc-native/mlkem/indcpa.c"
#include "pqc-native/mlkem/kem.c"
#include "pqc-native/mlkem/poly.c"
#include "pqc-native/mlkem/poly_k.c"
#include "pqc-native/mlkem/sampling.c"
#include "pqc-native/mlkem/verify.c"

#include "ta_pqc_low.h"

int pqchsm_mlk1024_keypair(uint8_t *pk, uint8_t *sk)
{
	return mlk_kem_keypair(pk, sk, 0);
}

int pqchsm_mlk1024_keypair_derand(uint8_t *pk, uint8_t *sk,
                                  const uint8_t coins[64])
{
	return mlk_kem_keypair_derand(pk, sk, coins, 0);
}

int pqchsm_mlk1024_enc(uint8_t *ct, uint8_t *ss, const uint8_t *pk)
{
	return mlk_kem_enc(ct, ss, pk, 0);
}

int pqchsm_mlk1024_enc_derand(uint8_t *ct, uint8_t *ss, const uint8_t *pk,
                              const uint8_t m[32])
{
	return mlk_kem_enc_derand(ct, ss, pk, m, 0);
}

int pqchsm_mlk1024_dec(uint8_t *ss, const uint8_t *ct, const uint8_t *sk)
{
	return mlk_kem_dec(ss, ct, sk, 0);
}

/* 库**自己算出来的**尺寸（params.h 里由参数集推导，不是手抄的常量）。
 * 上层 test_pqc_meta 拿它与 pqc.c 的长度表逐项对拍 —— 那张表是分配缓冲的
 * 唯一依据，与后端脱节就是缓冲区溢出。
 *
 * ⚠️ 这条对拍以前是拿 liboqs 的运行时值做的。liboqs 去掉之后必须另找一个
 *    **不是我们手抄的**来源，否则就成了"两张手写表互相比"——两处一起抄错
 *    时它一声不吭。这里取的是 vendored 库按参数集推出来的宏，比 liboqs
 *    那一版更近一层。 */
void pqchsm_mlk1024_sizes(size_t *pk, size_t *sk, size_t *ct, size_t *ss)
{
	if (pk) *pk = MLKEM_INDCCA_PUBLICKEYBYTES;
	if (sk) *sk = MLKEM_INDCCA_SECRETKEYBYTES;
	if (ct) *ct = MLKEM_INDCCA_CIPHERTEXTBYTES;
	if (ss) *ss = MLKEM_SSBYTES;
}
