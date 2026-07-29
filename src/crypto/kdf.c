#include "pqchsm/kdf.h"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <string.h>

/* 失败时不清 OpenSSL 错误队列，也不打印任何东西：
 * 调用方要么用 ERR_get_error() 取详情，要么直接看返回值。
 * 密码机不能往 stdout 吐东西（stdout 是 CLI 的协议通道）。 */

int pqc_kmac256(const uint8_t *key, size_t key_len,
                const uint8_t *data, size_t data_len,
                const char *custom,
                uint8_t *out, size_t out_len)
{
	EVP_MAC     *mac = NULL;
	EVP_MAC_CTX *ctx = NULL;
	OSSL_PARAM   params[3];
	size_t       nparams = 0;
	size_t       size_param = out_len;   /* 必须是 size_t 变量，OSSL_PARAM 存的是地址 */
	size_t       written = 0;
	int          ret = -1;

	if (!key || key_len == 0) {
		return -1;
	}
	if (!data && data_len != 0) {
		return -1;
	}
	if (!out || out_len == 0) {
		return -1;
	}

	mac = EVP_MAC_fetch(NULL, "KMAC-256", NULL);
	if (!mac) {
		goto cleanup;
	}
	ctx = EVP_MAC_CTX_new(mac);
	if (!ctx) {
		goto cleanup;
	}

	/* 输出长度必须作为参数设进去：它被 right_encode 进 KMAC 的尾部，
	 * 不设的话拿到的是默认长度的结果，且不符合 SP 800-185。 */
	params[nparams++] = OSSL_PARAM_construct_size_t(OSSL_MAC_PARAM_SIZE, &size_param);
	/* KMAC 的 custom 在 OpenSSL 里是 octet string（不是 utf8 string）——
	 * 传成 utf8 会让 EVP_MAC_init 直接返回 0 且不留错误码。
	 * 空串等价于不传，跳过可避免 provider 对零长参数的挑剔。 */
	if (custom && custom[0] != '\0') {
		params[nparams++] = OSSL_PARAM_construct_octet_string(
		        OSSL_MAC_PARAM_CUSTOM, (void *)(uintptr_t)custom, strlen(custom));
	}
	params[nparams] = OSSL_PARAM_construct_end();

	if (EVP_MAC_init(ctx, key, key_len, params) != 1) {
		goto cleanup;
	}
	if (data_len != 0 && EVP_MAC_update(ctx, data, data_len) != 1) {
		goto cleanup;
	}
	if (EVP_MAC_final(ctx, out, &written, out_len) != 1) {
		goto cleanup;
	}
	if (written != out_len) {
		goto cleanup;
	}
	ret = 0;

cleanup:
	EVP_MAC_CTX_free(ctx);
	EVP_MAC_free(mac);
	return ret;
}

int pqc_sha3_256(const uint8_t *data, size_t data_len, uint8_t out[32])
{
	static const uint8_t empty = 0;
	unsigned int n = 0;

	if (!out) {
		return -1;
	}
	if (!data && data_len != 0) {
		return -1;
	}

	if (EVP_Digest(data ? data : &empty, data_len, out, &n, EVP_sha3_256(), NULL) != 1) {
		return -1;
	}
	return n == 32u ? 0 : -1;
}

int pqc_kdf(const uint8_t *ikm, size_t ikm_len,
            const uint8_t *salt, size_t salt_len,
            const char *label,
            uint8_t *out, size_t out_len)
{
	/* label 强制非空：派生出来的密钥必须带用途标记，
	 * 否则 storage KEK 和 backup KEK 可能撞成同一个值。 */
	if (!label) {
		return -1;
	}
	return pqc_kmac256(ikm, ikm_len, salt, salt_len, label, out, out_len);
}
