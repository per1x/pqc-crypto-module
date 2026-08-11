/* ta_pqc.h —— TA 内 PQC 调度层：算法编号 → 参数集编译单元
 *
 * 算法编号与 include/pqchsm/pqc.h 的 pqc_alg_t 数值一致（也等于
 * tee/include/pqchsm_ta_proto.h 的 TA_ALG_*）。尺寸取自 FIPS 203/204。
 * 返回码沿用 pqc_status_t 的数值：0 成功，-1 参数错误，-2 不支持，
 * -3 后端内部失败，-4 校验不通过（验签失败）。
 */
#ifndef PQCHSM_TA_PQC_H
#define PQCHSM_TA_PQC_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint32_t alg;
	int      is_kem;
	size_t   pk_len;
	size_t   sk_len;
	size_t   ct_len;   /* KEM 密文 */
	size_t   ss_len;   /* KEM 共享秘密 */
	size_t   sig_len;  /* 签名（定长） */
	size_t   seed_len; /* ML-KEM 64B (d‖z)；ML-DSA 32B (ξ) */
} ta_pqc_dims_t;

/* alg 非法返回 NULL */
const ta_pqc_dims_t *ta_pqc_dims(uint32_t alg);

int ta_pqc_keypair(uint32_t alg, uint8_t *pk, uint8_t *sk);
int ta_pqc_keypair_from_seed(uint32_t alg, const uint8_t *seed,
                             size_t seed_len, uint8_t *pk, uint8_t *sk);
int ta_pqc_encaps(uint32_t alg, const uint8_t *pk, uint8_t *ct, uint8_t *ss);
int ta_pqc_decaps(uint32_t alg, const uint8_t *sk, const uint8_t *ct,
                  uint8_t *ss);
int ta_pqc_sign(uint32_t alg, const uint8_t *sk,
                const uint8_t *msg, size_t msg_len,
                const uint8_t *ctx, size_t ctx_len,
                uint8_t *sig, size_t *sig_len);
int ta_pqc_verify(uint32_t alg, const uint8_t *pk,
                  const uint8_t *msg, size_t msg_len,
                  const uint8_t *ctx, size_t ctx_len,
                  const uint8_t *sig, size_t sig_len);

#endif /* PQCHSM_TA_PQC_H */
