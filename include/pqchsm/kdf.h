/* pqchsm/kdf.h —— KMAC256 / SHA3-256 / 密钥派生
 *
 * KMAC256 来自 NIST SP 800-185，本项目里承担两个角色：
 *   1. 密钥派生（路线图 §8.1）：KEK = KMAC256(KDR, 盐, custom="storage")
 *   2. 完整性 tag：对存储对象/审计记录打标签
 * 选 KMAC 而不是 HMAC-SHA2，是因为它自带 custom 域分隔串，
 * 不同用途的派生天然隔离，不需要额外拼接约定。
 */
#ifndef PQCHSM_KDF_H
#define PQCHSM_KDF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* KMAC256(key, data, custom) -> out。custom 为域分隔字符串，可为 NULL。
 * 注意 out_len 参与 KMAC 内部编码：不同 out_len 的结果互不为前缀。
 * 成功返回 0，失败返回 -1。 */
int pqc_kmac256(const uint8_t *key, size_t key_len,
                const uint8_t *data, size_t data_len,
                const char *custom,
                uint8_t *out, size_t out_len);

/* SHA3-256(data) -> out[32]。成功返回 0。审计哈希链用。 */
int pqc_sha3_256(const uint8_t *data, size_t data_len, uint8_t out[32]);

/* 密钥派生：out = KMAC256(ikm, salt, custom=label)。
 * 对应路线图 §8.1 的 KDF: KMAC256(KDR, "storage" || 盐) -> KEK。
 * label 不可为 NULL（强制域分隔）。成功返回 0。 */
int pqc_kdf(const uint8_t *ikm, size_t ikm_len,
            const uint8_t *salt, size_t salt_len,
            const char *label,
            uint8_t *out, size_t out_len);

#ifdef __cplusplus
}
#endif
#endif /* PQCHSM_KDF_H */
