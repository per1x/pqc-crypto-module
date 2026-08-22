/* KEK 包裹/解包测试 */
#define _GNU_SOURCE
#include "testlib.h"
#include "pqchsm/wrap.h"
#include "pqchsm/kdr.h"
#include "pqchsm/util.h"

#include "../../src/store/wrap_internal.h"

#include <stdlib.h>

int main(void)
{
	test_use_stub_kdr();   /* 显式装桩：库里没有自动回退了 */
	uint8_t kek[PQC_KEK_LEN], kek2[PQC_KEK_LEN];
	uint8_t salt[16] = { 1, 2, 3, 4 };
	CHECK_EQ_INT(pqc_kek_derive(salt, sizeof(salt), kek), 0);

	TCASE("KEK 派生：确定性、随盐变化、随设备变化");
	CHECK_EQ_INT(pqc_kek_derive(salt, sizeof(salt), kek2), 0);
	CHECK_EQ_MEM(kek, kek2, PQC_KEK_LEN);
	salt[0] = 9;
	CHECK_EQ_INT(pqc_kek_derive(salt, sizeof(salt), kek2), 0);
	CHECK(memcmp(kek, kek2, PQC_KEK_LEN) != 0);
	salt[0] = 1;
	/* 换设备（换 KDR）→ 同盐也派生出不同 KEK，这是 sealing 的基础 */
	pqc_kdr_set_test_root((const uint8_t *)"device-B", 8);
	CHECK_EQ_INT(pqc_kek_derive(salt, sizeof(salt), kek2), 0);
	CHECK(memcmp(kek, kek2, PQC_KEK_LEN) != 0);
	pqc_kdr_set_test_root(NULL, 0);
	CHECK_EQ_INT(pqc_kek_derive(salt, sizeof(salt), kek2), 0);
	CHECK_EQ_MEM(kek, kek2, PQC_KEK_LEN);

	const uint8_t pt[]  = "ML-DSA-65 private key material (pretend)";
	const uint8_t aad[] = "slot=3;alg=ML-DSA-65;policy=no-export";
	size_t pt_len = sizeof(pt), aad_len = sizeof(aad);
	size_t cap = pqc_wrap_blob_len(pt_len);
	uint8_t *blob = malloc(cap), *blob2 = malloc(cap);
	uint8_t *out = malloc(cap);
	size_t blob_len = 0, out_len = 0;

	TCASE("wrap/unwrap 往返");
	CHECK_EQ_INT(pqc_wrap(kek, sizeof(kek), aad, aad_len, pt, pt_len, blob, cap, &blob_len), 0);
	CHECK_EQ_INT(blob_len, cap);
	CHECK_EQ_INT(pqc_wrap_blob_len(pt_len), pt_len + PQC_WRAP_OVERHEAD);
	CHECK_EQ_INT(pqc_unwrap(kek, sizeof(kek), aad, aad_len, blob, blob_len, out, cap, &out_len), 0);
	CHECK_EQ_INT(out_len, pt_len);
	CHECK_EQ_MEM(out, pt, pt_len);
	/* 密文里不应出现明文 */
	CHECK(memmem(blob, blob_len, pt, pt_len) == NULL);

	TCASE("nonce 每次新鲜：同样输入两次 wrap 结果不同");
	CHECK_EQ_INT(pqc_wrap(kek, sizeof(kek), aad, aad_len, pt, pt_len, blob2, cap, &blob_len), 0);
	CHECK(memcmp(blob, blob2, blob_len) != 0);
	/* nonce 字段本身不同 */
	CHECK(memcmp(blob + PQC_WRAP_HDR_LEN, blob2 + PQC_WRAP_HDR_LEN, PQC_WRAP_NONCE_LEN) != 0);

	TCASE("大量 wrap 的 nonce 互不相同");
	{
		enum { N = 200 };
		static uint8_t nonces[N][PQC_WRAP_NONCE_LEN];
		for (int i = 0; i < N; i++) {
			CHECK_EQ_INT(pqc_wrap(kek, sizeof(kek), aad, aad_len, pt, pt_len,
			                      blob2, cap, &blob_len), 0);
			memcpy(nonces[i], blob2 + PQC_WRAP_HDR_LEN, PQC_WRAP_NONCE_LEN);
		}
		int dup = 0;
		for (int i = 0; i < N; i++) {
			for (int j = i + 1; j < N; j++) {
				if (memcmp(nonces[i], nonces[j], PQC_WRAP_NONCE_LEN) == 0) {
					dup++;
				}
			}
		}
		CHECK_EQ_INT(dup, 0);
	}

	/* 红线：nonce 复用是 GCM 的死穴。这里把后果做成可执行的证明 ——
	 * 同 KEK 同 nonce 加密两条明文，两条密文异或 == 两条明文异或，
	 * 也就是密钥流被完整抵消掉、明文关系直接泄露。 */
	TCASE("nonce 复用会泄露明文关系（所以生产路径严禁复用）");
	{
		uint8_t nonce[PQC_WRAP_NONCE_LEN] = { 0 };
		const uint8_t a[16] = "AAAAAAAAAAAAAAA";
		const uint8_t b[16] = "BBBBBBBBBBBBBBB";
		size_t n = pqc_wrap_blob_len(sizeof(a));
		uint8_t *ba = malloc(n), *bb = malloc(n);
		size_t la = 0, lb = 0;
		CHECK_EQ_INT(pqc_wrap_with_nonce(kek, sizeof(kek), NULL, 0, a, sizeof(a),
		                                 nonce, ba, n, &la), 0);
		CHECK_EQ_INT(pqc_wrap_with_nonce(kek, sizeof(kek), NULL, 0, b, sizeof(b),
		                                 nonce, bb, n, &lb), 0);
		const uint8_t *ca = ba + PQC_WRAP_HDR_LEN + PQC_WRAP_NONCE_LEN;
		const uint8_t *cb = bb + PQC_WRAP_HDR_LEN + PQC_WRAP_NONCE_LEN;
		int leaked = 1;
		for (size_t i = 0; i < sizeof(a); i++) {
			if ((ca[i] ^ cb[i]) != (a[i] ^ b[i])) {
				leaked = 0;
			}
		}
		CHECK(leaked);   /* 复用 nonce 就是这个后果 */
		/* 而正常路径（随机 nonce）不具备这个性质 */
		CHECK_EQ_INT(pqc_wrap(kek, sizeof(kek), NULL, 0, a, sizeof(a), ba, n, &la), 0);
		CHECK_EQ_INT(pqc_wrap(kek, sizeof(kek), NULL, 0, b, sizeof(b), bb, n, &lb), 0);
		ca = ba + PQC_WRAP_HDR_LEN + PQC_WRAP_NONCE_LEN;
		cb = bb + PQC_WRAP_HDR_LEN + PQC_WRAP_NONCE_LEN;
		int still_leaked = 1;
		for (size_t i = 0; i < sizeof(a); i++) {
			if ((ca[i] ^ cb[i]) != (a[i] ^ b[i])) {
				still_leaked = 0;
			}
		}
		CHECK(!still_leaked);
		free(ba);
		free(bb);
	}

	/* ---- 负测试：任何一处改动都必须导致解包失败 ---- */
	CHECK_EQ_INT(pqc_wrap(kek, sizeof(kek), aad, aad_len, pt, pt_len, blob, cap, &blob_len), 0);

	TCASE("改元数据（AAD）→ 解包失败：防'改元数据复用密文'");
	{
		uint8_t bad_aad[sizeof(aad)];
		memcpy(bad_aad, aad, aad_len);
		bad_aad[0] ^= 0x01;
		CHECK_EQ_INT(pqc_unwrap(kek, sizeof(kek), bad_aad, aad_len, blob, blob_len,
		                        out, cap, &out_len), -1);
		/* 失败后 out 必须被清零，不能残留半解密的明文 */
		int all_zero = 1;
		for (size_t i = 0; i < cap; i++) {
			if (out[i] != 0) {
				all_zero = 0;
			}
		}
		CHECK(all_zero);
		/* AAD 长度不符也要拒（头里存了 aad_len） */
		CHECK_EQ_INT(pqc_unwrap(kek, sizeof(kek), aad, aad_len - 1, blob, blob_len,
		                        out, cap, &out_len), -1);
		CHECK_EQ_INT(pqc_unwrap(kek, sizeof(kek), NULL, 0, blob, blob_len,
		                        out, cap, &out_len), -1);
	}

	TCASE("改密文 / 改 tag / 改 nonce / 改头部 → 全部失败");
	for (size_t pos = 0; pos < blob_len; pos++) {
		memcpy(blob2, blob, blob_len);
		blob2[pos] ^= 0x01;
		int rc = pqc_unwrap(kek, sizeof(kek), aad, aad_len, blob2, blob_len,
		                    out, cap, &out_len);
		if (rc == 0) {
			fprintf(stderr, "  字节 %zu 被改动后仍解包成功！\n", pos);
		}
		CHECK_EQ_INT(rc, -1);
	}

	TCASE("换 KEK → 解包失败（设备绑定）");
	pqc_kdr_set_test_root((const uint8_t *)"device-B", 8);
	CHECK_EQ_INT(pqc_kek_derive(salt, sizeof(salt), kek2), 0);
	pqc_kdr_set_test_root(NULL, 0);
	CHECK_EQ_INT(pqc_unwrap(kek2, sizeof(kek2), aad, aad_len, blob, blob_len,
	                        out, cap, &out_len), -1);

	TCASE("截断 blob → 失败");
	CHECK_EQ_INT(pqc_unwrap(kek, sizeof(kek), aad, aad_len, blob, blob_len - 1,
	                        out, cap, &out_len), -1);
	CHECK_EQ_INT(pqc_unwrap(kek, sizeof(kek), aad, aad_len, blob, PQC_WRAP_OVERHEAD - 1,
	                        out, cap, &out_len), -1);

	TCASE("空明文与空 AAD 也要正确往返");
	{
		size_t n = pqc_wrap_blob_len(0);
		uint8_t *b0 = malloc(n);
		size_t l0 = 0, o0 = 1;
		CHECK_EQ_INT(pqc_wrap(kek, sizeof(kek), NULL, 0, NULL, 0, b0, n, &l0), 0);
		CHECK_EQ_INT(l0, PQC_WRAP_OVERHEAD);
		CHECK_EQ_INT(pqc_unwrap(kek, sizeof(kek), NULL, 0, b0, l0, out, cap, &o0), 0);
		CHECK_EQ_INT(o0, 0);
		free(b0);
	}

	TCASE("非法参数");
	CHECK_EQ_INT(pqc_wrap(NULL, 32, aad, aad_len, pt, pt_len, blob, cap, &blob_len), -1);
	CHECK_EQ_INT(pqc_wrap(kek, 16, aad, aad_len, pt, pt_len, blob, cap, &blob_len), -1);
	CHECK_EQ_INT(pqc_wrap(kek, 32, aad, aad_len, pt, pt_len, blob, cap - 1, &blob_len), -1);
	CHECK_EQ_INT(pqc_wrap(kek, 32, aad, aad_len, pt, pt_len, NULL, cap, &blob_len), -1);
	CHECK_EQ_INT(pqc_unwrap(kek, 16, aad, aad_len, blob, blob_len, out, cap, &out_len), -1);
	CHECK_EQ_INT(pqc_kek_derive(salt, sizeof(salt), NULL), -1);

	free(blob);
	free(blob2);
	free(out);
	return test_report("test_wrap");
}
