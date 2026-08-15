// hwrng_sdfe —— 经密码机服务取硬件熵源的字节源
//
// ============================================================================
// 【为什么不是一个寄存器级 transport】
// ============================================================================
// hwrng.h 里那个 hwrng_transport_t 是寄存器级的（write_reg/read_reg），
// 前提是调用方够得到 trng_axi 的寄存器。**在交付形态里这个前提结构上不成立**：
// 四个密码核都按 SECURE_ONLY=1 综合，普通世界发出的每一笔访问都被 PL 的
// 防火墙拒掉（读回 0）。所以那种 transport 在用户态写不出来 —— 不是没写。
//
// 通向硬件熵源的唯一一条路是：
//   本文件 → libsdfe → pqchsm_fpgad → /dev/secmmio → EL3 的 SiP → trng_axi
// 而这条路的接口是**字节**，不是寄存器。daemon 不代理任意寄存器读写，
// 那等于在密码边界上开一个后门。
//
// ============================================================================
// 【取不到就失败，绝不回退】
// ============================================================================
// 连不上服务、服务报硬件失败，一律返回错误，让上层决定停机还是降级。
// 静默回退到 OpenSSL 会让"熵来自硬件"这句话**恰好在最要紧的时刻**
// 悄悄变成假话，而调用方无从知道 —— 这与 src/util/util.c 里
// pqc_random_bytes 的既有纪律是同一条。
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pqchsm/hwrng.h"
#include "sdfe.h"

/* 一个进程一个会话：连接是有状态的（远程还要认证），每次取随机数都重连
 * 会把 TCP 握手 + 认证摊到每一次调用上。失败之后置空重连，
 * 不缓存一个已经坏掉的句柄。 */
static SDFE_HANDLE g_dev, g_ses;

static void close_dev(void)
{
	if (g_ses) { SDFE_CloseSession(g_ses); g_ses = NULL; }
	if (g_dev) { SDFE_CloseDevice(g_dev);  g_dev = NULL; }
}

static int ensure_open(void)
{
	const char *host = getenv("PQCHSM_SDFE_HOST");
	const char *tok  = getenv("PQCHSM_SDFE_TOKEN");
	const char *ps   = getenv("PQCHSM_SDFE_PORT");
	int rv;

	if (g_ses)
		return 0;

	if (host && tok)
		rv = SDFE_OpenDeviceRemote(&g_dev, host, ps ? atoi(ps) : 0, tok);
	else
		rv = SDFE_OpenDevice(&g_dev);
	if (rv != SDR_OK) {
		g_dev = NULL;
		return -1;
	}
	if (SDFE_OpenSession(g_dev, &g_ses) != SDR_OK) {
		close_dev();
		return -1;
	}
	return 0;
}

static int sdfe_bytes(uint8_t *out, size_t n)
{
	size_t done = 0;

	if (!out || n == 0)
		return -1;
	if (ensure_open())
		return -1;

	/* 线协议一次最多 8192 字节载荷，长请求要分段。
	 * 分段边界不影响熵：每一段都是同一个环振源的新鲜输出。 */
	while (done < n) {
		uint32_t chunk = (uint32_t)((n - done) > 4096 ? 4096 : (n - done));

		if (SDFE_GenerateRandom(g_ses, chunk, out + done) != SDR_OK) {
			/* 连接可能已经坏了（服务重启、网络断）。关掉重来，
			 * 下一次调用会重连；但**本次仍然报失败** ——
			 * 悄悄重试会把一次真实的硬件故障掩盖成偶发延迟。 */
			close_dev();
			return -1;
		}
		done += chunk;
	}
	return 0;
}

static const hwrng_byte_source_t g_src = {
	.name = "sdfe(pqchsm_fpgad → EL3 → trng_axi)",
	.is_hardware = 1,
	.bytes = sdfe_bytes,
};

const hwrng_byte_source_t *hwrng_byte_source_sdfe(void)
{
	return &g_src;
}
