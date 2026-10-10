/* test_kdf.c —— TA 内 KMAC-256 验证
 *
 * 两层：
 *   1. 固定 KAT（由 Mac 上 OpenSSL 3.6 EVP_MAC(KMAC-256) 生成，/tmp/kmac_gen.c），
 *      任何环境都跑 —— 这锁死与 src/crypto/kdf.c 的逐字节一致性；
 *   2. 若本机 OpenSSL ≥ 3.0，再做 200 轮随机对拍（WSL 是 1.1.1，自动跳过）。
 */
#include <stdio.h>
#include <string.h>

#include <openssl/opensslv.h>

#include "ta_fips202.h"
#include "ta_kdf.h"
/* 随机对拍那一段用 pqchsm_randombytes()。以前靠隐式声明混过去 ——
 * C99 起那是错误，新编译器直接拒编。 */
#include "ta_random.h"

/* ---- KAT（OpenSSL 3.6.3 生成）---- */
static const uint8_t kat1_key[32] = "0123456789abcdef0123456789abcdef";
static const uint8_t kat1_out[32] =
"\x39\x38\xf9\xde\x95\x00\x3a\x7a\x1b\x01\x4d\x9a\xfe\xfe\x5c\x57\x9c\x1e\xcd\xe9\x19\x66\x9b\xe2\x22\xd9\x9a\x52\xe0\x68\xcb\xb7";

static const uint8_t kat2_data[] = "The quick brown fox jumps";
static const uint8_t kat2_custom[] = "storage";
static const uint8_t kat2_out[32] =
"\x7b\xd4\xd2\x39\xd2\xa6\xc1\x85\x43\xbe\x24\xf0\xc9\x41\x05\xb8\xfb\x4d\x7e\xf6\x4a\xea\x16\x9a\xff\xab\x38\x98\x1a\x88\x02\x67";

/* 对齐 SP 800-185 KMAC256 Sample#1 的输入形状：K=00..1f, X=00010203, S="" */
static const uint8_t kat3_out[32] =
"\x18\x42\x16\xbc\xa6\x65\x7d\x50\xe3\xe1\xb5\xe9\xf6\x92\x18\x58\x10\xad\xb3\x81\x6d\x0b\xd2\x84\x21\x43\x40\x5f\x9d\x35\x03\x02";

static const uint8_t kat4_custom[] = "My Tagged Application";
static const uint8_t kat4_out[64] =
"\x91\x72\xf1\xae\xf3\xdb\xc9\xad\x02\x4e\xb3\x5d\xb3\xc1\x9d\x78\x42\x6d\x78\x6d\xc0\xe3\x21\x99\x92\x01\xed\xf7\x16\xd7\xa1\x6a\xc5\x2f\x6a\xf8\x97\xe0\xf3\xec\xd1\xe9\xd1\xb4\x5f\x3e\xf1\x81\x86\xe9\xfe\x8b\x90\x79\xb2\x75\x45\x50\x82\x3d\x90\x42\x08\xeb";

static const uint8_t kat5_key[32] = "PQCHSM-KDR-TEST-VECTOR-012345678";
static const uint8_t kat5_salt[16] = "keystore-salt-01";
static const uint8_t kat5_out[32] =
"\x83\x35\xda\x51\xec\xda\x97\x8b\x3a\xc4\x95\x1d\x74\x9c\xe7\x68\x69\x6f\x4a\x0a\x37\x1b\x7b\x46\x7c\x9a\xf9\x8c\x23\x06\x0e\xf9";

static int fails;

static void kat_check(const char *name, const uint8_t *key, size_t key_len,
                      const uint8_t *data, size_t data_len,
                      const uint8_t *custom, size_t custom_len,
                      const uint8_t *want, size_t out_len)
{
	uint8_t out[64];

	if (ta_kmac256(key, key_len, data, data_len,
	               custom, custom_len, out, out_len) != 0 ||
	    memcmp(out, want, out_len) != 0) {
		printf("FAIL %s\n", name);
		fails++;
	} else {
		printf("ok   %s\n", name);
	}
}

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/params.h>

