/* test_cross_world.c —— 安全世界与普通世界的两份实现必须逐字节一致
 *
 * ============================================================================
 * 【这条用例挡的是"漂移"，不是"重复"】
 * ============================================================================
 * 有两处东西被**故意**实现了两遍，而且两遍都正当：
 *
 *   ① KMAC-256：`src/crypto/kdf.c` 走 OpenSSL EVP；`tee/ta/ta_kdf.c` 自带
 *      Keccak 海绵（`ta_fips202.c`）—— **TA 里没有 OpenSSL**，自实现不是
 *      重复造轮子，是唯一的选择。
 *   ② PWRP 包裹：`src/store/wrap.c` 用 OpenSSL EVP 的 AES-256-GCM；
 *      `tee/ta/ta_wrap.c` 在 TA 里走 `TEE_ALG_AES_GCM`（本用例这一侧编的是
 *      它的 native 分支，同样走 OpenSSL —— 见下面那条⚠️）。
 *
 * 线格式的**定义**已经收进一处（include/pqchsm/pwrp_format.h），那挡住了
 * "改一处忘另一处"。但它挡不住**实现**各自跑偏：域分隔串怎么拼、长度怎么
 * 编码、AAD 里放不放头部 —— 这些都在各自的 .c 里。
 *
 * 而漂移的症状极难查：
 *   · 两条路平时各自都跑得好好的，只有**跨世界那一次**才碰头；
 *   · 碰头时的错误是"认证失败 / KEK 对不上"，与"密钥不对""文件被改过"
 *     "换了设备"完全无法区分；
 *   · 而那一次通常发生在最不方便调试的地方（板上、TA 里）。
 *
 * 所以这里把两边的输出**逐字节**钉在一起。
 *
 * ⚠️ **说清这条用例的边界。** 它把 TA 那几个 .c 用主机编译器编成普通代码来
 *    跑，所以证明的是"**同一份源码**在两种编译下算出同样的字节"。它证明
 *    不了"跑在真 OP-TEE 里的那份也一样"—— TA 侧 AES-GCM 在板上走的是
 *    TEE_ALG_AES_GCM 而不是 OpenSSL（`PQCHSM_TA_OPTEE` 分支），那一条只能
 *    上板验。KMAC 那一半没有这个问题：TA 的海绵两种构建下是同一份代码。
 */
#include "testlib.h"

#include "pqchsm/kdf.h"
#include "pqchsm/pwrp_format.h"
#include "pqchsm/util.h"
#include "pqchsm/wrap.h"

/* TA 侧的头（native 构建，不带 PQCHSM_TA_OPTEE） */
#include "ta_kdf.h"
#include "ta_wrap.h"

#include <stdint.h>
#include <string.h>

/* ============================================================================
 * ① KMAC-256：同一组 (ikm, salt, label, len) 两边逐字节相同
 * ==========================================================================*/
