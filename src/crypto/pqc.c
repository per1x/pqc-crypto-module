/* pqc.c —— 算法元数据表 + 后端分发与参数校验
 *
 * 每个入口都先过一次上电自测闸门：自测未通过时模块处于错误状态，一律拒绝服务。
 * 这是 FIPS 140-3 与 GM/T 0028 对"错误状态下不得输出密码运算结果"的要求，
 * 闸门放在分发层是因为所有对外的密码运算都从这里过。
 */
#include "pqchsm/pqc.h"
#include "pqchsm/selftest.h"

#include <string.h>

/* 长度取自 FIPS 203/204，并在 test_pqc_meta 中与 liboqs 的运行时值逐项断言，
 * 防止这张表与后端脱节。 */
static const pqc_alg_info_t g_algs[] = {
	/* alg, kind, name, pk, sk, ct, ss, sig, seed */
	{ PQC_ALG_ML_KEM_512,  PQC_KIND_KEM, "ML-KEM-512",   800, 1632,  768, 32,    0, 64 },
	{ PQC_ALG_ML_KEM_768,  PQC_KIND_KEM, "ML-KEM-768",  1184, 2400, 1088, 32,    0, 64 },
	{ PQC_ALG_ML_KEM_1024, PQC_KIND_KEM, "ML-KEM-1024", 1568, 3168, 1568, 32,    0, 64 },
	{ PQC_ALG_ML_DSA_44,   PQC_KIND_SIG, "ML-DSA-44",   1312, 2560,    0,  0, 2420, 32 },
	{ PQC_ALG_ML_DSA_65,   PQC_KIND_SIG, "ML-DSA-65",   1952, 4032,    0,  0, 3309, 32 },
	{ PQC_ALG_ML_DSA_87,   PQC_KIND_SIG, "ML-DSA-87",   2592, 4896,    0,  0, 4627, 32 },
};

const pqc_alg_info_t *pqc_alg_info(pqc_alg_t alg)
{
	for (size_t i = 0; i < sizeof(g_algs) / sizeof(g_algs[0]); i++) {
		if (g_algs[i].alg == alg) {
			return &g_algs[i];
		}
	}
	return NULL;
}

pqc_alg_t pqc_alg_by_name(const char *name)
{
	if (!name) {
		return PQC_ALG_NONE;
	}
	for (size_t i = 0; i < sizeof(g_algs) / sizeof(g_algs[0]); i++) {
		if (strcmp(g_algs[i].name, name) == 0) {
			return g_algs[i].alg;
		}
	}
	return PQC_ALG_NONE;
}

int pqc_backend_has_hw_keys(void)
{
	const pqc_backend_t *be = pqc_get_backend();

	/* 两个都在才算。只有一个的后端是没法用的：能生成却解不开，
	 * 或者能解却生不出来 —— 与其让上层在半路发现，不如这里就说没有。 */
	return be && be->keypair_hw && be->decaps_hw;
}

pqc_status_t pqc_keypair_hw(pqc_alg_t alg, uint8_t *pk, uint32_t *hw_handle)
{
	const pqc_backend_t *be = pqc_get_backend();

	if (!pk || !hw_handle)
		return PQC_ERR_BAD_ARG;
	if (!be || !be->keypair_hw)
		return PQC_ERR_UNSUPPORTED;
	return be->keypair_hw(alg, pk, hw_handle);
}

pqc_status_t pqc_decaps_hw(pqc_alg_t alg, uint32_t hw_handle,
                           const uint8_t *ct, uint8_t *ss)
{
	const pqc_backend_t *be = pqc_get_backend();

	if (!ct || !ss)
		return PQC_ERR_BAD_ARG;
	if (!be || !be->decaps_hw)
		return PQC_ERR_UNSUPPORTED;
	return be->decaps_hw(alg, hw_handle, ct, ss);
}

const char *pqc_strerror(pqc_status_t st)
{
	switch (st) {
	case PQC_OK:              return "ok";
	case PQC_ERR_BAD_ARG:     return "bad argument";
	case PQC_ERR_UNSUPPORTED: return "unsupported by backend";
	case PQC_ERR_BACKEND:     return "backend failure";
	case PQC_ERR_VERIFY:      return "verification failed";
	case PQC_ERR_SELF_TEST:   return "module in error state: self-test failed";
	}
	return "unknown";
}

static const pqc_backend_t *g_backend;

void pqc_set_backend(const pqc_backend_t *be)
{
	g_backend = be;
}

const pqc_backend_t *pqc_get_backend(void)
{
	if (!g_backend) {
		g_backend = pqc_backend_liboqs();
	}
	return g_backend;
}

