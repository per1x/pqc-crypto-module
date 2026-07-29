#include "pqchsm/wrap.h"

#include "pqchsm/kdr.h"
#include "pqchsm/util.h"
#include "wrap_internal.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <string.h>

static const uint8_t WRAP_MAGIC[4] = { 'P', 'W', 'R', 'P' };

size_t pqc_wrap_blob_len(size_t pt_len)
{
	return pt_len + PQC_WRAP_OVERHEAD;
}

int pqc_kek_derive(const uint8_t *salt, size_t salt_len, uint8_t kek[PQC_KEK_LEN])
{
	if (!kek) {
		return -1;
	}
	/* ：KEK 由 KDR 现场派生，不落盘。域分隔串固定为 "storage"。 */
	return pqc_kdr_derive("pqc-hsm/storage-kek", salt, salt_len, kek, PQC_KEK_LEN);
}

static void put_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++) {
		p[i] = (uint8_t)(v >> (8 * i));
	}
}

static uint16_t get_u16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
	uint32_t v = 0;
	for (int i = 0; i < 4; i++) {
		v |= (uint32_t)p[i] << (8 * i);
	}
	return v;
}

int pqc_wrap_with_nonce(const uint8_t *kek, size_t kek_len,
                        const uint8_t *aad, size_t aad_len,
                        const uint8_t *pt, size_t pt_len,
                        const uint8_t nonce[PQC_WRAP_NONCE_LEN],
                        uint8_t *blob, size_t cap, size_t *blob_len)
{
	if (!kek || kek_len != PQC_KEK_LEN || !blob || !blob_len || !nonce) {
		return -1;
	}
	if ((!pt && pt_len) || (!aad && aad_len)) {
		return -1;
	}
	if (pt_len > 0x7fffffffu || aad_len > 0x7fffffffu) {
		return -1;
	}
	size_t need = pqc_wrap_blob_len(pt_len);
	if (cap < need) {
		return -1;
	}

	memcpy(blob, WRAP_MAGIC, 4);
	put_u16(blob + 4, PQC_WRAP_VERSION);
	put_u16(blob + 6, PQC_WRAP_ALG_AES256GCM);
	put_u32(blob + 8, (uint32_t)aad_len);
	put_u32(blob + 12, (uint32_t)pt_len);
	memcpy(blob + PQC_WRAP_HDR_LEN, nonce, PQC_WRAP_NONCE_LEN);

	uint8_t *ct  = blob + PQC_WRAP_HDR_LEN + PQC_WRAP_NONCE_LEN;
	uint8_t *tag = ct + pt_len;

	EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
	int rc = -1, len = 0;
	if (!c) {
		goto out;
	}
	if (EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) {
		goto out;
	}
	if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, PQC_WRAP_NONCE_LEN, NULL) != 1) {
		goto out;
	}
	if (EVP_EncryptInit_ex(c, NULL, NULL, kek, nonce) != 1) {
		goto out;
	}
	/* AAD = 头部 16 字节 ‖ 调用方元数据 */
	if (EVP_EncryptUpdate(c, NULL, &len, blob, PQC_WRAP_HDR_LEN) != 1) {
		goto out;
	}
	if (aad_len && EVP_EncryptUpdate(c, NULL, &len, aad, (int)aad_len) != 1) {
		goto out;
	}
	if (pt_len && EVP_EncryptUpdate(c, ct, &len, pt, (int)pt_len) != 1) {
		goto out;
	}
	if (EVP_EncryptFinal_ex(c, ct + len, &len) != 1) {
		goto out;
	}
	if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, PQC_WRAP_TAG_LEN, tag) != 1) {
		goto out;
	}
	*blob_len = need;
	rc = 0;
out:
	EVP_CIPHER_CTX_free(c);
	return rc;
}

int pqc_wrap(const uint8_t *kek, size_t kek_len,
             const uint8_t *aad, size_t aad_len,
             const uint8_t *pt, size_t pt_len,
             uint8_t *blob, size_t cap, size_t *blob_len)
{
	uint8_t nonce[PQC_WRAP_NONCE_LEN];
	/* SP 800-38D RBG 构造：每次现取，无状态 */
	if (RAND_bytes(nonce, PQC_WRAP_NONCE_LEN) != 1) {
		return -1;
	}
	int rc = pqc_wrap_with_nonce(kek, kek_len, aad, aad_len, pt, pt_len, nonce,
	                             blob, cap, blob_len);
	pqc_secure_zero(nonce, sizeof(nonce));
	return rc;
}

int pqc_unwrap(const uint8_t *kek, size_t kek_len,
               const uint8_t *aad, size_t aad_len,
               const uint8_t *blob, size_t blob_len,
               uint8_t *pt, size_t cap, size_t *pt_len)
{
	if (!kek || kek_len != PQC_KEK_LEN || !blob || !pt_len) {
		return -1;
	}
	if ((!aad && aad_len) || blob_len < PQC_WRAP_OVERHEAD) {
		return -1;
	}
	if (memcmp(blob, WRAP_MAGIC, 4) != 0) {
		return -1;
	}
	if (get_u16(blob + 4) != PQC_WRAP_VERSION ||
	    get_u16(blob + 6) != PQC_WRAP_ALG_AES256GCM) {
		return -1;
	}
	/* 头里声明的 aad_len 必须与调用方给的一致 —— 否则就是拿别处的元数据来配 */
	if (get_u32(blob + 8) != (uint32_t)aad_len) {
		return -1;
	}
	size_t ct_len = get_u32(blob + 12);
	if (ct_len != blob_len - PQC_WRAP_OVERHEAD) {
		return -1;
	}
	if (ct_len && (!pt || cap < ct_len)) {
		return -1;
	}

	const uint8_t *nonce = blob + PQC_WRAP_HDR_LEN;
	const uint8_t *ct    = nonce + PQC_WRAP_NONCE_LEN;
	const uint8_t *tag   = ct + ct_len;

	EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
	int rc = -1, len = 0;
	if (!c) {
		goto out;
	}
	if (EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) {
		goto out;
	}
	if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, PQC_WRAP_NONCE_LEN, NULL) != 1) {
		goto out;
	}
	if (EVP_DecryptInit_ex(c, NULL, NULL, kek, nonce) != 1) {
		goto out;
	}
	if (EVP_DecryptUpdate(c, NULL, &len, blob, PQC_WRAP_HDR_LEN) != 1) {
		goto out;
	}
	if (aad_len && EVP_DecryptUpdate(c, NULL, &len, aad, (int)aad_len) != 1) {
		goto out;
	}
	if (ct_len && EVP_DecryptUpdate(c, pt, &len, ct, (int)ct_len) != 1) {
		goto out;
	}
	if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, PQC_WRAP_TAG_LEN,
	                        (void *)(uintptr_t)tag) != 1) {
		goto out;
	}
	/* Final 返回 <=0 即 tag 校验失败 */
	if (EVP_DecryptFinal_ex(c, ct_len ? pt + len : NULL, &len) != 1) {
		goto out;
	}
	*pt_len = ct_len;
	rc = 0;
out:
	EVP_CIPHER_CTX_free(c);
	if (rc != 0 && pt && cap) {
		/* 认证失败 → 明文一律作废，绝不把未认证数据交给调用方 */
		pqc_secure_zero(pt, cap);
	}
	return rc;
}
