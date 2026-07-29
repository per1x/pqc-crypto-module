#include "testlib.h"
#include "pqchsm/kdf.h"
#include "pqchsm/util.h"

/* NIST SP 800-185 官方示例，取自 CSRC 的 KMAC_samples.pdf
 * （Cryptographic Standards and Guidelines / Examples with Intermediate Values）。
 * 三条都是 KMAC256、512-bit 输出、Key = 0x40..0x5F。 */
#define NIST_KEY_HEX \
	"404142434445464748494A4B4C4D4E4F505152535455565758595A5B5C5D5E5F"

/* Sample #4：data = 00 01 02 03，S = "My Tagged Application" */
#define NIST_S4_OUT_HEX \
	"20C570C31346F703C9AC36C61C03CB64C3970D0CFC787E9B79599D273A68D2F7" \
	"F69D4CC3DE9D104A351689F27CF6F5951F0103F33F4F24871024D9C27773A8DD"

/* Sample #5：data = 0x00..0xC7（200 字节），S = ""（PDF 里写作 "(null)"） */
#define NIST_S5_OUT_HEX \
	"75358CF39E41494E949707927CEE0AF20A3FF553904C86B08F21CC414BCFD691" \
	"589D27CF5E15369CBBFF8B9A4C2EB17800855D0235FF635DA82533EC6B759B69"

/* Sample #6：data = 0x00..0xC7（200 字节），S = "My Tagged Application" */
#define NIST_S6_OUT_HEX \
	"B58618F71F92E1D56C1B8C55DDD7CD188B97B4CA4D99831EB2699A837DA2E4D9" \
	"70FBACFDE50033AEA585F1A2708510C32D07880801BD182898FE476876FC8965"

/* SHA3-256("")，FIPS 202 已知值 */
#define SHA3_EMPTY_HEX \
	"a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a"

static void expect_hex(const char *hex, uint8_t *out, size_t n)
{
	long got = pqc_hex_decode(hex, (size_t)-1, out, n);
	CHECK_EQ_INT(got, (long)n);
}