/* 统一的前置校验：模块可用、算法已知、种类匹配、后端实现了该方法 */
#define PROLOGUE(want_kind, method)                                    \
	const pqc_alg_info_t *info = pqc_alg_info(alg);                \
	const pqc_backend_t  *be   = pqc_get_backend();                \
	if (!pqc_self_test_passed()) return PQC_ERR_SELF_TEST;         \
	if (!info || info->kind != (want_kind)) return PQC_ERR_BAD_ARG; \
	if (!be || !be->method) return PQC_ERR_UNSUPPORTED

pqc_status_t pqc_keypair(pqc_alg_t alg, uint8_t *pk, uint8_t *sk)
{
	const pqc_alg_info_t *info = pqc_alg_info(alg);
	const pqc_backend_t  *be   = pqc_get_backend();
	if (!pqc_self_test_passed()) {
		return PQC_ERR_SELF_TEST;
	}
	if (!info || !pk || !sk) {
		return PQC_ERR_BAD_ARG;
	}
	if (!be || !be->keypair) {
		return PQC_ERR_UNSUPPORTED;
	}
	return be->keypair(alg, pk, sk);
}

pqc_status_t pqc_keypair_from_seed(pqc_alg_t alg, const uint8_t *seed, size_t seed_len,
                                   uint8_t *pk, uint8_t *sk)
{
	const pqc_alg_info_t *info = pqc_alg_info(alg);
	const pqc_backend_t  *be   = pqc_get_backend();
	if (!pqc_self_test_passed()) {
		return PQC_ERR_SELF_TEST;
	}
	if (!info || !seed || !pk || !sk) {
		return PQC_ERR_BAD_ARG;
	}
	if (seed_len != info->seed_len) {
		return PQC_ERR_BAD_ARG;
	}
	if (!be || !be->keypair_from_seed) {
		return PQC_ERR_UNSUPPORTED;
	}
	return be->keypair_from_seed(alg, seed, seed_len, pk, sk);
}

pqc_status_t pqc_encaps(pqc_alg_t alg, const uint8_t *pk, uint8_t *ct, uint8_t *ss)
{
	PROLOGUE(PQC_KIND_KEM, encaps);
	if (!pk || !ct || !ss) {
		return PQC_ERR_BAD_ARG;
	}
	return be->encaps(alg, pk, ct, ss);
}

pqc_status_t pqc_encaps_derand(pqc_alg_t alg, const uint8_t *pk,
                               const uint8_t *m, size_t m_len,
                               uint8_t *ct, uint8_t *ss)
{
	PROLOGUE(PQC_KIND_KEM, encaps_derand);
	if (!pk || !m || !ct || !ss || m_len != 32) {
		return PQC_ERR_BAD_ARG;
	}
	return be->encaps_derand(alg, pk, m, m_len, ct, ss);
}

pqc_status_t pqc_decaps(pqc_alg_t alg, const uint8_t *sk, const uint8_t *ct, uint8_t *ss)
{
	PROLOGUE(PQC_KIND_KEM, decaps);
	if (!sk || !ct || !ss) {
		return PQC_ERR_BAD_ARG;
	}
	return be->decaps(alg, sk, ct, ss);
}

pqc_status_t pqc_sign(pqc_alg_t alg, const uint8_t *sk,
                      const uint8_t *msg, size_t msg_len,
                      const uint8_t *ctx, size_t ctx_len,
                      const uint8_t *rnd, uint8_t *sig, size_t *sig_len)
{
	PROLOGUE(PQC_KIND_SIG, sign);
	if (!sk || !sig || !sig_len) {
		return PQC_ERR_BAD_ARG;
	}
	if ((!msg && msg_len) || (!ctx && ctx_len) || ctx_len > 255) {
		return PQC_ERR_BAD_ARG;
	}
	return be->sign(alg, sk, msg, msg_len, ctx, ctx_len, rnd, sig, sig_len);
}

pqc_status_t pqc_verify(pqc_alg_t alg, const uint8_t *pk,
                        const uint8_t *msg, size_t msg_len,
                        const uint8_t *ctx, size_t ctx_len,
                        const uint8_t *sig, size_t sig_len)
{
	PROLOGUE(PQC_KIND_SIG, verify);
	if (!pk || !sig) {
		return PQC_ERR_BAD_ARG;
	}
	if ((!msg && msg_len) || (!ctx && ctx_len) || ctx_len > 255) {
		return PQC_ERR_BAD_ARG;
	}
	return be->verify(alg, pk, msg, msg_len, ctx, ctx_len, sig, sig_len);
}
