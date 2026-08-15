/* sdfe_pkenc —— KEM-DEM 实现（见 sdfe_pkenc.h 的边界说明）*/
#define _GNU_SOURCE
#include <string.h>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include "sdfe_pkenc.h"

/* ML-KEM 密文长度（与 pset 一一对应，用来切分 blob 与预留缓冲）*/
static uint32_t ct_len_of(uint32_t pset)
{
	switch (pset) {
	case SDFE_MLKEM_512:  return 768;
	case SDFE_MLKEM_768:  return 1088;
	case SDFE_MLKEM_1024: return 1568;
	default:              return 0;
	}
}

static void wr32(uint8_t *p, uint32_t v)   /* 大端 */
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint32_t rd32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
	     | ((uint32_t)p[2] << 8)  | p[3];
}

/* AES-256-GCM 加密。ss 必须 32 字节。out 写 ciphertext，tag 单独写。 */
static int gcm_seal(const uint8_t *ss, const uint8_t *iv,
                    const uint8_t *pt, int pt_len,
                    uint8_t *ct, uint8_t *tag)
{
	EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
	int len = 0, ok = 0;

	if (!c)
		return -1;
	if (EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto out;
	if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, SDFE_PKENC_IVLEN, NULL) != 1) goto out;
	if (EVP_EncryptInit_ex(c, NULL, NULL, ss, iv) != 1) goto out;
	if (EVP_EncryptUpdate(c, ct, &len, pt, pt_len) != 1) goto out;
	if (EVP_EncryptFinal_ex(c, ct + len, &len) != 1) goto out;   /* GCM: len=0 */
	if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, SDFE_PKENC_TAGLEN, tag) != 1) goto out;
	ok = 1;
out:
	EVP_CIPHER_CTX_free(c);
	return ok ? 0 : -1;
}

/* AES-256-GCM 解密并验证 tag。tag 不对返回 -1，且不输出明文。 */
static int gcm_open(const uint8_t *ss, const uint8_t *iv,
                    const uint8_t *ct, int ct_len,
                    const uint8_t *tag, uint8_t *pt)
{
	EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
	int len = 0, ok = 0;

	if (!c)
		return -1;
	if (EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto out;
	if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, SDFE_PKENC_IVLEN, NULL) != 1) goto out;
	if (EVP_DecryptInit_ex(c, NULL, NULL, ss, iv) != 1) goto out;
	if (EVP_DecryptUpdate(c, pt, &len, ct, ct_len) != 1) goto out;
	if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, SDFE_PKENC_TAGLEN,
	                        (void *)tag) != 1) goto out;
	/* Final 在此做 tag 校验：不通过返回 0，明文作废 */
	if (EVP_DecryptFinal_ex(c, pt + len, &len) != 1) goto out;
	ok = 1;
out:
	EVP_CIPHER_CTX_free(c);
	return ok ? 0 : -1;
}

int SDFE_PKEncrypt(SDFE_HANDLE hSession, uint32_t pset,
                   const uint8_t *ek, uint32_t ek_len,
                   const uint8_t *data, uint32_t data_len,
                   uint8_t *out, uint32_t *out_len)
{
	uint32_t ctl = ct_len_of(pset);
	uint8_t ss[32], ct[1600], iv[SDFE_PKENC_IVLEN];   /* tag 直接写进 out */
	uint32_t ss_len = sizeof ss, real_ct = sizeof ct;
	int rv;

	if (!ek || !data || !out || !out_len || ctl == 0)
		return SDR_INARGERR;

	/* KEM：只用公钥，走硬件 */
	rv = SDFE_Encapsulate_MLKEM(hSession, pset, ek, ek_len,
	                            ss, &ss_len, ct, &real_ct);
	if (rv != SDR_OK)
		return rv;
	if (ss_len != 32 || real_ct != ctl)
		return SDR_UNKNOWERR;

	/* DEM：随机 iv + AES-256-GCM（软件），见头文件的诚实口径 */
	if (RAND_bytes(iv, sizeof iv) != 1)
		return SDR_UNKNOWERR;

	{
		uint8_t *p = out;
		wr32(p, SDFE_PKENC_MAGIC); p += 4;
		wr32(p, ctl);              p += 4;
		memcpy(p, ct, ctl);        p += ctl;
		memcpy(p, iv, SDFE_PKENC_IVLEN); p += SDFE_PKENC_IVLEN;
		if (gcm_seal(ss, iv, data, (int)data_len, p + SDFE_PKENC_TAGLEN, p) != 0)
			return SDR_UNKNOWERR;      /* tag 写在 ciphertext 之前 */
		*out_len = (uint32_t)(p - out) + SDFE_PKENC_TAGLEN + data_len;
	}
	memset(ss, 0, sizeof ss);
	return SDR_OK;
}

int SDFE_PKDecrypt(SDFE_HANDLE hSession, uint32_t key_handle,
                   const uint8_t *blob, uint32_t blob_len,
                   uint8_t *data, uint32_t *data_len)
{
	uint8_t ss[32];
	uint32_t ss_len = sizeof ss, ctl;
	const uint8_t *ct, *iv, *tag, *ciph;
	uint32_t ciph_len;
	int rv;

	if (!blob || !data || !data_len || blob_len < 8)
		return SDR_INARGERR;
	if (rd32(blob) != SDFE_PKENC_MAGIC)
		return SDR_INARGERR;
	ctl = rd32(blob + 4);
	if (blob_len < 8 + ctl + SDFE_PKENC_IVLEN + SDFE_PKENC_TAGLEN)
		return SDR_INARGERR;

	ct   = blob + 8;
	iv   = ct + ctl;
	tag  = iv + SDFE_PKENC_IVLEN;
	ciph = tag + SDFE_PKENC_TAGLEN;
	ciph_len = blob_len - (8 + ctl + SDFE_PKENC_IVLEN + SDFE_PKENC_TAGLEN);

	/* KEM：私钥在硬件片内金库，按句柄 decaps */
	rv = SDFE_Decapsulate_MLKEM(hSession, key_handle, ct, ctl, ss, &ss_len);
	if (rv != SDR_OK)
		return rv;
	if (ss_len != 32)
		return SDR_UNKNOWERR;

	/* DEM：GCM 解密 + 认证。tag 不对 → 拒绝，不给明文 */
	if (gcm_open(ss, iv, ciph, (int)ciph_len, tag, data) != 0) {
		memset(ss, 0, sizeof ss);
		return SDR_HARDFAIL;   /* 认证失败 */
	}
	*data_len = ciph_len;
	memset(ss, 0, sizeof ss);
	return SDR_OK;
}
