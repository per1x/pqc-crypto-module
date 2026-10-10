/* ta_wrap.h —— TA 内 KEK 密钥包裹（PWRP，AES-256-GCM）
 *
 * blob 格式由 include/pqchsm/pwrp_format.h **单点定义**，两侧共用一份：
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

/* ⚠️ 线格式**不在这里定义**，在仓库根 include/pqchsm/pwrp_format.h ——
 * 普通世界那份实现（src/store/wrap.c，用 OpenSSL）include 的是同一个头。
 * 以前两边各写了一份宏（TA_WRAP_* 与 PQC_WRAP_*），改一处忘另一处不会有
 * 任何编译错误，症状是"TA 包的 blob 普通世界解不开、错误码却是认证失败"，
 * 与密钥不对/文件被改完全分不开。理由全文见那个文件头。 */
#include "pqchsm/pwrp_format.h"

#define TA_WRAP_VERSION   PWRP_VERSION
#define TA_WRAP_HDR_LEN   PWRP_HDR_LEN
#define TA_WRAP_NONCE_LEN PWRP_NONCE_LEN
#define TA_WRAP_TAG_LEN   PWRP_TAG_LEN
#define TA_WRAP_OVERHEAD  PWRP_OVERHEAD
#define TA_WRAP_ALG_AES256GCM PWRP_ALG_AES256GCM
#define TA_KEK_LEN PWRP_KEK_LEN

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
