/* pqchsm/kdr.h —— 设备根密钥 KDR 与派生层级
 *
 * 【铁律，也是本头文件的全部设计】
 * **KDR 只进不出**：这里没有、将来也不会有任何形如 pqc_kdr_get() 的接口。
 * 外部只能拿到由它派生出来的子密钥。这条在软件阶段靠"接口里不存在读出函数"保证，
 * 之后靠"地址译码上物理不存在读回路径"保证 —— 两个阶段是同一句话的两种实现。
 * tests/unit/test_kdr.c 里有一条结构性回归测试，扫本头文件确保没人偷偷加回读出接口。
 *
 * 【provider 抽象：现在就把结构定死，将来只换实现】
 * 派生链的**结构**（域分隔串、每类子密钥的用途隔离、KEK 轮换流程）是纯软件，
 * 只有"根密钥从哪来"这一个点需要硬件。所以把根密钥抽象成 provider：
 *
 *     stub    —— 当前唯一实现，源码里一段固定的假根密钥（src/crypto/kdr.c）
 *     bbram   —— ：Zynq-7000 的 BBRAM AES-256 密钥
 *     efuse   —— ：eFUSE（⚠️ 烧录不可逆，先在 BBRAM 上跑通再考虑）
 *     puf     —— ：Zynq UltraScale+ 的 PUF 生成设备唯一 KEK（首选）
 *
 * 换 provider 时上层一行不改 —— 与 pqchsm/pqc.h 的算法后端 vtable 是同一个套路。
 *
 * 【派生层级】
 *
 *   KDR ─┬─ "pqc-hsm/storage-kek"      → KEK      包裹密钥库（绑定设备）
 *        ├─ "pqc-hsm/keystore-filemac" → K_file   密钥库全文件 MAC
 *        ├─ "pqc-hsm/slot-meta-key"    → K_meta   槽位元数据 KMAC 标签（按 slot_id 分盐）
 *        └─ …（新增用途一律新增域分隔串，绝不复用）
 *
 *   备份路径**不经过 KDR**（RMK → BEK），那是它能跨设备恢复的原因，见 backup.h。
 *   PIN 验证值也不经过 KDR（用每槽位随机的 pin_key），原因见 persist.c。
 */
#ifndef PQCHSM_KDR_H
#define PQCHSM_KDR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PQC_KDR_LEN 32

/* 根密钥 provider。注意这张表里**没有**读出根密钥的函数，这是有意的。 */
typedef struct pqc_kdr_provider {
	const char *name;

	/* out = KDF(root, salt, custom=label)。实现内部持有根密钥。
	 * 成功返回 0。 */
	int (*derive)(const char *label, const uint8_t *salt, size_t salt_len,
	              uint8_t *out, size_t out_len);

	/* 根密钥是否有硬件保证（不出芯片、不可读出）。
	 * stub 为 0 —— 上层可以据此在启动自检里拒绝进入"生产模式"。 */
	int hardware_backed;
} pqc_kdr_provider_t;

/* 当前唯一实现：桩。⚠️ hardware_backed == 0。 */
const pqc_kdr_provider_t *pqc_kdr_provider_stub(void);

/* 设置/查询当前 provider。p == NULL 恢复默认（stub）。 */
void                       pqc_kdr_set_provider(const pqc_kdr_provider_t *p);
const pqc_kdr_provider_t  *pqc_kdr_get_provider(void);

/* 便捷判定：当前根密钥是否有硬件保证。
 * 启动自检（POST）里应当检查它 —— 之后为 0 就说明信任根没落地。 */
int pqc_kdr_is_hardware_backed(void);

/* 由 KDR 派生子密钥。label 必须非空（强制域分隔，避免不同用途的子密钥互相替换）。
 * 成功返回 0。 */
int pqc_kdr_derive(const char *label, const uint8_t *salt, size_t salt_len,
                   uint8_t *out, size_t out_len);

/* 测试用：把桩 provider 的根密钥换成另一个"设备"，用于跨设备 sealing 负测试。
 * seed == NULL 时恢复默认桩根密钥。对非 stub provider 无效果。 */
void pqc_kdr_set_test_root(const uint8_t *seed, size_t seed_len);

#ifdef __cplusplus
}
#endif
#endif /* PQCHSM_KDR_H */
