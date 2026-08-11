/* ta_kdf.h —— TA 内 KMAC-256 / 密钥派生（SP 800-185）
 *
 * 语义与 src/crypto/kdf.c（OpenSSL EVP_MAC KMAC-256）逐字节一致：
 *   ta_kdf_derive(ikm, salt, label) = KMAC256(K=ikm, X=salt, L=out*8, S=label)
 * native 测试用 OpenSSL 对拍保证。
 */
#ifndef PQCHSM_TA_KDF_H
#define PQCHSM_TA_KDF_H

#include <stddef.h>
#include <stdint.h>

/* KMAC256(key, data, custom) -> out。custom 为域分隔串（可为 NULL/空）。
 * out_len 参与 right_encode 编码。成功 0，失败 -1。 */
int ta_kmac256(const uint8_t *key, size_t key_len,
               const uint8_t *data, size_t data_len,
               const uint8_t *custom, size_t custom_len,
               uint8_t *out, size_t out_len);

/* out = KMAC256(ikm, salt, custom=label)。label 必须为非空 C 字符串。 */
int ta_kdf_derive(const uint8_t *ikm, size_t ikm_len,
                  const uint8_t *salt, size_t salt_len,
                  const char *label,
                  uint8_t *out, size_t out_len);

#endif /* PQCHSM_TA_KDF_H */