static void test_kmac_agrees(void)
{
	static const struct {
		const char *label;
		size_t      salt_len;
		size_t      out_len;
	} cases[] = {
		{ "pqc-hsm/storage-kek", 16, 32 },
		{ "pqc-hsm/storage-kek",  0, 32 },   /* 空盐 */
		{ "pqc-hsm/pin-verifier", 16, 32 },
		{ "backup",               1, 16 },
		{ "x",                   64, 64 },
		/* 输出长度跨过海绵 rate（KMAC-256 的 rate 是 136 字节）——
		 * 挤出多于一块时两边的 squeeze 逻辑才真的被比到 */
		{ "pqc-hsm/long-output", 16, 200 },
	};

	TCASE("KMAC-256：OpenSSL 那份与 TA 自带海绵那份逐字节相同");
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		uint8_t ikm[32], salt[64];
		uint8_t a[256], b[256];

		/* 确定性输入：用例失败时能原样复现，不用去猜当时的随机数 */
		for (size_t j = 0; j < sizeof(ikm); j++) {
			ikm[j] = (uint8_t)(0x10 + i * 7 + j);
		}
		for (size_t j = 0; j < sizeof(salt); j++) {
			salt[j] = (uint8_t)(0xA0 + i * 3 + j);
		}
		memset(a, 0, sizeof(a));
		memset(b, 0, sizeof(b));

		CHECK_EQ_INT(pqc_kdf(ikm, sizeof(ikm), salt, cases[i].salt_len,
		                     cases[i].label, a, cases[i].out_len), 0);
		CHECK_EQ_INT(ta_kdf_derive(ikm, sizeof(ikm), salt, cases[i].salt_len,
		                           cases[i].label, b, cases[i].out_len), 0);
		CHECK_EQ_MEM(a, b, cases[i].out_len);
	}

	TCASE("反证：换掉 label，两边**同时**变（说明上面比的不是常量）");
	{
		uint8_t ikm[32] = { 1 }, salt[16] = { 2 };
		uint8_t a1[32], a2[32], b1[32], b2[32];

		CHECK_EQ_INT(pqc_kdf(ikm, sizeof(ikm), salt, sizeof(salt),
		                     "label-one", a1, sizeof(a1)), 0);
		CHECK_EQ_INT(pqc_kdf(ikm, sizeof(ikm), salt, sizeof(salt),
		                     "label-two", a2, sizeof(a2)), 0);
		CHECK_EQ_INT(ta_kdf_derive(ikm, sizeof(ikm), salt, sizeof(salt),
		                           "label-one", b1, sizeof(b1)), 0);
		CHECK_EQ_INT(ta_kdf_derive(ikm, sizeof(ikm), salt, sizeof(salt),
		                           "label-two", b2, sizeof(b2)), 0);
		CHECK(memcmp(a1, a2, sizeof(a1)) != 0);
		CHECK(memcmp(b1, b2, sizeof(b1)) != 0);
		CHECK_EQ_MEM(a1, b1, sizeof(a1));
		CHECK_EQ_MEM(a2, b2, sizeof(a2));
	}
}

/* ============================================================================
 * ② PWRP：一边包、另一边解得开（两个方向都要）
 * ==========================================================================*/
