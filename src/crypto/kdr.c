#include "pqchsm/kdr.h"
#include "pqchsm/kdf.h"
#include "pqchsm/util.h"

#include <string.h>

/* ⚠️ 桩根密钥。真实设备上这 32 字节来自 eFUSE/BBRAM/PUF，永不出芯片。
 * 之所以敢把它写死在源码里：Phase 5 阶段它保护的是测试数据，
 * 而 Phase 7 的验收项之一就是"固件二进制里搜不到 KDR" —— 到那时这段必须消失。 */
static const uint8_t KDR_STUB[PQC_KDR_LEN] = {
	0x50, 0x51, 0x43, 0x2d, 0x48, 0x53, 0x4d, 0x20,   /* "PQC-HSM " */
	0x53, 0x54, 0x55, 0x42, 0x20, 0x4b, 0x44, 0x52,   /* "STUB KDR" */
	0x20, 0x2d, 0x2d, 0x20, 0x4e, 0x4f, 0x54, 0x20,   /* " -- NOT " */
	0x53, 0x45, 0x43, 0x52, 0x45, 0x54, 0x21, 0x21,   /* "SECRET!!" */
};

static uint8_t g_kdr[PQC_KDR_LEN];
static int     g_kdr_ready;

static void ensure_root(void)
{
	if (!g_kdr_ready) {
		memcpy(g_kdr, KDR_STUB, PQC_KDR_LEN);
		g_kdr_ready = 1;
	}
}

int pqc_kdr_derive(const char *label, const uint8_t *salt, size_t salt_len,
                   uint8_t *out, size_t out_len)
{
	if (!label || label[0] == '\0' || !out || out_len == 0) {
		return -1;
	}
	ensure_root();
	return pqc_kdf(g_kdr, PQC_KDR_LEN, salt, salt_len, label, out, out_len);
}

void pqc_kdr_set_test_root(const uint8_t *seed, size_t seed_len)
{
	if (!seed || seed_len == 0) {
		memcpy(g_kdr, KDR_STUB, PQC_KDR_LEN);
		g_kdr_ready = 1;
		return;
	}
	/* 把任意长度的测试种子压成 32 字节，保证不同种子 = 不同"设备" */
	uint8_t root[PQC_KDR_LEN];
	if (pqc_kdf(seed, seed_len, NULL, 0, "pqc-hsm/test-device-root", root, sizeof(root)) != 0) {
		return;
	}
	memcpy(g_kdr, root, PQC_KDR_LEN);
	g_kdr_ready = 1;
	pqc_secure_zero(root, sizeof(root));
}
