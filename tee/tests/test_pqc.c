/* test_pqc.c —— 六参数集往返 + 种子确定性 + 篡改拒绝 */
#include <stdio.h>
#include <string.h>

#include "ta_pqc.h"
#include "ta_random.h"

static int fails;

static void test_kem(uint32_t alg)
{
	const ta_pqc_dims_t *d = ta_pqc_dims(alg);
	uint8_t  pk[1600], sk[3200], pk2[1600], sk2[3200];
	uint8_t  ct[1600], ss[32], ss2[32], seed[64];
	char     name[64];

	pqchsm_randombytes(seed, sizeof(seed));
	if (ta_pqc_keypair_from_seed(alg, seed, d->seed_len, pk, sk) != 0) {
		printf("FAIL kem %u keypair_from_seed\n", alg);
		fails++;
		return;
	}
	/* 同种子 → 同密钥对（种子存储优化路径的根基） */
	if (ta_pqc_keypair_from_seed(alg, seed, d->seed_len, pk2, sk2) != 0 ||
	    memcmp(pk, pk2, d->pk_len) != 0 || memcmp(sk, sk2, d->sk_len) != 0) {
		printf("FAIL kem %u seed determinism\n", alg);
		fails++;
		return;
	}
	if (ta_pqc_encaps(alg, pk, ct, ss) != 0) {
		printf("FAIL kem %u encaps\n", alg);
		fails++;
		return;
	}
	if (ta_pqc_decaps(alg, sk, ct, ss2) != 0 || memcmp(ss, ss2, 32) != 0) {
		printf("FAIL kem %u roundtrip\n", alg);
		fails++;
		return;
	}
	/* 篡改密文 → 静默失败：返回 0 但 ss 不同（FIPS 203 implicit rejection） */
	ct[0] ^= 1;
	if (ta_pqc_decaps(alg, sk, ct, ss2) != 0 || memcmp(ss, ss2, 32) == 0) {
		printf("FAIL kem %u implicit rejection\n", alg);
		fails++;
		return;
	}
	snprintf(name, sizeof(name), "kem alg=%u roundtrip+determinism", alg);
	printf("ok   %s\n", name);

	/* 随机 keypair 路径也过一遍 */
	if (ta_pqc_keypair(alg, pk, sk) != 0 ||
	    ta_pqc_encaps(alg, pk, ct, ss) != 0 ||
	    ta_pqc_decaps(alg, sk, ct, ss2) != 0 || memcmp(ss, ss2, 32) != 0) {
		printf("FAIL kem %u random keypair roundtrip\n", alg);
		fails++;
	}
}

static void test_sig(uint32_t alg)
{
	const ta_pqc_dims_t *d = ta_pqc_dims(alg);
	uint8_t pk[2600], sk[4900], pk2[2600], sk2[4900];
	uint8_t sig[4700], seed[32];
	uint8_t msg[257], ctx[10];
	size_t  sig_len = 0;
	char    name[64];
	int     i;

	for (i = 0; i < (int)sizeof(msg); i++)
		msg[i] = (uint8_t)i;
	memcpy(ctx, "ctx-test", 8);

	pqchsm_randombytes(seed, sizeof(seed));
	if (ta_pqc_keypair_from_seed(alg, seed, d->seed_len, pk, sk) != 0) {
		printf("FAIL sig %u keypair_from_seed\n", alg);
		fails++;
		return;
	}
	if (ta_pqc_keypair_from_seed(alg, seed, d->seed_len, pk2, sk2) != 0 ||
	    memcmp(pk, pk2, d->pk_len) != 0 || memcmp(sk, sk2, d->sk_len) != 0) {
		printf("FAIL sig %u seed determinism\n", alg);
		fails++;
		return;
	}
	sig_len = sizeof(sig);
	if (ta_pqc_sign(alg, sk, msg, sizeof(msg), ctx, 8, sig, &sig_len) != 0 ||
	    sig_len != d->sig_len) {
		printf("FAIL sig %u sign\n", alg);
		fails++;
		return;
	}
	if (ta_pqc_verify(alg, pk, msg, sizeof(msg), ctx, 8, sig, sig_len) != 0) {
		printf("FAIL sig %u verify\n", alg);
		fails++;
		return;
	}
	/* 篡改签名 / 错误 ctx / 篡改消息 → -4 */
	sig[10] ^= 1;
	if (ta_pqc_verify(alg, pk, msg, sizeof(msg), ctx, 8, sig, sig_len) != -4) {
		printf("FAIL sig %u tampered sig accepted\n", alg);
		fails++;
		return;
	}
	sig[10] ^= 1;
	if (ta_pqc_verify(alg, pk, msg, sizeof(msg), ctx, 7, sig, sig_len) != -4) {
		printf("FAIL sig %u wrong ctx accepted\n", alg);
		fails++;
		return;
	}
	msg[0] ^= 1;
	if (ta_pqc_verify(alg, pk, msg, sizeof(msg), ctx, 8, sig, sig_len) != -4) {
		printf("FAIL sig %u tampered msg accepted\n", alg);
		fails++;
		return;
	}
	msg[0] ^= 1;
	snprintf(name, sizeof(name), "sig alg=%u sign/verify+reject", alg);
	printf("ok   %s\n", name);
}

int test_pqc(void)
{
	fails = 0;
	test_kem(1); /* ML-KEM-512 */
	test_kem(2); /* ML-KEM-768 */
	test_kem(3); /* ML-KEM-1024 */
	test_sig(4); /* ML-DSA-44 */
	test_sig(5); /* ML-DSA-65 */
	test_sig(6); /* ML-DSA-87 */
	return fails;
}
