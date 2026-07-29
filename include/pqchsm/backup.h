/* pqchsm/backup.h —— 备份与恢复
 *
 * 密钥层级：
 *
 *   KDR（设备根密钥，eFUSE/BBRAM/PUF，不出芯片，不可备份）
 *    └─ KDF ─→ KEK（存储主密钥）──wrap──→ 密钥库文件（**绑定本设备**）
 *
 *   RMK（恢复主密钥，32 B，导出时现场随机生成）
 *    └─ KDF ─→ BEK（备份加密密钥）──wrap──→ 备份文件（**不绑定设备**）
 *    └─ Shamir M-of-N ─→ N 份分片，交由不同保管员离线保管
 *
 * 关键区别：密钥库用 KEK，所以拷走文件换台机器打不开（sealing）；
 * 备份用 RMK，所以**可以**跨设备恢复 —— 代价就是 RMK 的分片必须离线妥善保管。
 * RMK 本身从不落盘、也从不返回给调用方，只以分片形式存在。
 *
 * 只有带 SLOT_POLICY_BACKUPABLE 策略位的槽位才会进备份；其余槽位是
 * "纯 sealed 槽位"（如设备身份钥），设备损坏就是密钥消失，这是有意的。
 */
#ifndef PQCHSM_BACKUP_H
#define PQCHSM_BACKUP_H

#include "pqchsm/slot.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HSM_BACKUP_VERSION 1
#define HSM_RMK_LEN        32
/* 一份分片的字节数：RMK 长度 + Shamir 行开销 */
#define HSM_SHARE_LEN      (HSM_RMK_LEN + 6)
/* 给调用方分配分片二维缓冲用的行宽 */
#define HSM_SHARE_CAP      64

/* 导出备份。需 SO 会话。
 *
 * 现场生成 RMK → 派生 BEK → 逐槽位包裹写文件 → 把 RMK 切成 n 片（阈值 m）
 * 写入 shares（n 行 × share_cap 字节），每行长度回填到 share_lens。
 * 返回后 RMK 在本进程内已被清零 —— 想再解开这个备份，只能靠凑齐 m 份分片。
 *
 * 只导出带 SLOT_POLICY_BACKUPABLE 的槽位；实际导出的槽位数回填到 *n_exported。 */
hsm_status_t hsm_backup_export(hsm_token_t *tok, hsm_session_t sess, const char *path,
                               uint8_t m, uint8_t n,
                               uint8_t *shares, size_t share_cap, size_t *share_lens,
                               size_t *n_exported);

/* 恢复仪式。**设备级操作，不需要会话** —— 因为典型场景就是
 * 一台空白的新设备，上面还没有任何 SO 凭证可供登录。
 * 起这条路径必须由物理"恢复模式"信号 + SO 现场授权门控。
 *
 * 用 k 份分片重构 RMK → 派生 BEK → 解出备份 → 逐槽位装载。
 * 装载后槽位的元数据标签会用**本机 KDR** 重新盖（自动重新 sealing），
 * 调用方随后应立刻 hsm_keystore_save() 把它固化成本机密钥库。
 *
 * 分片不足 m 份时：Shamir 的固有性质是"算得出一个错误的 RMK"而不是报错，
 * 但错误的 RMK 派生出的 BEK 会让 GCM tag 校验失败，所以最终仍然返回
 * HSM_ERR_INTEGRITY。这是本设计特意依赖的一层兜底。 */
hsm_status_t hsm_backup_restore(hsm_token_t *tok, const char *path,
                                const uint8_t *shares, size_t share_cap,
                                const size_t *share_lens, uint8_t k,
                                size_t *n_restored);

#ifdef __cplusplus
}
#endif
#endif /* PQCHSM_BACKUP_H */
