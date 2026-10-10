/* pqchsm_ta_client.h —— pqc-hsm TA 的普通世界客户端（libteec）
 *
 * 形状对齐 pqc_backend_t + wrap/kek 接口，供槽位管理器/PKCS#11 前端
 * 把后端从软件（liboqs）切到 TA。所有长度上限见 pqchsm_ta_proto.h。
 * 返回 0 成功，非 0 为 TEEC_Result 或 -1（本地参数错误）。
 */
#ifndef PQCHSM_TA_CLIENT_H
#define PQCHSM_TA_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include <tee_client_api.h>

typedef struct {
	TEEC_Context ctx;
	TEEC_Session sess;
	int          open;
} pqchsm_ta;

int  pqchsm_ta_open(pqchsm_ta *t);
void pqchsm_ta_close(pqchsm_ta *t);

int pqchsm_ta_get_info(pqchsm_ta *t, uint32_t *version, uint32_t *features);

/* ⚠️ pqchsm_ta_kdf / _wrap / _unwrap **已删除**（PS-22 / PS-24）：
 * 它们分别是"拿 TA 内 KDR 按任意 label 派生并交出来"和"任意 blob 解包"
 * 两台谕言机，而产品路径一条都不用。别加回来 —— 理由与完整分析见
 * tee/include/pqchsm_ta_proto.h 里那段墓碑注释。 */

/* salt 来自密钥库头部；调用后 KEYGEN/SIGN/DECAPS 才可用。
 * 它只在 TA 内缓存 KEK，没有任何把 KEK 或明文交出去的出口。 */
int pqchsm_ta_kek_set(pqchsm_ta *t, const uint8_t *salt, size_t salt_len);

/* pk 长度由算法决定（见 ta_pqc_dims / pqc_alg_info），调用方须给足 */
int pqchsm_ta_keygen(pqchsm_ta *t, uint32_t alg,
                     uint8_t *pk,
                     uint8_t *blob, size_t cap, size_t *blob_len);
int pqchsm_ta_keygen_from_seed(pqchsm_ta *t, uint32_t alg,
                               const uint8_t *seed, size_t seed_len,
                               uint8_t *pk,
                               uint8_t *blob, size_t cap, size_t *blob_len);

int pqchsm_ta_decaps(pqchsm_ta *t, uint32_t alg,
                     const uint8_t *blob, size_t blob_len,
                     const uint8_t *ct, size_t ct_len,
                     uint8_t *ss, size_t ss_len);

int pqchsm_ta_sign(pqchsm_ta *t, uint32_t alg,
                   const uint8_t *blob, size_t blob_len,
                   const uint8_t *ctx, size_t ctx_len,
                   const uint8_t *msg, size_t msg_len,
                   uint8_t *sig, size_t cap, size_t *sig_len);

#endif /* PQCHSM_TA_CLIENT_H */
