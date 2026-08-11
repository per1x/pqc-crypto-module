#include "ta_pqc.h"

#include "pqchsm_ta_proto.h"
#include "ta_pqc_low.h"

/* 尺寸表：FIPS 203/204 标称值（与 pqc.c 里的 pqc_alg_info 表一致） */
static const ta_pqc_dims_t dims[] = {
	/* alg                  kem  pk    sk    ct    ss  sig   seed */
	{ TA_ALG_ML_KEM_512,   1,   800, 1632,  768, 32,    0, 64 },
	{ TA_ALG_ML_KEM_768,   1,  1184, 2400, 1088, 32,    0, 64 },
	{ TA_ALG_ML_KEM_1024,  1,  1568, 3168, 1568, 32,    0, 64 },
	{ TA_ALG_ML_DSA_44,    0,  1312, 2560,    0,  0, 2420, 32 },
	{ TA_ALG_ML_DSA_65,    0,  1952, 4032,    0,  0, 3309, 32 },
	{ TA_ALG_ML_DSA_87,    0,  2592, 4896,    0,  0, 4627, 32 },
};

const ta_pqc_dims_t *ta_pqc_dims(uint32_t alg)
{
	size_t i;

	for (i = 0; i < sizeof(dims) / sizeof(dims[0]); i++)
		if (dims[i].alg == alg)
			return &dims[i];
	return NULL;
}

typedef struct {
	int (*keypair)(uint8_t *pk, uint8_t *sk);
	int (*keypair_derand)(uint8_t *pk, uint8_t *sk, const uint8_t coins[64]);
	int (*enc)(uint8_t *ct, uint8_t *ss, const uint8_t *pk);
	int (*dec)(uint8_t *ss, const uint8_t *ct, const uint8_t *sk);
} kem_ops_t;

typedef struct {
	int (*keypair)(uint8_t *pk, uint8_t *sk);
	int (*keypair_internal)(uint8_t *pk, uint8_t *sk, const uint8_t seed[32]);
	int (*sign)(uint8_t *sig, size_t *siglen, const uint8_t *m, size_t mlen,
	            const uint8_t *ctx, size_t ctxlen, const uint8_t *sk);
	int (*verify)(const uint8_t *sig, size_t siglen, const uint8_t *m,
	              size_t mlen, const uint8_t *ctx, size_t ctxlen,
	              const uint8_t *pk);
} sig_ops_t;

static const kem_ops_t *kem_ops(uint32_t alg)
{
	static const kem_ops_t ops512 = {
		pqchsm_mlk512_keypair, pqchsm_mlk512_keypair_derand,
		pqchsm_mlk512_enc, pqchsm_mlk512_dec,
	};
	static const kem_ops_t ops768 = {
		pqchsm_mlk768_keypair, pqchsm_mlk768_keypair_derand,
		pqchsm_mlk768_enc, pqchsm_mlk768_dec,
	};
	static const kem_ops_t ops1024 = {
		pqchsm_mlk1024_keypair, pqchsm_mlk1024_keypair_derand,
		pqchsm_mlk1024_enc, pqchsm_mlk1024_dec,
	};

	switch (alg) {
	case TA_ALG_ML_KEM_512:
		return &ops512;
	case TA_ALG_ML_KEM_768:
		return &ops768;
	case TA_ALG_ML_KEM_1024:
		return &ops1024;
	default:
		return NULL;
	}
}

static const sig_ops_t *sig_ops(uint32_t alg)
{
	static const sig_ops_t ops44 = {
		pqchsm_mld44_keypair, pqchsm_mld44_keypair_internal,
		pqchsm_mld44_sign, pqchsm_mld44_verify,
	};
	static const sig_ops_t ops65 = {
		pqchsm_mld65_keypair, pqchsm_mld65_keypair_internal,
		pqchsm_mld65_sign, pqchsm_mld65_verify,
	};
	static const sig_ops_t ops87 = {
		pqchsm_mld87_keypair, pqchsm_mld87_keypair_internal,
		pqchsm_mld87_sign, pqchsm_mld87_verify,
	};

	switch (alg) {
	case TA_ALG_ML_DSA_44:
		return &ops44;
	case TA_ALG_ML_DSA_65:
		return &ops65;
	case TA_ALG_ML_DSA_87:
		return &ops87;
	default:
		return NULL;
	}
}

