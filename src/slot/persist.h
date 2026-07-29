/* 内部头：单槽位状态 ↔ 密文 blob。keystore 与 backup 共用。 */
#ifndef PQCHSM_SLOT_PERSIST_H
#define PQCHSM_SLOT_PERSIST_H

#include "pqchsm/slot.h"

/* 一条槽位 blob 的最大字节数（按最大参数集算） */
size_t hsm_slot_blob_max(void);

/* 设备级（无会话）：密钥库与备份模块使用。
 * 会话级的策略检查（可否备份/可否导出）由上层做。 */
hsm_status_t hsm_slot_serialize(hsm_token_t *tok, hsm_slot_id_t id,
                                const uint8_t *kek, size_t kek_len,
                                uint8_t *blob, size_t cap, size_t *blob_len);

hsm_status_t hsm_slot_deserialize(hsm_token_t *tok, hsm_slot_id_t id,
                                  const uint8_t *kek, size_t kek_len,
                                  const uint8_t *blob, size_t blob_len);

#endif
