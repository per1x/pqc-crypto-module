#include "pqchsm/kdr.h"
#include "pqchsm/kdf.h"
#include "pqchsm/util.h"

#include <string.h>

/* ⚠️ 桩根密钥。真实设备上这 32 字节来自 eFUSE/BBRAM/PUF，永不出芯片。
 * 在当前阶段它保护的只是测试数据；进入真实设备后，"固件二进制里搜不到 KDR"
 * 是验收项之一，届时这段常量必须消失。
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
};

const pqc_kdr_provider_t *pqc_kdr_provider_stub(void)
{
	return &g_stub;
}

static const pqc_kdr_provider_t *g_provider;

void pqc_kdr_set_provider(const pqc_kdr_provider_t *p)
{
	g_provider = p;
}

const pqc_kdr_provider_t *pqc_kdr_get_provider(void)
{
	if (!g_provider) {
		g_provider = &g_stub;
	}
	return g_provider;
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
		return;
	}
	memcpy(g_root, root, PQC_KDR_LEN);
	g_root_ready = 1;
	pqc_secure_zero(root, sizeof(root));
}