int ta_pqc_keypair(uint32_t alg, uint8_t *pk, uint8_t *sk)
{
	const ta_pqc_dims_t *d = ta_pqc_dims(alg);
	int                 rc;

	if (!d || !pk || !sk)
		return -1;
	if (d->is_kem) {
		const kem_ops_t *op = kem_ops(alg);
		rc = op->keypair(pk, sk);
	} else {
		const sig_ops_t *op = sig_ops(alg);
		rc = op->keypair(pk, sk);
	}
	return rc == 0 ? 0 : -3;
}

int ta_pqc_keypair_from_seed(uint32_t alg, const uint8_t *seed,
                             size_t seed_len, uint8_t *pk, uint8_t *sk)
{
	const ta_pqc_dims_t *d = ta_pqc_dims(alg);
	int                 rc;

	if (!d || !seed || !pk || !sk)
		return -1;
	if (seed_len != d->seed_len)
		return -1;
	if (d->is_kem) {
		const kem_ops_t *op = kem_ops(alg);
		rc = op->keypair_derand(pk, sk, seed);
	} else {
		const sig_ops_t *op = sig_ops(alg);
		rc = op->keypair_internal(pk, sk, seed);
	}
	return rc == 0 ? 0 : -3;
}

int ta_pqc_encaps(uint32_t alg, const uint8_t *pk, uint8_t *ct, uint8_t *ss)
{
	const ta_pqc_dims_t *d  = ta_pqc_dims(alg);
	const kem_ops_t     *op = kem_ops(alg);

	if (!d || !d->is_kem || !op || !pk || !ct || !ss)
		return -1;
	return op->enc(ct, ss, pk) == 0 ? 0 : -3;
}

int ta_pqc_decaps(uint32_t alg, const uint8_t *sk, const uint8_t *ct,
                  uint8_t *ss)
{
	const ta_pqc_dims_t *d  = ta_pqc_dims(alg);
	const kem_ops_t     *op = kem_ops(alg);

	if (!d || !d->is_kem || !op || !sk || !ct || !ss)
		return -1;
	/* FIPS 203 静默失败在库内完成：密文被篡改时返回 0 但 ss 为伪随机 */
	return op->dec(ss, ct, sk) == 0 ? 0 : -3;
}

int ta_pqc_sign(uint32_t alg, const uint8_t *sk,
                const uint8_t *msg, size_t msg_len,
                const uint8_t *ctx, size_t ctx_len,
                uint8_t *sig, size_t *sig_len)
{
	const ta_pqc_dims_t *d  = ta_pqc_dims(alg);
	const sig_ops_t     *op = sig_ops(alg);
	int                 rc;

	if (!d || d->is_kem || !op || !sk || !sig || !sig_len)
		return -1;
	if ((!msg && msg_len) || (!ctx && ctx_len) || ctx_len > 255)
		return -1;
	rc = op->sign(sig, sig_len, msg, msg_len, ctx, ctx_len, sk);
	return rc == 0 ? 0 : -3;
}

int ta_pqc_verify(uint32_t alg, const uint8_t *pk,
                  const uint8_t *msg, size_t msg_len,
                  const uint8_t *ctx, size_t ctx_len,
                  const uint8_t *sig, size_t sig_len)
{
	const ta_pqc_dims_t *d  = ta_pqc_dims(alg);
	const sig_ops_t     *op = sig_ops(alg);
	int                 rc;

	if (!d || d->is_kem || !op || !pk || !sig)
		return -1;
	if ((!msg && msg_len) || (!ctx && ctx_len) || ctx_len > 255)
		return -1;
	rc = op->verify(sig, sig_len, msg, msg_len, ctx, ctx_len, pk);
	return rc == 0 ? 0 : -4;
}
