/* ta_pqc_low.h —— 各参数集编译单元（ta_mlkemXXX.c / ta_mldsaXX.c）导出的
 * 底层符号原型。只被 ta_pqc.c（调度）和编译单元自己包含。
 * 所有函数返回 0 成功，非 0 为库错误码（mlkem/mldsa 的 MLD/MLK_ERR_*）。
 */
#ifndef PQCHSM_TA_PQC_LOW_H
#define PQCHSM_TA_PQC_LOW_H

#include <stddef.h>
#include <stdint.h>

/* ML-KEM：coins = 64B (d‖z)，m = 32B */
int pqchsm_mlk512_keypair(uint8_t *pk, uint8_t *sk);
int pqchsm_mlk512_keypair_derand(uint8_t *pk, uint8_t *sk,
                                 const uint8_t coins[64]);
int pqchsm_mlk512_enc(uint8_t *ct, uint8_t *ss, const uint8_t *pk);
int pqchsm_mlk512_enc_derand(uint8_t *ct, uint8_t *ss, const uint8_t *pk,
                             const uint8_t m[32]);
int pqchsm_mlk512_dec(uint8_t *ss, const uint8_t *ct, const uint8_t *sk);

int pqchsm_mlk768_keypair(uint8_t *pk, uint8_t *sk);
int pqchsm_mlk768_keypair_derand(uint8_t *pk, uint8_t *sk,
                                 const uint8_t coins[64]);
int pqchsm_mlk768_enc(uint8_t *ct, uint8_t *ss, const uint8_t *pk);
int pqchsm_mlk768_enc_derand(uint8_t *ct, uint8_t *ss, const uint8_t *pk,
                             const uint8_t m[32]);
int pqchsm_mlk768_dec(uint8_t *ss, const uint8_t *ct, const uint8_t *sk);

int pqchsm_mlk1024_keypair(uint8_t *pk, uint8_t *sk);
int pqchsm_mlk1024_keypair_derand(uint8_t *pk, uint8_t *sk,
                                  const uint8_t coins[64]);
int pqchsm_mlk1024_enc(uint8_t *ct, uint8_t *ss, const uint8_t *pk);
int pqchsm_mlk1024_enc_derand(uint8_t *ct, uint8_t *ss, const uint8_t *pk,
                              const uint8_t m[32]);
int pqchsm_mlk1024_dec(uint8_t *ss, const uint8_t *ct, const uint8_t *sk);

/* ML-DSA：seed = 32B (ξ)；sign 为 hedged（rnd 内部取自 TA 随机源） */
int pqchsm_mld44_keypair(uint8_t *pk, uint8_t *sk);
int pqchsm_mld44_keypair_internal(uint8_t *pk, uint8_t *sk,
                                  const uint8_t seed[32]);
int pqchsm_mld44_sign(uint8_t *sig, size_t *siglen,
                      const uint8_t *m, size_t mlen,
                      const uint8_t *ctx, size_t ctxlen,
                      const uint8_t *sk);
int pqchsm_mld44_verify(const uint8_t *sig, size_t siglen,
                        const uint8_t *m, size_t mlen,
                        const uint8_t *ctx, size_t ctxlen,
                        const uint8_t *pk);

int pqchsm_mld65_keypair(uint8_t *pk, uint8_t *sk);
int pqchsm_mld65_keypair_internal(uint8_t *pk, uint8_t *sk,
                                  const uint8_t seed[32]);
int pqchsm_mld65_sign(uint8_t *sig, size_t *siglen,
                      const uint8_t *m, size_t mlen,
                      const uint8_t *ctx, size_t ctxlen,
                      const uint8_t *sk);
int pqchsm_mld65_verify(const uint8_t *sig, size_t siglen,
                        const uint8_t *m, size_t mlen,
                        const uint8_t *ctx, size_t ctxlen,
                        const uint8_t *pk);

int pqchsm_mld87_keypair(uint8_t *pk, uint8_t *sk);
int pqchsm_mld87_keypair_internal(uint8_t *pk, uint8_t *sk,
                                  const uint8_t seed[32]);
int pqchsm_mld87_sign(uint8_t *sig, size_t *siglen,
                      const uint8_t *m, size_t mlen,
                      const uint8_t *ctx, size_t ctxlen,
                      const uint8_t *sk);
int pqchsm_mld87_verify(const uint8_t *sig, size_t siglen,
                        const uint8_t *m, size_t mlen,
                        const uint8_t *ctx, size_t ctxlen,
                        const uint8_t *pk);

#endif /* PQCHSM_TA_PQC_LOW_H */
