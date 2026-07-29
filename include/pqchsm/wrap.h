/* pqchsm/wrap.h —— KEK 密钥包裹（路线图 §8.2）
 *
 * blob 格式（与 §8.2 的「版本‖算法ID‖元数据‖nonce‖密文‖tag」对应）：
 *
 *   偏移  长度  字段
 *   0     4     magic  "PWRP"
 *   4     2     version
 *   6     2     wrap_alg（1 = AES-256-GCM）
 *   8     4     aad_len   —— 调用方元数据的长度（元数据本身不进 blob，见下）
 *   12    4     ct_len
 *   16    12    nonce
 *   28    ct_len 密文
 *   ...   16    GCM tag
 *
 * 认证范围 = 头部 16 字节 ‖ 调用方 aad。于是版本、算法 ID、长度、以及调用方
 * 传入的槽位元数据全部被 GCM 认证 —— 这就是 §8.2 说的"元数据进 AAD，
 * 防改元数据复用密文"。
 *
 * 元数据本身**不写进 blob**：它在密钥库里已经以明文记录存在（unwrap 时由
 * 调用方重新提供），blob 里再存一份只会造成两处可能不一致。
 *
 * nonce：每次 wrap 由 RAND_bytes 现取 96 bit，对应 SP 800-38D §8.2.2 的
 * RBG-based 构造（无状态、可跨重启，不需要维护计数器）。该构造下同一把 KEK
 * 的调用次数上限是 2^32，本项目的量级（每槽位每次改动一次）远达不到。
 * **严禁复用 nonce** —— 见 tests/unit/test_wrap.c 里那条演示复用后果的测试。
 */
#ifndef PQCHSM_WRAP_H
#define PQCHSM_WRAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PQC_WRAP_VERSION   1
#define PQC_WRAP_HDR_LEN   16
#define PQC_WRAP_NONCE_LEN 12
#define PQC_WRAP_TAG_LEN   16
#define PQC_WRAP_OVERHEAD  (PQC_WRAP_HDR_LEN + PQC_WRAP_NONCE_LEN + PQC_WRAP_TAG_LEN)

#define PQC_WRAP_ALG_AES256GCM 1
#define PQC_KEK_LEN 32

/* 给定明文长度，blob 需要多大 */
size_t pqc_wrap_blob_len(size_t pt_len);

/* 成功返回 0，失败返回 -1。kek_len 必须为 32。 */
int pqc_wrap(const uint8_t *kek, size_t kek_len,
             const uint8_t *aad, size_t aad_len,
             const uint8_t *pt, size_t pt_len,
             uint8_t *blob, size_t cap, size_t *blob_len);

/* 成功返回 0；tag 校验失败、头部不合法、aad 不匹配都返回 -1。
 * **解包失败时 pt 缓冲会被清零**，绝不把未认证的明文交出去。 */
int pqc_unwrap(const uint8_t *kek, size_t kek_len,
               const uint8_t *aad, size_t aad_len,
               const uint8_t *blob, size_t blob_len,
               uint8_t *pt, size_t cap, size_t *pt_len);

/* 由 KDR 派生存储主密钥 KEK（§8.1：KMAC256(KDR, "storage" ‖ 盐)）。
 * KEK 每次开机现场派生、不落盘；salt 存在密钥库头部。 */
int pqc_kek_derive(const uint8_t *salt, size_t salt_len, uint8_t kek[PQC_KEK_LEN]);

#ifdef __cplusplus
}
#endif
#endif /* PQCHSM_WRAP_H */
