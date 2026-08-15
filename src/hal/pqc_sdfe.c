// pqc_sdfe —— 经密码机服务打 FPGA 的 pqc_backend_t 实现
//
// ============================================================================
// 【它和 pqc_accel.c 的区别】
// ============================================================================
// pqc_accel.c 也是一个硬件后端，但它打的是 pqc_accel_axi（带操作码的
// AXI-Stream 加速器）—— 那个从机**不在板上的位流里**（它的批量数据要一路
// DMA，是独立的一块工作，见 zu3eg_hsm_top.v 的说明）。
//
// 板上真正跑着的 ML-KEM 是 mlkem_axi，而它对普通世界是关死的
// （SECURE_ONLY=1）。唯一的通路是 pqchsm_fpgad → /dev/secmmio → EL3 的 SiP。
// 本文件就是把 pqc_backend_t 架在那条通路上。
//
// ============================================================================
// 【为什么 keypair / decaps 这两个字节版故意不实现】
// ============================================================================
// 它们的签名要求私钥以字节形式进出（keypair 要填 sk，decaps 要收 sk）。
// 而 PL 里的 dk 留在片内金库、并且有一道一次性闩锁封死出口 —— 交付形态下
// 硬件**根本不会**把 dk 交出来。
//
// 这里有两种写法，差别很大：
//   ① 填 NULL，让上层看见"这个后端做不了字节版"，于是改用句柄版；
//   ② 实现成"内部走句柄、把 dk 伪造成一段字节"或者"关掉闩锁取出 dk"。
//
// 选 ①。写法 ② 能让现有测试全绿，而它恰好把这一整套改动的理由取消了：
// 上层再也分不清私钥到底在不在硬件里。**接口说不了谎比接口好用重要。**
//
// encaps 是例外：它只用公钥，字节版天然成立，所以照常实现。
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>

#include "pqchsm/pqc.h"
#include "sdfe.h"

static SDFE_HANDLE g_dev, g_ses;

static void closedev(void)
{
	if (g_ses) { SDFE_CloseSession(g_ses); g_ses = NULL; }
	if (g_dev) { SDFE_CloseDevice(g_dev);  g_dev = NULL; }
}

static int opendev(void)
{
	const char *host = getenv("PQCHSM_SDFE_HOST");
	const char *tok  = getenv("PQCHSM_SDFE_TOKEN");
	const char *ps   = getenv("PQCHSM_SDFE_PORT");
	int rv;

	if (g_ses)
		return 0;
	rv = (host && tok) ? SDFE_OpenDeviceRemote(&g_dev, host, ps ? atoi(ps) : 0, tok)
	                   : SDFE_OpenDevice(&g_dev);
	if (rv != SDR_OK) { g_dev = NULL; return -1; }
	if (SDFE_OpenSession(g_dev, &g_ses) != SDR_OK) { closedev(); return -1; }
	return 0;
}

/* pqc_alg_t → SDF 的参数集编号。只有 ML-KEM 三档；其余算法这个后端做不了，
 * 如实回 UNSUPPORTED，让上层去用软件后端 —— 不猜、不代打。 */
static int pset_of(pqc_alg_t alg)
{
	switch (alg) {
	case PQC_ALG_ML_KEM_512:  return SDFE_MLKEM_512;
	case PQC_ALG_ML_KEM_768:  return SDFE_MLKEM_768;
	case PQC_ALG_ML_KEM_1024: return SDFE_MLKEM_1024;
	default:                  return -1;
	}
}

static pqc_status_t sdfe_keypair_hw(pqc_alg_t alg, uint8_t *pk, uint32_t *hw)
{
	uint8_t buf[1600];
	uint32_t len = sizeof buf, h = 0;
	int ps = pset_of(alg);

	if (ps < 0 || !pk || !hw)
		return PQC_ERR_UNSUPPORTED;
	if (opendev())
		return PQC_ERR_BACKEND;
	if (SDFE_GenerateKeyPair_MLKEM(g_ses, (uint32_t)ps, buf, &len, &h) != SDR_OK) {
		closedev();
		return PQC_ERR_BACKEND;
	}
	memcpy(pk, buf, len);
	*hw = h;
	return PQC_OK;
}

static pqc_status_t sdfe_decaps_hw(pqc_alg_t alg, uint32_t hw,
                                   const uint8_t *ct, uint8_t *ss)
{
	const pqc_alg_info_t *info = pqc_alg_info(alg);
	uint32_t ss_len = 32;

	if (pset_of(alg) < 0 || !info || !ct || !ss)
		return PQC_ERR_UNSUPPORTED;
	if (opendev())
		return PQC_ERR_BACKEND;
	if (SDFE_Decapsulate_MLKEM(g_ses, hw, ct, (uint32_t)info->ct_len,
	                           ss, &ss_len) != SDR_OK) {
		closedev();
		return PQC_ERR_BACKEND;
	}
	return ss_len == info->ss_len ? PQC_OK : PQC_ERR_BACKEND;
}

static pqc_status_t sdfe_encaps(pqc_alg_t alg, const uint8_t *pk,
                                uint8_t *ct, uint8_t *ss)
{
	const pqc_alg_info_t *info = pqc_alg_info(alg);
	uint32_t ct_len, ss_len = 32;
	int ps = pset_of(alg);

	if (ps < 0 || !info || !pk || !ct || !ss)
		return PQC_ERR_UNSUPPORTED;
	if (opendev())
		return PQC_ERR_BACKEND;
	ct_len = (uint32_t)info->ct_len;
	if (SDFE_Encapsulate_MLKEM(g_ses, (uint32_t)ps, pk, (uint32_t)info->pk_len,
	                           ss, &ss_len, ct, &ct_len) != SDR_OK) {
		closedev();
		return PQC_ERR_BACKEND;
	}
	return PQC_OK;
}

static const pqc_backend_t g_be = {
	.name = "sdfe(FPGA via pqchsm_fpgad)",
	/* 字节版私钥操作故意留 NULL —— 见文件头 */
	.keypair = NULL,
	.keypair_from_seed = NULL,
	.encaps = sdfe_encaps,
	.encaps_derand = NULL,   /* 硬件自取 m，给不了确定性版本；ACVP 走软件后端 */
	.decaps = NULL,
	.sign = NULL,
	.verify = NULL,
	.keypair_hw = sdfe_keypair_hw,
	.decaps_hw = sdfe_decaps_hw,
};

const pqc_backend_t *pqc_backend_sdfe(void)
{
	return &g_be;
}
