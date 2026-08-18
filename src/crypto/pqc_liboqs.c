/* pqc_liboqs.c —— pqc_backend_t 的 liboqs 0.16 实现
 *
 * 这是当前唯一的后端。将来 src/hal/pqc_accel.c 会实现同一张 vtable，
 * 通过 AXI-Lite 寄存器驱动 PL 里的算法核，上层代码不改一行。
 *
 * ⚠️ **凡是会让 liboqs 调 randombytes 的调用，都必须包在 RNG_GUARD 里。**
 *    liboqs 的随机源是进程级全局状态，而本模块又用它做确定性脚本（KAT 与
 *    种子存储）。少包一个，两条路就会互相吃对方的字节 —— 症状见 oqs_rng.c
 *    的文件头。新增后端方法时先问一句"它会不会取随机数"，会就包上。
 */
#include "pqchsm/pqc.h"
#include "pqchsm/util.h"
#include "oqs_rng.h"

#include <oqs/oqs.h>
#include <stdlib.h>
#include <string.h>

/* 随机源临界区。递归锁，所以与 pqc_oqs_rng_begin()/end() 嵌套是安全的。 */
#define RNG_GUARD_ENTER() pqc_oqs_rng_lock()
#define RNG_GUARD_LEAVE() pqc_oqs_rng_unlock()

static const char *oqs_kem_name(pqc_alg_t a)
{
	switch (a) {
	case PQC_ALG_ML_KEM_512:  return OQS_KEM_alg_ml_kem_512;
	case PQC_ALG_ML_KEM_768:  return OQS_KEM_alg_ml_kem_768;
	case PQC_ALG_ML_KEM_1024: return OQS_KEM_alg_ml_kem_1024;
	default:                  return NULL;
	}
}

static const char *oqs_sig_name(pqc_alg_t a)
{
	switch (a) {
	case PQC_ALG_ML_DSA_44: return OQS_SIG_alg_ml_dsa_44;
	case PQC_ALG_ML_DSA_65: return OQS_SIG_alg_ml_dsa_65;
	case PQC_ALG_ML_DSA_87: return OQS_SIG_alg_ml_dsa_87;
	default:                return NULL;
	}
}

static pqc_status_t map(OQS_STATUS s)
{
	return s == OQS_SUCCESS ? PQC_OK : PQC_ERR_BACKEND;
}

/* ---- KEM ---------------------------------------------------------------- */

static pqc_status_t be_keypair(pqc_alg_t alg, uint8_t *pk, uint8_t *sk)
{
	const char *n;
	if ((n = oqs_kem_name(alg)) != NULL) {
		OQS_KEM *k = OQS_KEM_new(n);
		if (!k) {
			return PQC_ERR_UNSUPPORTED;
		}
		RNG_GUARD_ENTER();
		pqc_status_t st = map(OQS_KEM_keypair(k, pk, sk));
		RNG_GUARD_LEAVE();
		OQS_KEM_free(k);
		return st;
	}
	if ((n = oqs_sig_name(alg)) != NULL) {
		OQS_SIG *s = OQS_SIG_new(n);
		if (!s) {
			return PQC_ERR_UNSUPPORTED;
		}
		RNG_GUARD_ENTER();
		pqc_status_t st = map(OQS_SIG_keypair(s, pk, sk));
		RNG_GUARD_LEAVE();
		OQS_SIG_free(s);
		return st;
	}
	return PQC_ERR_BAD_ARG;
}

static pqc_status_t be_keypair_from_seed(pqc_alg_t alg,
                                         const uint8_t *seed, size_t seed_len,
                                         uint8_t *pk, uint8_t *sk)
{
	const char *n;

	/* ML-KEM：liboqs 有原生去随机化 API，seed = d‖z (64 B) */
	if ((n = oqs_kem_name(alg)) != NULL) {
		if (seed_len != 64) {
			return PQC_ERR_BAD_ARG;
		}
		OQS_KEM *k = OQS_KEM_new(n);
		if (!k) {
			return PQC_ERR_UNSUPPORTED;
		}
		pqc_status_t st = k->keypair_derand
		                  ? map(OQS_KEM_keypair_derand(k, pk, sk, seed))
		                  : PQC_ERR_UNSUPPORTED;
		OQS_KEM_free(k);
		return st;
	}

	/* ML-DSA：无去随机化 API，改用脚本化随机源喂入 ξ（32 B） */
	if ((n = oqs_sig_name(alg)) != NULL) {
		if (seed_len != 32) {
			return PQC_ERR_BAD_ARG;
		}
		OQS_SIG *s = OQS_SIG_new(n);
		if (!s) {
			return PQC_ERR_UNSUPPORTED;
		}
		if (pqc_oqs_rng_begin(seed, seed_len) != 0) {
			OQS_SIG_free(s);
			return PQC_ERR_BACKEND;
		}
		pqc_status_t st = map(OQS_SIG_keypair(s, pk, sk));
		size_t consumed = 0;
		if (pqc_oqs_rng_end(&consumed) != 0 || consumed != seed_len) {
			st = PQC_ERR_BACKEND;   /* 消费模型不符，绝不返回可疑密钥 */
		}
		OQS_SIG_free(s);
		return st;
	}
	return PQC_ERR_BAD_ARG;
}