static int openssl_kmac256(const uint8_t *key, size_t key_len,
                           const uint8_t *data, size_t data_len,
                           const uint8_t *custom, size_t custom_len,
                           uint8_t *out, size_t out_len)
{
	EVP_MAC     *mac = NULL;
	EVP_MAC_CTX *ctx = NULL;
	OSSL_PARAM   params[3];
	size_t       nparams = 0, size_param = out_len, written = 0;
	int          ret = -1;

	mac = EVP_MAC_fetch(NULL, "KMAC-256", NULL);
	if (!mac)
		goto cleanup;
	ctx = EVP_MAC_CTX_new(mac);
	if (!ctx)
		goto cleanup;
	params[nparams++] =
	        OSSL_PARAM_construct_size_t(OSSL_MAC_PARAM_SIZE, &size_param);
	if (custom && custom_len) {
		params[nparams++] = OSSL_PARAM_construct_octet_string(
		        OSSL_MAC_PARAM_CUSTOM, (void *)custom, custom_len);
	}
	params[nparams] = OSSL_PARAM_construct_end();
	if (EVP_MAC_init(ctx, key, key_len, params) != 1)
		goto cleanup;
	if (data_len && EVP_MAC_update(ctx, data, data_len) != 1)
		goto cleanup;
	if (EVP_MAC_final(ctx, out, &written, out_len) != 1)
		goto cleanup;
	ret = (written == out_len) ? 0 : -1;
cleanup:
	EVP_MAC_CTX_free(ctx);
	EVP_MAC_free(mac);
	return ret;
}

static void crosscheck(void)
{
	int     i, bad = 0;
	uint8_t key[32], data[100], custom[64], out_a[64], out_b[64];
	size_t  key_len, data_len, custom_len, out_len;

	for (i = 0; i < 200; i++) {
		pqchsm_randombytes(key, sizeof(key));
		pqchsm_randombytes(data, sizeof(data));
		pqchsm_randombytes(custom, sizeof(custom));
		key_len    = 16 + (size_t)i % 17;
		data_len   = (size_t)(i * 37) % 101;
		custom_len = (size_t)(i * 13) % 65;
		out_len    = (size_t[]){ 16, 32, 64 }[i % 3];

		if (ta_kmac256(key, key_len, data, data_len,
		               custom, custom_len, out_a, out_len) != 0 ||
		    openssl_kmac256(key, key_len, data, data_len,
		                    custom, custom_len, out_b, out_len) != 0 ||
		    memcmp(out_a, out_b, out_len) != 0) {
			printf("FAIL kmac256 cross (iter %d)\n", i);
			bad++;
		}
	}
	if (bad)
		fails += bad;
	else
		printf("ok   kmac256 vs OpenSSL (200 iters)\n");
}
#else
static void crosscheck(void)
{
	printf("skip kmac256 vs OpenSSL（本机 OpenSSL < 3.0，无 EVP_MAC；"
	       "KAT 已覆盖语义）\n");
}
#endif

int test_kdf(void)
{
	uint8_t key3[32], data3[4], out_a[32], out_b[32];
	int     i;

	fails = 0;

	kat_check("kmac kat1 (no data, no custom)", kat1_key, 32,
	          NULL, 0, NULL, 0, kat1_out, 32);
	kat_check("kmac kat2 (data+custom)", kat1_key, 32,
	          kat2_data, sizeof(kat2_data) - 1,
	          kat2_custom, sizeof(kat2_custom) - 1, kat2_out, 32);
	for (i = 0; i < 32; i++)
		key3[i] = (uint8_t)i;
	for (i = 0; i < 4; i++)
		data3[i] = (uint8_t)i;
	kat_check("kmac kat3 (SP800-185 sample1 shape)", key3, 32,
	          data3, 4, NULL, 0, kat3_out, 32);
	kat_check("kmac kat4 (custom, out=64)", key3, 32,
	          data3, 4, kat4_custom, sizeof(kat4_custom) - 1, kat4_out, 64);

	/* KDF 语义：KMAC256(ikm, salt, custom=label)，与 kat5 对拍 */
	if (ta_kdf_derive(kat5_key, 32, kat5_salt, 16,
	                  "pqc-hsm/storage-kek", out_a, 32) != 0 ||
	    memcmp(out_a, kat5_out, 32) != 0) {
		printf("FAIL kdf kat5 (storage-kek)\n");
		fails++;
	} else {
		printf("ok   kdf kat5 (storage-kek)\n");
	}

	/* label 域分隔：不同 label 结果必须不同 */
	if (ta_kdf_derive(kat5_key, 32, kat5_salt, 16, "backup", out_b, 32) != 0 ||
	    memcmp(out_a, out_b, 32) == 0) {
		printf("FAIL kdf label separation\n");
		fails++;
	} else {
		printf("ok   kdf label separation\n");
	}
	/* label 必须非空 */
	if (ta_kdf_derive(kat5_key, 32, kat5_salt, 16, NULL, out_b, 32) == 0 ||
	    ta_kdf_derive(kat5_key, 32, kat5_salt, 16, "", out_b, 32) == 0) {
		printf("FAIL kdf empty label accepted\n");
		fails++;
	} else {
		printf("ok   kdf empty label rejected\n");
	}

	crosscheck();
	return fails;
}
