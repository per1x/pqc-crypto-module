/* test_wrap.c —— PWRP 包裹：往返、篡改 tag/AAD、失败清零 */
#include <stdio.h>
#include <string.h>

#include "ta_random.h"
#include "ta_wrap.h"
#include "pqchsm_ta_proto.h"

int test_wrap(void)
{
	int     fails = 0;
	uint8_t kek[TA_KEK_LEN], pt[300], blob[300 + TA_WRAP_OVERHEAD];
	uint8_t out[300], aad[32];
	size_t  blob_len = 0, out_len = 0;
	int     rc;

	pqchsm_randombytes(kek, sizeof(kek));
	pqchsm_randombytes(pt, sizeof(pt));
	pqchsm_randombytes(aad, sizeof(aad));

	rc = ta_wrap_seal(kek, aad, sizeof(aad), pt, sizeof(pt),
	                  blob, sizeof(blob), &blob_len);
	if (rc != 0 || blob_len != ta_wrap_blob_len(sizeof(pt))) {
		printf("FAIL wrap seal rc=%d\n", rc);
		fails++;
	}
	rc = ta_wrap_open(kek, aad, sizeof(aad), blob, blob_len,
	                  out, sizeof(out), &out_len);
	if (rc != 0 || out_len != sizeof(pt) || memcmp(out, pt, sizeof(pt)) != 0) {
		printf("FAIL wrap roundtrip rc=%d\n", rc);
		fails++;
	} else {
		printf("ok   wrap roundtrip\n");
	}

	/* 篡改 tag → -2，且输出缓冲清零 */
	blob[blob_len - 1] ^= 1;
	memset(out, 0xAA, sizeof(out));
	rc = ta_wrap_open(kek, aad, sizeof(aad), blob, blob_len,
	                  out, sizeof(out), &out_len);
	{
		size_t i, nonzero = 0;
		for (i = 0; i < sizeof(out); i++)
			nonzero |= out[i];
		if (rc != -2 || nonzero) {
			printf("FAIL wrap tampered tag rc=%d zeroed=%d\n", rc,
			       nonzero == 0);
			fails++;
		} else {
			printf("ok   wrap tampered tag rejected+zeroed\n");
		}
	}
	blob[blob_len - 1] ^= 1;

	/* AAD 不匹配 → -2 */
	aad[0] ^= 1;
	rc = ta_wrap_open(kek, aad, sizeof(aad), blob, blob_len,
	                  out, sizeof(out), &out_len);
	if (rc != -2 && rc != -1) { /* 头里 aad_len 不符时可能走 -1 */
		printf("FAIL wrap wrong aad rc=%d\n", rc);
		fails++;
	} else {
		printf("ok   wrap wrong aad rejected (rc=%d)\n", rc);
	}
	aad[0] ^= 1;

	/* 空明文、空 AAD 边界 */
	rc = ta_wrap_seal(kek, NULL, 0, NULL, 0, blob, sizeof(blob), &blob_len);
	if (rc != 0 || blob_len != TA_WRAP_OVERHEAD) {
		printf("FAIL wrap empty pt rc=%d\n", rc);
		fails++;
	}
	rc = ta_wrap_open(kek, NULL, 0, blob, blob_len, out, sizeof(out), &out_len);
	if (rc != 0 || out_len != 0) {
		printf("FAIL unwrap empty pt rc=%d\n", rc);
		fails++;
	} else {
		printf("ok   wrap empty pt\n");
	}

	/* D7：对称 blob 与种子 blob 的域分隔。
	 * 两类 blob 同一个 KEK —— 唯一把它们分开的是 AAD。一份 32 字节的 ML-DSA ξ
	 * （种子 blob，AAD 为空）绝不能被 CMD_SYMKEY_LOAD 当成 AES-256 密钥装进金库，
	 * 反过来对称 blob 也不能被 CMD_SEED_REPLAY 当种子送进 PL。 */
	{
		static const uint8_t sym_aad[] = TA_SYMKEY_AAD;
		const size_t sym_aad_len = sizeof(sym_aad) - 1;
		uint8_t seed[32];

		pqchsm_randombytes(seed, sizeof(seed));
		/* 种子 blob（AAD 空）→ 按对称的 AAD 打开：必须失败 */
		rc = ta_wrap_seal(kek, NULL, 0, seed, sizeof(seed),
		                  blob, sizeof(blob), &blob_len);
		rc = rc ? rc : ta_wrap_open(kek, sym_aad, sym_aad_len, blob, blob_len,
		                            out, sizeof(out), &out_len);
		if (rc == 0) {
			printf("FAIL 种子 blob 被当成对称密钥 blob 打开了\n");
			fails++;
		} else {
			printf("ok   种子 blob 不能当对称 blob 用 (rc=%d)\n", rc);
		}
		/* 对称 blob → 按种子的（空）AAD 打开：必须失败 */
		rc = ta_wrap_seal(kek, sym_aad, sym_aad_len, seed, sizeof(seed),
		                  blob, sizeof(blob), &blob_len);
		rc = rc ? rc : ta_wrap_open(kek, NULL, 0, blob, blob_len,
		                            out, sizeof(out), &out_len);
		if (rc == 0) {
			printf("FAIL 对称 blob 被当成种子 blob 打开了\n");
			fails++;
		} else {
			printf("ok   对称 blob 不能当种子 blob 用 (rc=%d)\n", rc);
		}
		/* 正向：同域往返 */
		rc = ta_wrap_seal(kek, sym_aad, sym_aad_len, seed, sizeof(seed),
		                  blob, sizeof(blob), &blob_len);
		rc = rc ? rc : ta_wrap_open(kek, sym_aad, sym_aad_len, blob, blob_len,
		                            out, sizeof(out), &out_len);
		if (rc != 0 || out_len != sizeof(seed) || memcmp(out, seed, sizeof(seed))) {
			printf("FAIL 对称 blob 同域往返 rc=%d\n", rc);
			fails++;
		} else {
			printf("ok   对称 blob 同域往返\n");
		}
	}

	return fails;
}