static pqc_status_t be_encaps(pqc_alg_t alg, const uint8_t *pk, uint8_t *ct, uint8_t *ss)
{
	const char *n = oqs_kem_name(alg);
	if (!n) {
		return PQC_ERR_BAD_ARG;
	}
	OQS_KEM *k = OQS_KEM_new(n);
	if (!k) {
		return PQC_ERR_UNSUPPORTED;
	}
	RNG_GUARD_ENTER();
	pqc_status_t st = map(OQS_KEM_encaps(k, ct, ss, pk));
	RNG_GUARD_LEAVE();
	OQS_KEM_free(k);
	return st;
}

static pqc_status_t be_encaps_derand(pqc_alg_t alg, const uint8_t *pk,
                                     const uint8_t *m, size_t m_len,
                                     uint8_t *ct, uint8_t *ss)
{
	const char *n = oqs_kem_name(alg);
	if (!n || m_len != 32) {
		return PQC_ERR_BAD_ARG;
	}
	OQS_KEM *k = OQS_KEM_new(n);
	if (!k) {
		return PQC_ERR_UNSUPPORTED;
	}
	pqc_status_t st = k->encaps_derand
	                  ? map(OQS_KEM_encaps_derand(k, ct, ss, pk, m))
	                  : PQC_ERR_UNSUPPORTED;
	OQS_KEM_free(k);
	return st;
}

static pqc_status_t be_decaps(pqc_alg_t alg, const uint8_t *sk,
                              const uint8_t *ct, uint8_t *ss)
{
	const char *n = oqs_kem_name(alg);
	if (!n) {
		return PQC_ERR_BAD_ARG;
	}
	OQS_KEM *k = OQS_KEM_new(n);
	if (!k) {
		return PQC_ERR_UNSUPPORTED;
	}
	pqc_status_t st = map(OQS_KEM_decaps(k, ss, ct, sk));
	OQS_KEM_free(k);
	return st;
}

/* ---- SIG ---------------------------------------------------------------- */

static pqc_status_t be_sign(pqc_alg_t alg, const uint8_t *sk,
                            const uint8_t *msg, size_t msg_len,
                            const uint8_t *ctx, size_t ctx_len,
                            const uint8_t *rnd,
                            uint8_t *sig, size_t *sig_len)
{
	const char *n = oqs_sig_name(alg);
	if (!n) {
		return PQC_ERR_BAD_ARG;
	}
	OQS_SIG *s = OQS_SIG_new(n);
	if (!s) {
		return PQC_ERR_UNSUPPORTED;
	}
	if (!s->sign_with_ctx_str) {
		OQS_SIG_free(s);
		return PQC_ERR_UNSUPPORTED;
	}

	pqc_status_t st;
	if (rnd) {
		/* FIPS 204 §Algorithm 2 的 rnd：全 0 = deterministic，随机 = hedged。
		 * liboqs 内部从 randombytes 取这 32 B，这里脚本化喂入。 */
		if (pqc_oqs_rng_begin(rnd, PQC_SIG_RND_LEN) != 0) {
			OQS_SIG_free(s);
			return PQC_ERR_BACKEND;
		}
		st = map(OQS_SIG_sign_with_ctx_str(s, sig, sig_len, msg, msg_len,
		                                   ctx, ctx_len, sk));
		size_t consumed = 0;
		if (pqc_oqs_rng_end(&consumed) != 0) {
			st = PQC_ERR_BACKEND;
		} else if (st == PQC_OK && consumed != PQC_SIG_RND_LEN) {
			/* 后端没有按预期消费 rnd（例如被编译成确定性签名），
			 * 此时签名值与调用方要求的 rnd 无关 —— 报不支持而非静默不一致。 */
			st = PQC_ERR_UNSUPPORTED;
		}
	} else {
		/* 不给 rnd 就是 hedged 签名：liboqs 会自己去取那 32 B 随机数，
		 * 所以这条路一样要进临界区。 */
		RNG_GUARD_ENTER();
		st = map(OQS_SIG_sign_with_ctx_str(s, sig, sig_len, msg, msg_len,
		                                   ctx, ctx_len, sk));
		RNG_GUARD_LEAVE();
	}
	OQS_SIG_free(s);
	return st;
}

static pqc_status_t be_verify(pqc_alg_t alg, const uint8_t *pk,
                              const uint8_t *msg, size_t msg_len,
                              const uint8_t *ctx, size_t ctx_len,
                              const uint8_t *sig, size_t sig_len)
{
	const char *n = oqs_sig_name(alg);
	if (!n) {
		return PQC_ERR_BAD_ARG;
	}
	OQS_SIG *s = OQS_SIG_new(n);
	if (!s) {
		return PQC_ERR_UNSUPPORTED;
	}
	if (!s->verify_with_ctx_str) {
		OQS_SIG_free(s);
		return PQC_ERR_UNSUPPORTED;
	}
	OQS_STATUS r = OQS_SIG_verify_with_ctx_str(s, msg, msg_len, sig, sig_len,
	                                           ctx, ctx_len, pk);
	OQS_SIG_free(s);
	/* 验签不过是"结果"不是"故障"，区分开，上层才能正确落审计 */
	return r == OQS_SUCCESS ? PQC_OK : PQC_ERR_VERIFY;
}

static const pqc_backend_t g_liboqs = {
	.name              = "liboqs-0.16",
	.keypair           = be_keypair,
	.keypair_from_seed = be_keypair_from_seed,
	.encaps            = be_encaps,
	.encaps_derand     = be_encaps_derand,
	.decaps            = be_decaps,
	.sign              = be_sign,
	.verify            = be_verify,
};

const pqc_backend_t *pqc_backend_liboqs(void)
{
	pqc_oqs_rng_init();
	return &g_liboqs;
}
