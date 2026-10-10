#include "ta_wrap.h"

#include <string.h>

#include "ta_fips202.h"
#include "ta_random.h"

#if defined(PQCHSM_TA_OPTEE)
#include <tee_api.h>
#else
#include <openssl/evp.h>
#endif

/* magic / 版本 / 字段偏移 / 头部组装全部来自 pqchsm/pwrp_format.h
 * （ta_wrap.h 已经 include 它）—— 这里一个格式常量都不许再定义。
 * 本文件只剩"这一侧用哪个 AES-GCM 后端"这一件事。 */

size_t ta_wrap_blob_len(size_t pt_len)
{
	return pt_len + TA_WRAP_OVERHEAD;
}

/* ---- AES-256-GCM 后端 -------------------------------------------------- */

#if defined(PQCHSM_TA_OPTEE)

/* 返回 0 成功；-2 认证失败；-1 其它失败 */
static int gcm_crypt(int enc, const uint8_t kek[TA_KEK_LEN],
                     const uint8_t nonce[TA_WRAP_NONCE_LEN],
                     const uint8_t *aad1, size_t aad1_len,
                     const uint8_t *aad2, size_t aad2_len,
                     const uint8_t *in, uint8_t *out, size_t len,
                     uint8_t tag[TA_WRAP_TAG_LEN])
{
	TEE_OperationHandle op  = TEE_HANDLE_NULL;
	TEE_ObjectHandle    key = TEE_HANDLE_NULL;
	TEE_Attribute       attr;
	TEE_Result          res;
	size_t              out_len = len;
	size_t              tag_len = TA_WRAP_TAG_LEN;
	uint32_t            got_tag_len;
	int                 rc = -1;

	res = TEE_AllocateOperation(&op, TEE_ALG_AES_GCM,
	                            enc ? TEE_MODE_ENCRYPT : TEE_MODE_DECRYPT,
	                            TA_KEK_LEN * 8);
	if (res != TEE_SUCCESS)
		goto out;
	res = TEE_AllocateTransientObject(TEE_TYPE_AES, TA_KEK_LEN * 8, &key);
	if (res != TEE_SUCCESS)
		goto out;
	TEE_InitRefAttribute(&attr, TEE_ATTR_SECRET_VALUE, (void *)kek,
	                     TA_KEK_LEN);
	res = TEE_PopulateTransientObject(key, &attr, 1);
	if (res != TEE_SUCCESS)
		goto out;
	res = TEE_SetOperationKey(op, key);
	if (res != TEE_SUCCESS)
		goto out;

	TEE_AEInit(op, (void *)nonce, TA_WRAP_NONCE_LEN, TA_WRAP_TAG_LEN * 8,
	           aad1_len + aad2_len, len);
	if (aad1_len)
		TEE_AEUpdateAAD(op, (void *)aad1, aad1_len);
	if (aad2_len)
		TEE_AEUpdateAAD(op, (void *)aad2, aad2_len);

	if (enc) {
		got_tag_len = TA_WRAP_TAG_LEN;
		res = TEE_AEEncryptFinal(op, (void *)in, len, out, &out_len,
		                         tag, &got_tag_len);
		if (res == TEE_SUCCESS &&
		    (out_len != len || got_tag_len != TA_WRAP_TAG_LEN))
			res = TEE_ERROR_GENERIC;
	} else {
		res = TEE_AEDecryptFinal(op, (void *)in, len, out, &out_len,
		                         (void *)tag, tag_len);
		if (res == TEE_SUCCESS && out_len != len)
			res = TEE_ERROR_GENERIC;
	}
	if (res == TEE_ERROR_MAC_INVALID)
		rc = -2;
	else if (res == TEE_SUCCESS)
		rc = 0;

out:
	if (key != TEE_HANDLE_NULL)
		TEE_FreeTransientObject(key);
	if (op != TEE_HANDLE_NULL)
		TEE_FreeOperation(op);
	return rc;
}

#else /* native 测试构建：OpenSSL EVP */

