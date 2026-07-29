/* pqchsm/kdr.h —— 设备根密钥 KDR（路线图 §8.1 / §8.3）
 *
 * ⚠️ 当前是**桩实现**：固定假根密钥，存在进程内存里。
 * 路线图 §5.7.2 明确了这一点 —— 设备绑定（sealing）到 eFUSE/BBRAM/PUF
 * "只能设计与桩实现，真实绑定必须有板"。Phase 7 起换成：
 *   Zynq-7000: BBRAM/eFUSE AES-256 密钥
 *   Zynq US+ : PUF 生成的设备唯一 KEK
 * 换实现时本头文件不变，上层不受影响。
 *
 * 铁律：KDR 本身永远不出这个模块 —— 外部只能拿到由它派生出来的子密钥。
 */
#ifndef PQCHSM_KDR_H
#define PQCHSM_KDR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PQC_KDR_LEN 32

/* 由 KDR 派生子密钥：out = KMAC256(KDR, salt, custom=label)。
 * label 必须非空（强制域分隔，避免不同用途的子密钥相互替换）。
 * 成功返回 0。注意：**没有** pqc_kdr_get() —— 根密钥不提供读出接口。 */
int pqc_kdr_derive(const char *label,
                   const uint8_t *salt, size_t salt_len,
                   uint8_t *out, size_t out_len);

/* 测试用：重置为另一个"设备"的根密钥，用于跨设备 sealing 负测试。
 * seed == NULL 时恢复默认桩根密钥。 */
void pqc_kdr_set_test_root(const uint8_t *seed, size_t seed_len);

#ifdef __cplusplus
}
#endif
#endif /* PQCHSM_KDR_H */
