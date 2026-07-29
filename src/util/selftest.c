/* selftest.c —— 上电自测的已知答案测试
 *
 * 【期望值的来源】每一条都来自模块之外，没有一条是从本模块自己的输出反推的：
 *
 *   SHA3-256     FIPS 202 的公开示例：SHA3-256("abc")
 *   KMAC256      SP 800-185 的官方示例 Sample #4（CSRC 的 KMAC_samples.pdf）
 *   AES-256-GCM  GCM 规范（McGrew & Viega）的公开测试用例 14：
 *                密钥、IV、明文全零，128 位标签
 *   ML-KEM-768   NIST ACVP ML-KEM-keyGen-FIPS203 的一条向量，
 *                比对 SHA3-256(ek‖dk) 而不是全文，以便把期望值压成 32 字节
 *   ML-DSA-65    NIST ACVP ML-DSA-keyGen-FIPS204 的一条向量，同样比对摘要
 *
 * 对公钥私钥比对摘要而不是全文，是为了不在源码里塞进几千字节常量；
 * 摘要由 SHA3-256 计算，而 SHA3-256 自身在更早的一项里已经先被验证过。
 * 两项的先后顺序因此是有意的，不能调换。
 *
 * 【错误状态】任一项失败即置错误状态，pqc.c 的每个入口都会拒绝服务。
 * 自测本身不受该状态阻断，否则一旦进入错误状态就再也出不来。
 */
#include "pqchsm/selftest.h"

#include "pqchsm/kdf.h"
#include "pqchsm/pqc.h"
#include "pqchsm/util.h"

#include <openssl/evp.h>

#include <stdlib.h>
#include <string.h>

/* ---- 期望值 ---- */

/* FIPS 202：SHA3-256("abc") */
static const uint8_t SHA3_MSG[3] = { 'a', 'b', 'c' };
static const uint8_t SHA3_EXP[32] = {
	0x3a, 0x98, 0x5d, 0xa7, 0x4f, 0xe2, 0x25, 0xb2, 0x04, 0x5c, 0x17, 0x2d,
	0x6b, 0xd3, 0x90, 0xbd, 0x85, 0x5f, 0x08, 0x6e, 0x3e, 0x9d, 0x52, 0x5b,
	0x46, 0xbf, 0xe2, 0x45, 0x11, 0x43, 0x15, 0x32,
};

/* SP 800-185 Sample #4：Key = 0x40..0x5F，Data = 00 01 02 03，
 * S = "My Tagged Application"，输出 512 位 */
static const uint8_t KMAC_KEY[32] = {
	0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B,
	0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
	0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F,
};
static const uint8_t KMAC_DATA[4] = { 0x00, 0x01, 0x02, 0x03 };
static const uint8_t KMAC_EXP[64] = {
	0x20, 0xC5, 0x70, 0xC3, 0x13, 0x46, 0xF7, 0x03, 0xC9, 0xAC, 0x36, 0xC6,
	0x1C, 0x03, 0xCB, 0x64, 0xC3, 0x97, 0x0D, 0x0C, 0xFC, 0x78, 0x7E, 0x9B,
	0x79, 0x59, 0x9D, 0x27, 0x3A, 0x68, 0xD2, 0xF7, 0xF6, 0x9D, 0x4C, 0xC3,
	0xDE, 0x9D, 0x10, 0x4A, 0x35, 0x16, 0x89, 0xF2, 0x7C, 0xF6, 0xF5, 0x95,
	0x1F, 0x01, 0x03, 0xF3, 0x3F, 0x4F, 0x24, 0x87, 0x10, 0x24, 0xD9, 0xC2,
	0x77, 0x73, 0xA8, 0xDD,
};

/* GCM 规范测试用例 14：AES-256，密钥/IV/明文全零，无 AAD */
static const uint8_t GCM_EXP_CT[16] = {
	0xce, 0xa7, 0x40, 0x3d, 0x4d, 0x60, 0x6b, 0x6e,
	0x07, 0x4e, 0xc5, 0xd3, 0xba, 0xf3, 0x9d, 0x18,
};
static const uint8_t GCM_EXP_TAG[16] = {
	0xd0, 0xd1, 0xc8, 0xa7, 0x99, 0x99, 0x6b, 0xf0,
	0x26, 0x5b, 0x98, 0xb5, 0xd4, 0x8a, 0xb9, 0x19,
};

