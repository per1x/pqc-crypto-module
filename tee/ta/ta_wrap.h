/* ta_wrap.h —— TA 内 KEK 密钥包裹（PWRP，AES-256-GCM）
 *
 * blob 格式与 include/pqchsm/wrap.h 逐字节一致（两侧可互换解包）：
 *   "PWRP" ‖ ver(LE16) ‖ alg(LE16=1) ‖ aad_len(LE32) ‖ ct_len(LE32)
 *   ‖ nonce(12) ‖ ct ‖ tag(16)
 * AAD = 头部 16 字节 ‖ 调用方 aad。
 * OP-TEE 构建用 TEE crypto API（TEE_ALG_AES_GCM），native 测试构建用
 * OpenSSL EVP（链接 -lcrypto）。
 */
#ifndef PQCHSM_TA_WRAP_H
#define PQCHSM_TA_WRAP_H

#include <stddef.h>
#include <stdint.h>

#define TA_WRAP_VERSION   1
#define TA_WRAP_HDR_LEN   16
#define TA_WRAP_NONCE_LEN 12
#define TA_WRAP_TAG_LEN   16
#define TA_WRAP_OVERHEAD  \
	(TA_WRAP_HDR_LEN + TA_WRAP_NONCE_LEN + TA_WRAP_TAG_LEN)
#define TA_WRAP_ALG_AES256GCM 1
#define TA_KEK_LEN 32

/* 返回值：0 成功；-1 参数/容量/格式错误；-2 认证失败（tag/AAD 不匹配） */
size_t ta_wrap_blob_len(size_t pt_len);

int ta_wrap_seal(const uint8_t kek[TA_KEK_LEN],
                 const uint8_t *aad, size_t aad_len,
                 const uint8_t *pt, size_t pt_len,
                 uint8_t *blob, size_t cap, size_t *blob_len);

/* 认证失败时 pt 缓冲（cap 字节）会被清零，绝不交出未认证的明文 */
int ta_wrap_open(const uint8_t kek[TA_KEK_LEN],
                 const uint8_t *aad, size_t aad_len,
                 const uint8_t *blob, size_t blob_len,
                 uint8_t *pt, size_t cap, size_t *pt_len);

#endif /* PQCHSM_TA_WRAP_H */
