/* pqchsm/inject.h —— 安全密钥注入（路线图 §8.5，Phase 6 第 4 项）
 *
 * 场景：产线或托管方要把一把密钥灌进设备，但**明文密钥不能出现在链路上**，
 * 也不能出现在注入端与设备之间的任何中间环节。
 *
 * 做法（§8.5 那句"用设备的 ML-KEM 公钥封装会话密钥"）：
 *
 *   注入端                                   设备
 *   ------                                   ----
 *   拿到设备注入公钥 ek  ←──────────────────  一个 KEM 槽位的公钥（可公开）
 *   (ct, CEK) = Encaps(ek)
 *   blob = 头 ‖ ct ‖ AES-GCM(CEK, 头, 种子)
 *                    ──────────────────────→ Decaps(dk, ct) 得到同一个 CEK
 *                                             解包得到种子 → 直接装进目标槽位
 *
 * 于是链路上只有密文，注入端算完就把 CEK 清掉，设备端解完也清掉。
 * **这是"用自己实现的 PQC 保护自己的密钥"的自举闭环** —— 同一套 ML-KEM
 * 既是被保护的对象，也是保护手段。
 *
 * 【注入的是种子，不是完整私钥】
 * §7.6 的种子存储让这件事变得干净：注入 32/64 B 的种子，设备端自己重展开
 * 出密钥对。链路上要搬的字节少，且设备可以自行验证展开结果。
 *
 * 【明文注入模式】
 * §8.5 允许"制造模式"下裸灌明文，出厂前用 eFUSE 熔断。这里给的是**桩**：
 * hsm_inject_set_manufacturing_mode() 是一个进程内的开关，Phase 7 换成读
 * eFUSE 位。默认关闭，且关闭后本进程内**不可再打开**（模拟熔断的不可逆）。
 */
#ifndef PQCHSM_INJECT_H
#define PQCHSM_INJECT_H

#include "pqchsm/slot.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HSM_INJECT_VERSION 1

/* 注入端：产出注入 blob。不需要 token —— 注入端通常是另一台机器。
 * device_pk 是设备某个 KEM 槽位的公钥（通过可信渠道取得）。 */
hsm_status_t hsm_inject_build(pqc_alg_t kem_alg,
                              const uint8_t *device_pk, size_t device_pk_len,
                              pqc_alg_t key_alg, uint32_t usage, uint32_t policy,
                              const uint8_t *seed, size_t seed_len,
                              uint8_t *blob, size_t cap, size_t *blob_len);

/* 设备端：用注入槽位的私钥解封装，把密钥装进目标槽位。
 *
 * kem_sess / kem_handle  指向设备的注入钥（需 User 会话，用途含 DECAP）
 * target_sess            指向要装载的目标槽位（需 User 会话）
 *
 * 目标槽位为空时直接装载；已装载时**要求其策略位带 SLOT_POLICY_INJECTABLE**
 * （§7.2 的"可注入更新"），否则返回 HSM_ERR_POLICY —— 不能靠注入无声顶掉一把
 * 本来不允许被替换的密钥。 */
hsm_status_t hsm_inject_apply(hsm_token_t *tok,
                              hsm_session_t kem_sess, hsm_handle_t kem_handle,
                              hsm_session_t target_sess,
                              const uint8_t *blob, size_t blob_len,
                              hsm_handle_t *out);

/* 一次注入需要多大的 blob 缓冲 */
size_t hsm_inject_blob_len(pqc_alg_t kem_alg, size_t seed_len);

/* ---- 制造模式（桩）------------------------------------------------------ */
/* 打开后允许明文注入。**一旦关闭就不能再打开**（模拟 eFUSE 熔断不可逆）。
 * Phase 7 换成读真实的 eFUSE 位。 */
int  hsm_inject_set_manufacturing_mode(int on);
int  hsm_inject_manufacturing_mode(void);

/* 明文注入：只在制造模式下可用，否则返回 HSM_ERR_POLICY。 */
hsm_status_t hsm_inject_plaintext(hsm_token_t *tok, hsm_session_t target_sess,
                                  pqc_alg_t key_alg, uint32_t usage, uint32_t policy,
                                  const uint8_t *seed, size_t seed_len,
                                  hsm_handle_t *out);

#ifdef __cplusplus
}
#endif
#endif /* PQCHSM_INJECT_H */