/* NIST ACVP ML-KEM-keyGen-FIPS203，ML-KEM-768，tcId 26 */
static const uint8_t KEM_SEED[64] = {
	0xE5, 0x82, 0xB7, 0xD7, 0x5E, 0x6C, 0x80, 0xB0, 0x5A, 0xE3, 0x92, 0xA1,
	0xFC, 0x9F, 0x71, 0x53, 0xB1, 0x23, 0x90, 0xFD, 0x99, 0x93, 0x03, 0x68,
	0xCC, 0x67, 0xA7, 0x68, 0xBA, 0xEB, 0xC8, 0xA0, 0x1C, 0xDA, 0xCB, 0x87,
	0x40, 0xC0, 0xB8, 0x7C, 0x4A, 0x37, 0x95, 0x75, 0xF1, 0x87, 0xB3, 0x67,
	0xCB, 0xFA, 0x3B, 0x30, 0x0B, 0xF5, 0x91, 0xB1, 0x09, 0xF7, 0x98, 0x16,
	0xE9, 0xCB, 0xE8, 0xF0,
};
static const uint8_t KEM_EXP_DIGEST[32] = {
	0xed, 0x71, 0x08, 0xe5, 0x24, 0xc6, 0xcc, 0xbd, 0xb5, 0xe4, 0xb8, 0x56,
	0x35, 0x1c, 0x64, 0x69, 0x1c, 0x33, 0xc7, 0x07, 0x8f, 0x13, 0x85, 0xb0,
	0x80, 0xb6, 0x4f, 0x45, 0x07, 0x44, 0x2d, 0x70,
};

/* NIST ACVP ML-DSA-keyGen-FIPS204，ML-DSA-65，tcId 26 */
static const uint8_t DSA_SEED[32] = {
	0xA9, 0x91, 0xFD, 0x42, 0xB0, 0x71, 0xD4, 0x9C, 0x48, 0xAE, 0x3E, 0x75,
	0xC6, 0x47, 0x45, 0x9E, 0x0D, 0xAA, 0xD1, 0xE1, 0xBA, 0x35, 0x6A, 0x04,
	0x80, 0x19, 0x12, 0xD3, 0x29, 0x4B, 0xCF, 0xF8,
};
static const uint8_t DSA_EXP_DIGEST[32] = {
	0x6d, 0x3c, 0x24, 0xfa, 0xbc, 0xeb, 0x65, 0x7d, 0xa3, 0x93, 0x67, 0x11,
	0xf6, 0x75, 0xc1, 0x77, 0x0b, 0xf0, 0x87, 0x1c, 0xb6, 0x23, 0x48, 0x2b,
	0x33, 0xe4, 0x63, 0xfc, 0x92, 0xfd, 0x90, 0x58,
};

/* ---- 状态 ---- */

static int      g_passed;
static int      g_running;
static uint32_t g_failures = 0xFFFFFFFFu;   /* 未跑过之前视为"未通过" */

const char *pqc_selftest_name(pqc_selftest_id_t id)
{
	switch (id) {
	case PQC_ST_SHA3:    return "SHA3-256";
	case PQC_ST_KMAC:    return "KMAC256";
	case PQC_ST_AES_GCM: return "AES-256-GCM";
	case PQC_ST_ML_KEM:  return "ML-KEM-768";
	case PQC_ST_ML_DSA:  return "ML-DSA-65";
	default:             return "unknown";
	}
}

/* ---- 各项测试 ---- */

static int st_sha3(void)
{
	uint8_t out[32];
	if (pqc_sha3_256(SHA3_MSG, sizeof(SHA3_MSG), out) != 0) {
		return -1;
	}
	return pqc_ct_equal(out, SHA3_EXP, sizeof(out)) ? 0 : -1;
}

static int st_kmac(void)
{
	uint8_t out[64];
	if (pqc_kmac256(KMAC_KEY, sizeof(KMAC_KEY), KMAC_DATA, sizeof(KMAC_DATA),
	                "My Tagged Application", out, sizeof(out)) != 0) {
		return -1;
	}
	return pqc_ct_equal(out, KMAC_EXP, sizeof(out)) ? 0 : -1;
}

