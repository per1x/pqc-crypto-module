/* 内部头：槽位元数据序列化与 KMAC 完整性标签 */
#ifndef PQCHSM_SLOT_META_H
#define PQCHSM_SLOT_META_H

#include "pqchsm/slot.h"

#define SLOT_META_TAG_LEN 32
/* 序列化后的定长字节数 */
#define SLOT_META_WIRE_LEN 92

/* 确定性序列化：定长、显式小端，不依赖结构体布局/对齐。
 * 返回写入字节数，缓冲不足返回 -1。 */
long slot_meta_serialize(const slot_meta_t *m, uint8_t *out, size_t cap);

/* tag = KMAC256(K_meta, serialize(m), custom="pqc-hsm/slot-meta")
 * 其中 K_meta 由 KDR 按 slot_id 派生 —— 因此标签同时绑定设备与槽位号，
 * 把 slot 3 的记录整条搬到 slot 5 也会被检出。 */
int slot_meta_seal(const slot_meta_t *m, uint8_t tag[SLOT_META_TAG_LEN]);

/* 反序列化：从 wire 恢复 slot_meta_t。返回消费字节数，失败 -1。 */
long slot_meta_deserialize(slot_meta_t *m, const uint8_t *in, size_t len);

/* 常量时间比对；相符返回 0，不符返回 -1 */
int slot_meta_verify(const slot_meta_t *m, const uint8_t tag[SLOT_META_TAG_LEN]);

#endif
