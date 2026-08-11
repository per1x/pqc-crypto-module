/* test_interop_oqs.c —— TA 密码核与 liboqs 0.16 互操作对拍（仅 Mac 跑）
 *
 * 两套完全独立的实现互为 KAT：
 *   ML-KEM：同种子 derand keypair 逐字节相等（= FIPS 203 KAT）；
 *           我方 pk → oqs encaps → 我方 decaps；oqs pk → 我方 encaps → oqs decaps。
 *   ML-DSA：我方 sign → oqs verify；oqs sign → 我方 verify（含 ctx）。
 * 编译：cc -O2 -I../ta -I../ta/config -I../include -I/opt/homebrew/include \
 *        ../ta/ta_fips202.c ../ta/ta_random.c ../ta/ta_pqc.c \
 *        ../ta/ta_mlkem*.c ../ta/ta_mldsa*.c test_interop_oqs.c \
 *        -L/opt/homebrew/lib -loqs -o interop_test
 */
#include <stdio.h>
#include <string.h>

#include <oqs/oqs.h>

#include "ta_pqc.h"
#include "ta_random.h"

static int fails;

static void kem_interop(uint32_t alg, const char *oqs_name)
{
	const ta_pqc_dims_t *d = ta_pqc_dims(alg);
	OQS_KEM *kem = OQS_KEM_new(oqs_name);
	uint8_t pk_ours[1600], sk_ours[3200];
	uint8_t pk_oqs[1600], sk_oqs[3200];
	uint8_t ct[1600], ss_a[32], ss_b[32], seed[64];

	if (!kem) {
		printf("FAIL %s: OQS_KEM_new\n", oqs_name);
		fails++;
		return;
	}
	if (kem->length_public_key != d->pk_len ||
	    kem->length_secret_key != d->sk_len ||
	    kem->length_ciphertext != d->ct_len) {
		printf("FAIL %s: dims mismatch\n", oqs_name);
		fails++;
		OQS_KEM_free(kem);
		return;
	}

	pqchsm_randombytes(seed, d->seed_len);
	if (ta_pqc_keypair_from_seed(alg, seed, d->seed_len, pk_ours, sk_ours) != 0 ||
	    OQS_KEM_keypair_derand(kem, pk_oqs, sk_oqs, seed) != OQS_SUCCESS) {
		printf("FAIL %s: keypair_derand\n", oqs_name);
		fails++;
		OQS_KEM_free(kem);
		return;
	}
	if (memcmp(pk_ours, pk_oqs, d->pk_len) != 0 ||
	    memcmp(sk_ours, sk_oqs, d->sk_len) != 0) {
		printf("FAIL %s: same-seed keypair mismatch (=KAT 失败)\n", oqs_name);
		fails++;
	} else {
		printf("ok   %s same-seed keypair == liboqs\n", oqs_name);
	}

	/* 我方公钥 → oqs 封装 → 我方解封 */
	if (OQS_KEM_encaps(kem, ct, ss_a, pk_ours) != OQS_SUCCESS ||
	    ta_pqc_decaps(alg, sk_ours, ct, ss_b) != 0 ||
	    memcmp(ss_a, ss_b, 32) != 0) {
		printf("FAIL %s: ours-pk oqs-enc ours-dec\n", oqs_name);
		fails++;
	} else {
		printf("ok   %s ours-pk oqs-enc ours-dec\n", oqs_name);
	}

	/* oqs 公钥 → 我方封装 → oqs 解封 */
	if (ta_pqc_encaps(alg, pk_oqs, ct, ss_a) != 0 ||
	    OQS_KEM_decaps(kem, ss_b, ct, sk_oqs) != OQS_SUCCESS ||
	    memcmp(ss_a, ss_b, 32) != 0) {
		printf("FAIL %s: oqs-pk ours-enc oqs-dec\n", oqs_name);
		fails++;
	} else {
		printf("ok   %s oqs-pk ours-enc oqs-dec\n", oqs_name);
	}
	OQS_KEM_free(kem);
}

static void sig_interop(uint32_t alg, const char *oqs_name)
{
	const ta_pqc_dims_t *d = ta_pqc_dims(alg);
	OQS_SIG *sig = OQS_SIG_new(oqs_name);
	uint8_t pk_ours[2600], sk_ours[4900];
	uint8_t pk_oqs[2600], sk_oqs[4900];
	uint8_t sigbuf[4700];
	size_t  siglen = 0;
	static const uint8_t msg[] = "pqc-hsm tee interop test message";
	static const uint8_t ctx[] = "interop";

	if (!sig) {
		printf("FAIL %s: OQS_SIG_new\n", oqs_name);
		fails++;
		return;
	}
	if (sig->length_public_key != d->pk_len ||
	    sig->length_secret_key != d->sk_len ||
	    sig->length_signature != d->sig_len) {
		printf("FAIL %s: dims mismatch\n", oqs_name);
		fails++;
		OQS_SIG_free(sig);
		return;
	}

	/* 我方签名 → oqs 验 */
	if (ta_pqc_keypair(alg, pk_ours, sk_ours) != 0) {
		printf("FAIL %s: ours keypair\n", oqs_name);
		fails++;
		OQS_SIG_free(sig);
		return;
	}
	siglen = sizeof(sigbuf);
	if (ta_pqc_sign(alg, sk_ours, msg, sizeof(msg) - 1, ctx, sizeof(ctx) - 1,
	                sigbuf, &siglen) != 0 ||
	    OQS_SIG_verify_with_ctx_str(sig, msg, sizeof(msg) - 1,
	                                sigbuf, siglen,
	                                ctx, sizeof(ctx) - 1,
	                                pk_ours) != OQS_SUCCESS) {
		printf("FAIL %s: ours-sign oqs-verify\n", oqs_name);
		fails++;
	} else {
		printf("ok   %s ours-sign oqs-verify\n", oqs_name);
	}

	/* oqs 签名 → 我方验 */
	if (OQS_SIG_keypair(sig, pk_oqs, sk_oqs) != OQS_SUCCESS)
		{
		printf("FAIL %s: oqs keypair\n", oqs_name);
		fails++;
		OQS_SIG_free(sig);
		return;
	}
	siglen = sizeof(sigbuf);
	if (OQS_SIG_sign_with_ctx_str(sig, sigbuf, &siglen,
	                              msg, sizeof(msg) - 1,
	                              ctx, sizeof(ctx) - 1,
	                              sk_oqs) != OQS_SUCCESS ||
	    ta_pqc_verify(alg, pk_oqs, msg, sizeof(msg) - 1,
	                  ctx, sizeof(ctx) - 1, sigbuf, siglen) != 0) {
		printf("FAIL %s: oqs-sign ours-verify\n", oqs_name);
		fails++;
	} else {
		printf("ok   %s oqs-sign ours-verify\n", oqs_name);
	}
	OQS_SIG_free(sig);
}

int main(void)
{
	kem_interop(1, "ML-KEM-512");
	kem_interop(2, "ML-KEM-768");
	kem_interop(3, "ML-KEM-1024");
	sig_interop(4, "ML-DSA-44");
	sig_interop(5, "ML-DSA-65");
	sig_interop(6, "ML-DSA-87");

	if (fails) {
		printf("\n%d INTEROP FAILURES\n", fails);
		return 1;
	}
	printf("\nINTEROP ALL PASS\n");
	return 0;
}
