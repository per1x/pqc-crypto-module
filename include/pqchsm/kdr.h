/* pqchsm/kdr.h —— 设备根密钥 KDR 与派生层级
 *
 * 【铁律，也是本头文件的全部设计】
 * **KDR 只进不出**：这里没有、将来也不会有任何形如 pqc_kdr_get() 的接口。
 * 外部只能拿到由它派生出来的子密钥。这条在软件阶段靠"接口里不存在读出函数"保证，
 * 之后靠"地址译码上物理不存在读回路径"保证 —— 两个阶段是同一句话的两种实现。
 * tools/check_no_readback.py 是一条结构性回归检查（由 ctest 驱动），扫本头文件
 * 确保没有任何读出根密钥的接口被加回来。
 *
 * 【provider 抽象：现在就把结构定死，将来只换实现】
 * 派生链的**结构**（域分隔串、每类子密钥的用途隔离、KEK 轮换流程）是纯软件，
 * 只有"根密钥从哪来"这一个点需要硬件。所以把根密钥抽象成 provider：
 *
 * ⚠️ 下面四行里**只有前两个真的存在**，后三个是规划，一行代码都还没有。
 *    别把这张表读成"四个 provider 都能选"。
 *
 *   【已实现】
 *     stub       —— 源码里一段固定的假根密钥（src/crypto/kdr.c）；
 *                   hardware_backed=0, device_bound=0；PRODUCTION 下整段编译掉
 *     device-dna —— 根 = KDF(设备 DNA)（src/crypto/kdr_dna.c）；
 *                   hardware_backed=0, device_bound=1（防克隆，非机密）
 *
 *   【规划中，尚无实现】—— 无函数声明、无 .c 文件，全仓 grep 零命中
 *     bbram      —— BBRAM AES-256 密钥（注：这块 AXU3EGB 的 VCC_BATT 无电池）
 *     efuse      —— eFUSE FUSE_AES（⚠️ 烧录不可逆，落在本项目红线内）
 *     puf        —— Zynq UltraScale+ 的 PUF 生成设备唯一 KEK（量产首选）
 *
 *    这三个要落地，不止是"烧片"：还要接上 pqchsmd 的 provider 安装（现在它
 *    一个都不装）、把 p11_module 里写死的 strcmp("device-dna") 换成注册表、
 *    以及一条换根后的 keystore 迁移路径。详见 docs/reference/HSM-COMPARISON.md §4.3。
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

	/* 根密钥是否**绑定到这颗芯片**（换一块板就派生不出同样的子密钥）。
	 *
	 * ⚠️ 这两个字段是**独立**的，别当成一个。设备 DNA provider 是
	 *    device_bound=1 而 hardware_backed=0：DNA 跨板不同，所以 keystore
	 *    拷到别的板上打不开（防克隆成立）；但 DNA **不是秘密** —— 有 JTAG
	 *    的人直接就能读出来，所以它不提供任何机密性。
	 *
	 *    把这两件事合成一个 flag 就是一种夸大：
	 *    "绑定到硬件" 会被读成 "受硬件保护"。 */
	int device_bound;
} pqc_kdr_provider_t;

/* 桩。⚠️ hardware_backed == 0 且 device_bound == 0。 */
const pqc_kdr_provider_t *pqc_kdr_provider_stub(void);

/* 设备 DNA provider（仅 Linux + 板上有 /dev/secmmio 时可用）。
 * 根 = KDF(设备 DNA)，DNA 经 EL3 的只读白名单窗口读出（0xFFCA0050-5C）。
 * 拿不到 DNA 时返回 NULL —— **不回退到任何常量**，宁可没有也不要一个
 * 看起来绑定了、实际上人人相同的根。 */
const pqc_kdr_provider_t *pqc_kdr_provider_device_dna(void);

/* 尝试安装设备 DNA provider。成功 0，拿不到 DNA 返回 -1 且**不改动**当前
 * provider。
 *
 * ⚠️ **换 KDR 会让既有 keystore 打不开**（包裹密钥变了，表现为完整性错误，
 *    和被篡改长得一模一样）。所以调用方必须显式决定，不能顺手就切。 */
int pqc_kdr_install_device_dna(void);

/* 用**已经取到手的** DNA 安装同一个 provider。给"库跑在远端主机上"用 ——
 * 那边没有 /dev/secmmio，DNA 是经 OP_DEVICE_DNA 从 daemon 取回来的。
 * 只做本地那条路的话，这个功能在真实拓扑（PKCS#11 在主机、密码机在板上）
 * 里等于不存在。成功 0；dna 为全 0 或全 F 视为"没读到"，返回 -1。 */
int pqc_kdr_install_device_dna_raw(const uint8_t *dna, size_t len);

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
