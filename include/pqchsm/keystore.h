/* pqchsm/keystore.h —— 密钥库持久化（路线图 §7.5 L2 层 / Phase 5）
 *
 * 落盘的永远是**密文**：每个槽位的 PIN 材料与密钥材料用 KEK 包裹，
 * 元数据以明文记录存在但进 GCM 的 AAD，因此改元数据一定被检出（§8.2）。
 * 被盗的密钥库文件在没有设备 KDR 的情况下只是一堆密文（§8.3 sealing）。
 *
 * 文件格式：
 *   magic "PQCHSMKS"(8) | version u32 | n_slots u32 | kek_salt(16)
 *   每槽位： blob_len u32 | blob    （blob = meta_wire ‖ wrap 密文）
 *   file_tag(32) = KMAC256(K_file, 以上全部, custom="pqc-hsm/keystore")
 *
 * file_tag 的作用是检出**记录级**的攻击：删掉一条记录、调换两条记录、
 * 截断文件 —— 这些单看每条记录都是合法的，只有全文件 MAC 能发现。
 *
 * 每次 save 都重新随机 kek_salt ⇒ 每次落盘都是一把新 KEK、全部记录重新包裹。
 * 这顺带把 §8.1 的「KEK 轮换」变成了默认行为，也让 GCM nonce 的复用风险
 * 进一步下降（新 KEK = 全新的 nonce 空间）。
 *
 * 原子性：先写 <path>.tmp、fsync、再 rename 覆盖（rename 在 POSIX 上是原子的），
 * 最后 fsync 目录。任何时刻断电，<path> 要么是旧的完整文件、要么是新的完整文件，
 * 绝不会半新半旧（§5.7.3 的掉电一致性要求）。
 */
#ifndef PQCHSM_KEYSTORE_H
#define PQCHSM_KEYSTORE_H

#include "pqchsm/slot.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HSM_KEYSTORE_VERSION 1

/* 把整个 token 落盘。 */
hsm_status_t hsm_keystore_save(hsm_token_t *tok, const char *path);

/* 从文件恢复到 token。token 的槽位数必须与文件一致。
 * 任何完整性校验失败都返回 HSM_ERR_INTEGRITY，且**不改动 token 的现有内容**。 */
hsm_status_t hsm_keystore_load(hsm_token_t *tok, const char *path);

/* ---- 测试钩子 ----------------------------------------------------------- */
/* 令下一次 save 在第 n 次写操作后立刻 _exit(9)，用于模拟掉电。
 * n < 0 关闭。仅供 tests/unit/test_keystore.c 在子进程里使用。 */
void hsm_keystore_set_crash_point(int n);
/* 上一次 save 一共发生了多少次写操作（用来穷举所有崩溃点） */
int  hsm_keystore_last_write_count(void);

#ifdef __cplusplus
}
#endif
#endif /* PQCHSM_KEYSTORE_H */