int main(void)
{
	uint8_t key[32];
	uint8_t data200[200];
	uint8_t small_data[4] = { 0x00, 0x01, 0x02, 0x03 };
	uint8_t expect[64];
	uint8_t got[64];
	uint8_t got32[32];
	uint8_t alt[64];

	expect_hex(NIST_KEY_HEX, key, sizeof(key));
	for (size_t i = 0; i < sizeof(data200); i++) {
		data200[i] = (uint8_t)i;      /* 0x00 .. 0xC7 */
	}

	TCASE("NIST KMAC256 Sample #4");
	expect_hex(NIST_S4_OUT_HEX, expect, 64);
	CHECK_EQ_INT(pqc_kmac256(key, sizeof(key), small_data, sizeof(small_data),
	                         "My Tagged Application", got, 64), 0);
	CHECK_EQ_MEM(got, expect, 64);

	TCASE("NIST KMAC256 Sample #5（空 custom）");
	expect_hex(NIST_S5_OUT_HEX, expect, 64);
	CHECK_EQ_INT(pqc_kmac256(key, sizeof(key), data200, sizeof(data200),
	                         "", got, 64), 0);
	CHECK_EQ_MEM(got, expect, 64);

	/* custom = NULL 与 custom = "" 在 SP 800-185 里是同一件事 */
	TCASE("custom=NULL 等价于 custom=\"\"");
	CHECK_EQ_INT(pqc_kmac256(key, sizeof(key), data200, sizeof(data200),
	                         NULL, alt, 64), 0);
	CHECK_EQ_MEM(alt, expect, 64);

	TCASE("NIST KMAC256 Sample #6");
	expect_hex(NIST_S6_OUT_HEX, expect, 64);
	CHECK_EQ_INT(pqc_kmac256(key, sizeof(key), data200, sizeof(data200),
	                         "My Tagged Application", got, 64), 0);
	CHECK_EQ_MEM(got, expect, 64);

	TCASE("确定性：相同输入相同输出");
	CHECK_EQ_INT(pqc_kmac256(key, sizeof(key), data200, sizeof(data200),
	                         "My Tagged Application", alt, 64), 0);
	CHECK_EQ_MEM(alt, got, 64);

	TCASE("改 key 输出必变");
	{
		uint8_t k2[32];
		memcpy(k2, key, sizeof(k2));
		k2[0] ^= 0x01;
		CHECK_EQ_INT(pqc_kmac256(k2, sizeof(k2), data200, sizeof(data200),
		                         "My Tagged Application", alt, 64), 0);
		CHECK(memcmp(alt, got, 64) != 0);
	}

	TCASE("改 data 输出必变");
	{
		uint8_t d2[200];
		memcpy(d2, data200, sizeof(d2));
		d2[199] ^= 0x01;
		CHECK_EQ_INT(pqc_kmac256(key, sizeof(key), d2, sizeof(d2),
		                         "My Tagged Application", alt, 64), 0);
		CHECK(memcmp(alt, got, 64) != 0);
	}

	/* 这条是域分隔真正生效的证据：只改 custom，其余全同 */
	TCASE("改 custom 输出必变（域分隔生效）");
	CHECK_EQ_INT(pqc_kmac256(key, sizeof(key), data200, sizeof(data200),
	                         "My Tagged Applicatiom", alt, 64), 0);
	CHECK(memcmp(alt, got, 64) != 0);
	CHECK_EQ_INT(pqc_kmac256(key, sizeof(key), data200, sizeof(data200),
	                         "storage", alt, 64), 0);
	CHECK(memcmp(alt, got, 64) != 0);

	/* out_len 进 KMAC 的编码，所以短输出不是长输出的前缀 —— 这点跟
	 * HMAC 截断的直觉相反，实现里漏设 OSSL_MAC_PARAM_SIZE 就会在这里露馅。 */
	TCASE("不同 out_len 不是截断关系");
	CHECK_EQ_INT(pqc_kmac256(key, sizeof(key), data200, sizeof(data200),
	                         "My Tagged Application", got32, 32), 0);
	CHECK(memcmp(got32, got, 32) != 0);

	TCASE("KMAC 非法参数");
	CHECK_EQ_INT(pqc_kmac256(NULL, 32, small_data, 4, NULL, got, 32), -1);
	CHECK_EQ_INT(pqc_kmac256(key, 0, small_data, 4, NULL, got, 32), -1);
	CHECK_EQ_INT(pqc_kmac256(key, sizeof(key), NULL, 4, NULL, got, 32), -1);
	CHECK_EQ_INT(pqc_kmac256(key, sizeof(key), small_data, 4, NULL, NULL, 32), -1);
	CHECK_EQ_INT(pqc_kmac256(key, sizeof(key), small_data, 4, NULL, got, 0), -1);

	TCASE("data_len == 0 且 data == NULL 是合法的");
	CHECK_EQ_INT(pqc_kmac256(key, sizeof(key), NULL, 0, "empty", got, 32), 0);
	CHECK_EQ_INT(pqc_kmac256(key, sizeof(key), (const uint8_t *)"", 0, "empty", alt, 32), 0);
	CHECK_EQ_MEM(alt, got, 32);

	TCASE("SHA3-256 空输入已知值");
	expect_hex(SHA3_EMPTY_HEX, expect, 32);
	CHECK_EQ_INT(pqc_sha3_256(NULL, 0, got32), 0);
	CHECK_EQ_MEM(got32, expect, 32);
	CHECK_EQ_INT(pqc_sha3_256((const uint8_t *)"", 0, alt), 0);
	CHECK_EQ_MEM(alt, expect, 32);

	TCASE("SHA3-256 非空输入确定且不同");
	CHECK_EQ_INT(pqc_sha3_256(small_data, sizeof(small_data), got32), 0);
	CHECK(memcmp(got32, expect, 32) != 0);
	CHECK_EQ_INT(pqc_sha3_256(small_data, sizeof(small_data), alt), 0);
	CHECK_EQ_MEM(alt, got32, 32);

	TCASE("SHA3-256 非法参数");
	CHECK_EQ_INT(pqc_sha3_256(small_data, 4, NULL), -1);
	CHECK_EQ_INT(pqc_sha3_256(NULL, 4, got32), -1);

	/* §8.1：KEK = KMAC256(KDR, 盐, custom="storage") */
	TCASE("pqc_kdf 等价于带 label 的 KMAC256");
	{
		uint8_t salt[16];
		for (size_t i = 0; i < sizeof(salt); i++) {
			salt[i] = (uint8_t)(0xF0 ^ i);
		}
		CHECK_EQ_INT(pqc_kdf(key, sizeof(key), salt, sizeof(salt), "storage", got32, 32), 0);
		CHECK_EQ_INT(pqc_kmac256(key, sizeof(key), salt, sizeof(salt), "storage", alt, 32), 0);
		CHECK_EQ_MEM(got32, alt, 32);

		TCASE("pqc_kdf 换 label 必须派生出不同密钥");
		CHECK_EQ_INT(pqc_kdf(key, sizeof(key), salt, sizeof(salt), "backup", alt, 32), 0);
		CHECK(memcmp(got32, alt, 32) != 0);

		TCASE("pqc_kdf 换盐必须派生出不同密钥");
		salt[0] ^= 0x01;
		CHECK_EQ_INT(pqc_kdf(key, sizeof(key), salt, sizeof(salt), "storage", alt, 32), 0);
		CHECK(memcmp(got32, alt, 32) != 0);
	}

	TCASE("pqc_kdf 的 label 不可为 NULL");
	CHECK_EQ_INT(pqc_kdf(key, sizeof(key), NULL, 0, NULL, got32, 32), -1);
	CHECK_EQ_INT(pqc_kdf(key, sizeof(key), small_data, 4, NULL, got32, 32), -1);

	TCASE("pqc_kdf 其余非法参数");
	CHECK_EQ_INT(pqc_kdf(NULL, 32, small_data, 4, "storage", got32, 32), -1);
	CHECK_EQ_INT(pqc_kdf(key, sizeof(key), small_data, 4, "storage", NULL, 32), -1);
	CHECK_EQ_INT(pqc_kdf(key, sizeof(key), small_data, 4, "storage", got32, 0), -1);

	return test_report("test_kdf");
}
