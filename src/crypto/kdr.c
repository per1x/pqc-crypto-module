#include "pqchsm/kdr.h"
#include "pqchsm/kdf.h"
#include "pqchsm/profile.h"
#include "pqchsm/util.h"

#include <string.h>

/* ============================================================================
 * 【桩根密钥：只在 DEV 形态下存在】
 * ============================================================================
 * "固件二进制里搜不到 KDR" 是验收项之一。老版本把这条写在注释里当作将来的
 * 待办，于是**默认构建出来的二进制里就躺着一个公开的根密钥**，而且没有
 * provider 时会自动回退到它 —— 谁都不会发现。
 *
 * 现在由 PQC_PROFILE 分开（见 pqchsm/profile.h）：
 *   DEV        —— 编译进来，且启动时会打一句明确的告警；
 *   PRODUCTION —— 整段 #if 掉，**二进制里搜不到这 32 字节**，
 *                 pqc_kdr_provider_stub() 返回 NULL，没装 provider 就
 *                 派生不出任何东西（fail-closed）。
 * tools/check_profile.sh 在 ctest 里对着 PRODUCTION 的目标文件扫这段字面量，
 * 保证这条不是靠人记住。
 */
#if PQC_PROFILE != PQC_PROFILE_PRODUCTION

/* ⚠️ 桩根密钥。真实设备上这 32 字节来自 eFUSE/BBRAM/PUF，永不出芯片。
 * 在当前阶段它保护的只是测试数据。
 * 字面量本身就写着 NOT SECRET，避免有人误以为它是真的。 */
static const uint8_t KDR_STUB[PQC_KDR_LEN] = {
	0x50, 0x51, 0x43, 0x2d, 0x48, 0x53, 0x4d, 0x20,   /* "PQC-HSM " */
	0x53, 0x54, 0x55, 0x42, 0x20, 0x4b, 0x44, 0x52,   /* "STUB KDR" */
	0x20, 0x2d, 0x2d, 0x20, 0x4e, 0x4f, 0x54, 0x20,   /* " -- NOT " */
	0x53, 0x45, 0x43, 0x52, 0x45, 0x54, 0x21, 0x21,   /* "SECRET!!" */
};

/* 桩 provider 的根密钥。static 且**没有任何 getter** —— 见 kdr.h 的铁律。 */
static uint8_t g_root[PQC_KDR_LEN];
static int     g_root_ready;

static void ensure_root(void)
{
	if (!g_root_ready) {
		memcpy(g_root, KDR_STUB, PQC_KDR_LEN);
		g_root_ready = 1;
	}
}

static int stub_derive(const char *label, const uint8_t *salt, size_t salt_len,
                       uint8_t *out, size_t out_len)
{
	ensure_root();
	return pqc_kdf(g_root, PQC_KDR_LEN, salt, salt_len, label, out, out_len);
}

static const pqc_kdr_provider_t g_stub = {
	.name            = "stub(software, NOT hardware-backed)",
	.derive          = stub_derive,
	.hardware_backed = 0,
	.device_bound    = 0,   /* 编译进去的常量 —— 换块板还是同一个根 */
};

const pqc_kdr_provider_t *pqc_kdr_provider_stub(void)
{
	return &g_stub;
}

#else   /* PQC_PROFILE == PQC_PROFILE_PRODUCTION */

/* 生产形态：桩**根本不存在**。返回 NULL 而不是"一个不会派生成功的桩" ——
 * 前者让调用方在编译期/启动期就撞上，后者会一路跑到第一次派生才失败。 */
const pqc_kdr_provider_t *pqc_kdr_provider_stub(void)
{
	return NULL;
}

#endif  /* PQC_PROFILE */

static const pqc_kdr_provider_t *g_provider;

void pqc_kdr_set_provider(const pqc_kdr_provider_t *p)
{
	g_provider = p;
}

/* ============================================================================
 * 【没有任何自动回退（PS-04）】
 * ============================================================================
 * 这里以前在 DEV 形态下有一句 `if (!g_provider) g_provider = &g_stub;` ——
 * 也就是"谁都没装 provider 的话，就悄悄用那个编译进去的公开常量当信任根"。
 *
 * 它的问题不是 DEV 下用桩（那本来就允许），而是**这件事没有任何人做过决定**：
 *   · 一个忘了装 provider 的**正式**入口，行为与"故意用桩"完全一样；
 *   · 症状是零 —— keystore 照常开、密钥照常生成、日志里也只有那句
 *     DEV 通用告警，而它在真的装了桩时也一样会打。
 *   · 于是"这台机器的信任根是什么"这个问题，答案取决于有没有人记得。
 *
 * 现在：**没装就是 NULL**，两种形态都一样。想用桩就显式调
 * pqc_kdr_install_stub()（演示、单元测试、KAT 都该这么写），
 * 那一行本身就是"我知道这是个公开常量"的书面记录。
 * 入口拿不到 provider 时按各自的 fail-closed 纪律拒绝启动。
 */
const pqc_kdr_provider_t *pqc_kdr_get_provider(void)
{
	return g_provider;
}

int pqc_kdr_install_stub(void)
{
#if PQC_PROFILE != PQC_PROFILE_PRODUCTION
	g_provider = &g_stub;
	return 0;
#else
	/* 生产形态里这段常量根本没编进来，装不了 —— 返回失败，让调用方按
	 * 自己的 fail-closed 纪律处理，而不是给它一个派生不出东西的壳。 */
	return -1;
#endif
}

int pqc_kdr_is_hardware_backed(void)
{
	const pqc_kdr_provider_t *p = pqc_kdr_get_provider();
	return p && p->hardware_backed;
}

int pqc_kdr_derive(const char *label, const uint8_t *salt, size_t salt_len,
                   uint8_t *out, size_t out_len)
{
	/* label 非空是硬性要求：所有子密钥必须域分隔，
	 * 否则"存储 KEK"和"元数据密钥"在同一个盐下会派生出同一个值。 */
	if (!label || label[0] == '\0' || !out || out_len == 0) {
		return -1;
	}
	const pqc_kdr_provider_t *p = pqc_kdr_get_provider();
	if (!p || !p->derive) {
		return -1;
	}
	return p->derive(label, salt, salt_len, out, out_len);
}

void pqc_kdr_set_test_root(const uint8_t *seed, size_t seed_len)
{
#if PQC_PROFILE == PQC_PROFILE_PRODUCTION
	/* 生产形态里没有桩，也就没有"换个测试根"这回事 */
	(void)seed;
	(void)seed_len;
	return;
#else
	/* 只对桩 provider 有意义 —— 硬件 provider 的根密钥改不了，这正是重点 */
	if (pqc_kdr_get_provider() != &g_stub) {
		return;
	}
	if (!seed || seed_len == 0) {
		memcpy(g_root, KDR_STUB, PQC_KDR_LEN);
		g_root_ready = 1;
		return;
	}
	/* 把任意长度的测试种子压成 32 字节，保证不同种子 = 不同"设备" */
	uint8_t root[PQC_KDR_LEN];
	if (pqc_kdf(seed, seed_len, NULL, 0, "pqc-hsm/test-device-root", root, sizeof(root)) != 0) {
		/* 失败也可能已经写进去半截，照样要抹掉 */
		pqc_secure_zero(root, sizeof(root));
		return;
	}
	memcpy(g_root, root, PQC_KDR_LEN);
	g_root_ready = 1;
	pqc_secure_zero(root, sizeof(root));
#endif
}