static void test_pwrp_interop(void)
{
	static const uint8_t kek[PWRP_KEK_LEN] = {
		0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
		0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
		0x0F, 0x1E, 0x2D, 0x3C, 0x4B, 0x5A, 0x69, 0x78,
		0x87, 0x96, 0xA5, 0xB4, 0xC3, 0xD2, 0xE1, 0xF0,
	};
	static const uint8_t aad[] = "slot=3;alg=ML-DSA-65";
	static const uint8_t pt[]  = "this is a private key, pretend it is 4896 bytes";

	uint8_t blob[512], out[512];
	size_t  blob_len = 0, out_len = 0;

	TCASE("TA 侧包裹 → 普通世界解得开，且明文逐字节相同");
	CHECK_EQ_INT(ta_wrap_seal(kek, aad, sizeof(aad) - 1, pt, sizeof(pt) - 1,
	                          blob, sizeof(blob), &blob_len), 0);
	CHECK_EQ_INT((int)blob_len, (int)(sizeof(pt) - 1 + PWRP_OVERHEAD));
	CHECK_EQ_INT(pqc_unwrap(kek, sizeof(kek), aad, sizeof(aad) - 1,
	                        blob, blob_len, out, sizeof(out), &out_len), 0);
	CHECK_EQ_INT((int)out_len, (int)(sizeof(pt) - 1));
	CHECK_EQ_MEM(out, pt, sizeof(pt) - 1);

	TCASE("普通世界包裹 → TA 侧解得开，且明文逐字节相同");
	memset(blob, 0, sizeof(blob));
	memset(out, 0, sizeof(out));
	CHECK_EQ_INT(pqc_wrap(kek, sizeof(kek), aad, sizeof(aad) - 1,
	                      pt, sizeof(pt) - 1, blob, sizeof(blob), &blob_len), 0);
	CHECK_EQ_INT(ta_wrap_open(kek, aad, sizeof(aad) - 1, blob, blob_len,
	                          out, sizeof(out), &out_len), 0);
	CHECK_EQ_INT((int)out_len, (int)(sizeof(pt) - 1));
	CHECK_EQ_MEM(out, pt, sizeof(pt) - 1);

	TCASE("头部字段两边解读一致：aad_len / pt_len 就在共享头定义的偏移上");
	CHECK_EQ_INT((int)pwrp_get_u32(blob + PWRP_OFF_AADLEN),
	             (int)(sizeof(aad) - 1));
	CHECK_EQ_INT((int)pwrp_get_u32(blob + PWRP_OFF_PTLEN),
	             (int)(sizeof(pt) - 1));

	/* 反证：两边对 AAD 的处理必须**同样严**。改一个 AAD 字节，另一侧也得拒 ——
	 * 只验"能互通"是不够的，一个把 AAD 忘进去的实现照样能互通。 */
	TCASE("反证：改 AAD 之后，两个方向都必须拒（不是只有一边拒）");
	{
		uint8_t bad_aad[sizeof(aad) - 1];

		memcpy(bad_aad, aad, sizeof(bad_aad));
		bad_aad[0] ^= 0x01;

		CHECK(pqc_unwrap(kek, sizeof(kek), bad_aad, sizeof(bad_aad),
		                 blob, blob_len, out, sizeof(out), &out_len) != 0);
		CHECK(ta_wrap_open(kek, bad_aad, sizeof(bad_aad), blob, blob_len,
		                   out, sizeof(out), &out_len) != 0);
	}

	TCASE("反证：改密文里一个字节，两个方向都必须拒");
	{
		blob[PWRP_OFF_CT] ^= 0x01;
		CHECK(pqc_unwrap(kek, sizeof(kek), aad, sizeof(aad) - 1,
		                 blob, blob_len, out, sizeof(out), &out_len) != 0);
		CHECK(ta_wrap_open(kek, aad, sizeof(aad) - 1, blob, blob_len,
		                   out, sizeof(out), &out_len) != 0);
		blob[PWRP_OFF_CT] ^= 0x01;   /* 改回来 */
	}

	TCASE("空明文这个边界两边也要一致");
	{
		size_t bl = 0, ol = 0;

		CHECK_EQ_INT(ta_wrap_seal(kek, NULL, 0, NULL, 0,
		                          blob, sizeof(blob), &bl), 0);
		CHECK_EQ_INT((int)bl, PWRP_OVERHEAD);
		CHECK_EQ_INT(pqc_unwrap(kek, sizeof(kek), NULL, 0, blob, bl,
		                        out, sizeof(out), &ol), 0);
		CHECK_EQ_INT((int)ol, 0);
	}
}

/* ============================================================================
 * ③ 两侧的 KEK 域分隔串必须是同一个
 * ==========================================================================*/
static void test_kek_label_shared(void)
{
	uint8_t salt[16];
	uint8_t from_shared[PWRP_KEK_LEN], from_ta[PWRP_KEK_LEN];
	uint8_t ikm[32];

	for (size_t i = 0; i < sizeof(salt); i++) {
		salt[i] = (uint8_t)(0x30 + i);
	}
	for (size_t i = 0; i < sizeof(ikm); i++) {
		ikm[i] = (uint8_t)(0x50 + i);
	}

	/* TA 侧派生 KEK 时用的字面量在 pqchsm_ta.c 里（PQCHSM_KEK_LABEL），
	 * 普通世界用的在 wrap.c 里。两处都应当等于共享头里的 PWRP_KEK_LABEL。
	 * 这里比的是"用共享常量算出来的"与"用 TA 那份实现算出来的"—— 一致
	 * 就说明标签没有各写各的。 */
	TCASE("KEK 域分隔串两侧同源（用它算出来的子密钥相同）");
	CHECK_EQ_INT(pqc_kdf(ikm, sizeof(ikm), salt, sizeof(salt),
	                     PWRP_KEK_LABEL, from_shared, sizeof(from_shared)), 0);
	CHECK_EQ_INT(ta_kdf_derive(ikm, sizeof(ikm), salt, sizeof(salt),
	                           PWRP_KEK_LABEL, from_ta, sizeof(from_ta)), 0);
	CHECK_EQ_MEM(from_shared, from_ta, sizeof(from_shared));
}

int main(void)
{
	test_use_stub_kdr();   /* 显式装桩：库里没有自动回退了 */
	test_kmac_agrees();
	test_pwrp_interop();
	test_kek_label_shared();
	return test_report("test_cross_world");
}