static int gcm_crypt(int enc, const uint8_t kek[TA_KEK_LEN],
                     const uint8_t nonce[TA_WRAP_NONCE_LEN],
                     const uint8_t *aad1, size_t aad1_len,
                     const uint8_t *aad2, size_t aad2_len,
                     const uint8_t *in, uint8_t *out, size_t len,
                     uint8_t tag[TA_WRAP_TAG_LEN])
{
	EVP_CIPHER_CTX *c = NULL;
	int             l = 0, rc = -1;

	c = EVP_CIPHER_CTX_new();
	if (!c)
		goto out;
	if (enc) {
		if (EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1)
			goto out;
		if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN,
		                        TA_WRAP_NONCE_LEN, NULL) != 1)
			goto out;
		if (EVP_EncryptInit_ex(c, NULL, NULL, kek, nonce) != 1)
			goto out;
		if (aad1_len &&
		    EVP_EncryptUpdate(c, NULL, &l, aad1, (int)aad1_len) != 1)
			goto out;
		if (aad2_len &&
		    EVP_EncryptUpdate(c, NULL, &l, aad2, (int)aad2_len) != 1)
			goto out;
		if (len && EVP_EncryptUpdate(c, out, &l, in, (int)len) != 1)
			goto out;
		if (EVP_EncryptFinal_ex(c, out + len, &l) != 1)
			goto out;
		if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG,
		                        TA_WRAP_TAG_LEN, tag) != 1)
			goto out;
		rc = 0;
	} else {
		if (EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1)
			goto out;
		if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN,
		                        TA_WRAP_NONCE_LEN, NULL) != 1)
			goto out;
		if (EVP_DecryptInit_ex(c, NULL, NULL, kek, nonce) != 1)
			goto out;
		if (aad1_len &&
		    EVP_DecryptUpdate(c, NULL, &l, aad1, (int)aad1_len) != 1)
			goto out;
		if (aad2_len &&
		    EVP_DecryptUpdate(c, NULL, &l, aad2, (int)aad2_len) != 1)
			goto out;
		if (len && EVP_DecryptUpdate(c, out, &l, in, (int)len) != 1)
			goto out;
		if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG,
		                        TA_WRAP_TAG_LEN, tag) != 1)
			goto out;
		/* tag 校验失败时 DecryptFinal 返回 <= 0 */
		if (EVP_DecryptFinal_ex(c, out + len, &l) != 1) {
			rc = -2;
			goto out;
		}
		rc = 0;
	}
out:
	EVP_CIPHER_CTX_free(c);
	return rc;
}

#endif /* PQCHSM_TA_OPTEE */

/* ---- PWRP 封包 ---------------------------------------------------------- */

int ta_wrap_seal(const uint8_t kek[TA_KEK_LEN],
                 const uint8_t *aad, size_t aad_len,
                 const uint8_t *pt, size_t pt_len,
                 uint8_t *blob, size_t cap, size_t *blob_len)
{
	uint8_t nonce[TA_WRAP_NONCE_LEN];
	size_t  need;
	int     rc;

	if (!kek || !blob || !blob_len)
		return -1;
	if ((!pt && pt_len) || (!aad && aad_len))
		return -1;
	if (pt_len > 0x7fffffffu || aad_len > 0x7fffffffu)
		return -1;
	need = ta_wrap_blob_len(pt_len);
	if (cap < need)
		return -1;

	pwrp_hdr_put(blob, (uint32_t)aad_len, (uint32_t)pt_len);

	pqchsm_randombytes(nonce, TA_WRAP_NONCE_LEN);
	memcpy(blob + PWRP_OFF_NONCE, nonce, TA_WRAP_NONCE_LEN);

	rc = gcm_crypt(1, kek, nonce,
	               blob, TA_WRAP_HDR_LEN, aad, aad_len,
	               pt, blob + TA_WRAP_HDR_LEN + TA_WRAP_NONCE_LEN, pt_len,
	               blob + TA_WRAP_HDR_LEN + TA_WRAP_NONCE_LEN + pt_len);
	pqchsm_bzero(nonce, sizeof(nonce));
	if (rc != 0)
		return rc;
	*blob_len = need;
	return 0;
}

int ta_wrap_open(const uint8_t kek[TA_KEK_LEN],
                 const uint8_t *aad, size_t aad_len,
                 const uint8_t *blob, size_t blob_len,
                 uint8_t *pt, size_t cap, size_t *pt_len)
{
	const uint8_t *nonce, *ct, *tag;
	uint32_t       hdr_aad_len, hdr_ct_len;
	int            rc;

	if (!kek || !blob || !pt_len)
		return -1;
	if ((!pt && cap) || (!aad && aad_len))
		return -1;
	if (blob_len < TA_WRAP_OVERHEAD)
		return -1;
	if (pwrp_hdr_parse(blob, &hdr_aad_len, &hdr_ct_len) != 0)
		return -1;
	/* 头里声明的 aad_len 必须与调用方给的一致 —— 否则就是拿别处的元数据
	 * 来配这份密文。这一条留在调用方判（见 pwrp_hdr_parse 的说明）。 */
	if (hdr_aad_len != aad_len)
		return -1;
	if ((size_t)hdr_ct_len + TA_WRAP_OVERHEAD != blob_len)
		return -1;
	if (cap < hdr_ct_len)
		return -1;

	nonce = blob + TA_WRAP_HDR_LEN;
	ct    = nonce + TA_WRAP_NONCE_LEN;
	tag   = ct + hdr_ct_len;

	rc = gcm_crypt(0, kek, nonce,
	               blob, TA_WRAP_HDR_LEN, aad, aad_len,
	               ct, pt, hdr_ct_len, (uint8_t *)tag);
	if (rc != 0) {
		/* 认证失败：输出缓冲清零，不交出未认证的明文 */
		if (pt && cap)
			pqchsm_bzero(pt, cap);
		return rc == -2 ? -2 : -1;
	}
	*pt_len = hdr_ct_len;
	return 0;
}
