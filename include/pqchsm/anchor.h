/* pqchsm/anchor.h —— 审计链头的签名固化（路线图 §8.6 最后一句）
 *
 * 【为什么需要它】
 * 纯哈希链（pqchsm/audit.h）只保证"改动会向后传播"，**不保证"改动无法被抹平"**：
 * 算法是公开的、链头就写在日志文件头里，能写整个文件的攻击者完全可以从任意一点
 * 重算整条链、把 count 与 head 一并改成自洽的值，交出一份 audit_verify_file
 * 挑不出毛病的假日志。tests/unit/test_audit.c 里那条反向断言跑的就是这个洞。
 *
 * 堵它的唯一办法是**外部锚点**：把 (count, head, 时间) 用设备身份钥签名，
 * 送到这台设备改不到的地方（另一台机器 / 上级系统 / 打印存档）。
 * 攻击者要抹平就得先伪造 ML-DSA 签名 —— 那才谈得上不可否认。
 *
 * 【锚点覆盖的是前缀，不是全部】
 * 一次锚定只固化"当时的前 count 条"。之后新追加的记录不受该锚点保护，
 * 直到下一次锚定。所以实际部署要**定期**锚定（§8.6 的"定期"二字是本质的）：
 * 两次锚定之间的窗口，就是攻击者仍可自由改写的范围。
 * hsm_audit_anchor_verify() 允许日志比锚点长，但**前 count 条必须逐字节还是当初那一段**。
 *
 * 【信任从哪来】
 * 校验时必须由调用方给出**out-of-band 得到的**设备身份公钥。锚点文件里也存了一份
 * 公钥，但那只用来标识"谁签的"——拿文件里的公钥去验文件里的签名是循环论证。
 * 首次建立信任用 hsm_audit_anchor_peek_pk() 把公钥读出来，通过可信渠道核对后固定下来。
 *
 * 锚点文件格式（全部显式小端）：
 *   magic "PQCHSMAN"(8) | version u32 | alg u32 | count u64 | head[32] | timestamp u64
 *   | pk_len u32 | pk | sig_len u32 | sig
 * 被签名的消息 = 上述从 magic 到 timestamp 的定长前 64 字节（不含公钥与签名本身）。
 */
#ifndef PQCHSM_ANCHOR_H
#define PQCHSM_ANCHOR_H

#include "pqchsm/audit.h"
#include "pqchsm/slot.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HSM_ANCHOR_VERSION 1
/* 被签名的定长前缀长度 */
#define HSM_ANCHOR_SIGNED_LEN 64

/* 生成锚点：取日志当前的 (count, head)，用槽位里的身份签名钥签名后写文件。
 *
 * 需要 User 会话且该句柄具备 KEY_USAGE_SIGN；身份钥用哪个槽位由调用方决定
 * （建议专用一个不可备份的"纯 sealed 槽位"当设备身份钥，见 §8.3）。
 * timestamp 由调用方给，便于测试可复现，也便于与外部时间源对齐。
 *
 * 注意：本函数**不**先跑 audit_verify_file —— 锚定的语义是"把此刻的链头固化下来"，
 * 至于这条链此刻是否已被动过，那是 verify 的职责。调用方应当先验后签。 */
hsm_status_t hsm_audit_anchor_create(audit_log_t *log, const char *anchor_path,
                                     hsm_token_t *tok, hsm_session_t sess,
                                     hsm_handle_t identity_key, uint64_t timestamp);

/* 校验锚点与日志。四件事全部通过才返回 HSM_OK：
 *   1. 锚点文件里的签名在 expect_pk 下有效（expect_pk 必须由调用方带来）；
 *   2. 锚点里的公钥与 expect_pk 一致；
 *   3. 日志自身的哈希链完好（audit_verify_file）；
 *   4. 日志前 count 条之后的链哈希 == 锚点里被签过的 head。
 *
 * 任何一条不成立返回 HSM_ERR_INTEGRITY。
 * anchored_count 可为 NULL；否则回填锚点覆盖到第几条。 */
hsm_status_t hsm_audit_anchor_verify(const char *log_path, const char *anchor_path,
                                     const uint8_t *expect_pk, size_t expect_pk_len,
                                     uint64_t *anchored_count);

/* 首次建立信任用：把锚点里的公钥读出来（**不校验任何东西**）。
 * 读出来之后必须通过可信渠道核对，不能直接拿它去验同一个文件。 */
hsm_status_t hsm_audit_anchor_peek_pk(const char *anchor_path, pqc_alg_t *alg,
                                      uint8_t *pk, size_t cap, size_t *pk_len);

#ifdef __cplusplus
}
#endif
#endif /* PQCHSM_ANCHOR_H */