static int st_aes_gcm(void)
{
	uint8_t key[32] = { 0 }, iv[12] = { 0 }, pt[16] = { 0 };
	uint8_t ct[16], tag[16];
	int len = 0;
	int ok = 0;

	EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
	if (!c) {
		return -1;
	}
	if (EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1
	    && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, (int)sizeof(iv), NULL) == 1
	    && EVP_EncryptInit_ex(c, NULL, NULL, key, iv) == 1
	    && EVP_EncryptUpdate(c, ct, &len, pt, (int)sizeof(pt)) == 1
	    && EVP_EncryptFinal_ex(c, ct + len, &len) == 1
	    && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, (int)sizeof(tag), tag) == 1) {
		ok = pqc_ct_equal(ct, GCM_EXP_CT, sizeof(ct))
		  && pqc_ct_equal(tag, GCM_EXP_TAG, sizeof(tag));
	}
	EVP_CIPHER_CTX_free(c);
	/* 无需清零：密钥、IV、明文、密文与标签全部来自公开的 GCM 测试用例，
	 * 是编译进二进制的常量，本来就不是秘密 */
	return ok ? 0 : -1;
}

/* 密钥对生成的已知答案：比对 SHA3-256(公钥‖私钥) */
static int st_keypair(pqc_alg_t alg, const uint8_t *seed, size_t seed_len,
                      const uint8_t *want_digest)
{
	const pqc_alg_info_t *info = pqc_alg_info(alg);
	const pqc_backend_t  *be   = pqc_backend_liboqs();
	if (!info || !be || !be->keypair_from_seed || seed_len != info->seed_len) {
		return -1;
	}

	size_t total = info->pk_len + info->sk_len;
	uint8_t *buf = malloc(total);
	if (!buf) {
		return -1;
	}
	int rc = -1;
	if (be->keypair_from_seed(alg, seed, seed_len, buf, buf + info->pk_len) == PQC_OK) {
		uint8_t digest[32];
		if (pqc_sha3_256(buf, total, digest) == 0
		    && pqc_ct_equal(digest, want_digest, sizeof(digest))) {
			rc = 0;
		}
		pqc_secure_zero(digest, sizeof(digest));
	}
	pqc_secure_zero(buf, total);
	free(buf);
	return rc;
}

/* ---- 对外接口 ---- */

uint32_t pqc_self_test(void)
{
	uint32_t fails = 0;

	g_running = 1;
	if (st_sha3() != 0) {
		fails |= 1u << PQC_ST_SHA3;
	}
	if (st_kmac() != 0) {
		fails |= 1u << PQC_ST_KMAC;
	}
	if (st_aes_gcm() != 0) {
		fails |= 1u << PQC_ST_AES_GCM;
	}
	/* 后两项用 SHA3-256 压缩期望值，因此必须在 SHA3 那项之后跑；
	 * SHA3 本身失败时它们的结论不可信，直接一并记为失败。 */
	if ((fails & (1u << PQC_ST_SHA3))
	    || st_keypair(PQC_ALG_ML_KEM_768, KEM_SEED, sizeof(KEM_SEED),
	                  KEM_EXP_DIGEST) != 0) {
		fails |= 1u << PQC_ST_ML_KEM;
	}
	if ((fails & (1u << PQC_ST_SHA3))
	    || st_keypair(PQC_ALG_ML_DSA_65, DSA_SEED, sizeof(DSA_SEED),
	                  DSA_EXP_DIGEST) != 0) {
		fails |= 1u << PQC_ST_ML_DSA;
	}
	g_running = 0;

	g_failures = fails;
	g_passed = (fails == 0);
	return fails;
}

int pqc_self_test_passed(void)
{
	if (g_running) {
		/* 自测过程中不阻断自身，否则进入错误状态后无法恢复 */
		return 1;
	}
	if (!g_passed && g_failures == 0xFFFFFFFFu) {
		pqc_self_test();
	}
	return g_passed;
}

uint32_t pqc_self_test_failures(void)
{
	return g_failures;
}

void pqc_self_test_force_error(int on)
{
	if (on) {
		g_passed = 0;
		g_failures = 1u << PQC_ST__COUNT;   /* 与任何真实项都不重合 */
	} else {
		g_failures = 0xFFFFFFFFu;
		g_passed = 0;
		pqc_self_test();
	}
}
