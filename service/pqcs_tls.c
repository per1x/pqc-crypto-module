/* pqcs_tls.c —— mTLS 传输层。设计与取舍写在 pqcs_tls.h 的文件头。 */
#define _GNU_SOURCE
#include "pqcs_tls.h"

#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <stdio.h>
#include <string.h>

void pqcs_tls_last_error(char *out, size_t cap)
{
	unsigned long e = ERR_get_error();

	if (!out || cap == 0) {
		ERR_clear_error();
		return;
	}
	if (e == 0) {
		snprintf(out, cap, "(没有 OpenSSL 错误)");
		return;
	}
	ERR_error_string_n(e, out, cap);
	ERR_clear_error();
}

static SSL_CTX *common_ctx(const SSL_METHOD *m,
                           const char *ca_file, const char *cert_file,
                           const char *key_file, char *err, size_t errcap)
{
	SSL_CTX *ctx = SSL_CTX_new(m);

	if (!ctx) {
		pqcs_tls_last_error(err, errcap);
		return NULL;
	}

	/* TLS 1.3 only —— 见头文件。上下限一起顶死，否则 max 留在 1.3
	 * 而 min 还是 1.0，实际能协商出来的仍然是老版本。 */
	if (!SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) ||
	    !SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION)) {
		snprintf(err, errcap, "这份 OpenSSL 不支持 TLS 1.3");
		SSL_CTX_free(ctx);
		return NULL;
	}

	if (SSL_CTX_load_verify_locations(ctx, ca_file, NULL) != 1) {
		snprintf(err, errcap, "读不了 CA 证书 %s", ca_file);
		SSL_CTX_free(ctx);
		return NULL;
	}
	if (SSL_CTX_use_certificate_chain_file(ctx, cert_file) != 1) {
		snprintf(err, errcap, "读不了证书 %s", cert_file);
		SSL_CTX_free(ctx);
		return NULL;
	}
	if (SSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM) != 1) {
		snprintf(err, errcap, "读不了私钥 %s", key_file);
		SSL_CTX_free(ctx);
		return NULL;
	}
	/* 证书与私钥对不上是一个**开机就该发现**的配置错误。不查的话，
	 * 症状会推迟到第一次握手才出现，而且报出来的是一句难懂的 TLS 错误。 */
	if (SSL_CTX_check_private_key(ctx) != 1) {
		snprintf(err, errcap, "私钥与证书不匹配（%s / %s）", key_file, cert_file);
		SSL_CTX_free(ctx);
		return NULL;
	}

	/* 双向都要验，且**对端不出证书就直接失败**。
	 * 少了 FAIL_IF_NO_PEER_CERT 的话，一个干脆不发证书的客户端会握手成功，
	 * 然后 SSL_get_peer_certificate() 返回 NULL —— 于是"验证通过"这件事
	 * 完全取决于上层记不记得再查一次。这里不留那个机会。 */
	SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
	SSL_CTX_set_verify_depth(ctx, 4);
	SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
	return ctx;
}

SSL_CTX *pqcs_tls_server_ctx(const char *ca_file, const char *cert_file,
                             const char *key_file, char *err, size_t errcap)
{
	SSL_CTX *ctx = common_ctx(TLS_server_method(), ca_file, cert_file, key_file,
	                          err, errcap);

	if (!ctx) {
		return NULL;
	}
	/* 不做会话复用：这个 daemon 一次只服务一个连接，复用省不下什么，
	 * 却多一份需要正确失效的状态。 */
	SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
	SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET);
	return ctx;
}

SSL_CTX *pqcs_tls_client_ctx(const char *ca_file, const char *cert_file,
                             const char *key_file, char *err, size_t errcap)
{
	return common_ctx(TLS_client_method(), ca_file, cert_file, key_file,
	                  err, errcap);
}

int pqcs_tls_peer_cn(SSL *ssl, char *out, size_t cap)
{
	X509 *c;
	X509_NAME *n;
	int rc = -1;

	if (!ssl || !out || cap == 0) {
		return -1;
	}
	c = SSL_get_peer_certificate(ssl);
	if (!c) {
		return -1;
	}
	n = X509_get_subject_name(c);
	if (n && X509_NAME_get_text_by_NID(n, NID_commonName, out, (int)cap) > 0) {
		rc = 0;
	}
	X509_free(c);
	return rc;
}

int pqcs_tls_acl_allows(const char *acl_file, const char *cn)
{
	FILE *f;
	char line[256];
	int allowed = 0;

	if (!cn) {
		return 0;
	}
	f = fopen(acl_file, "r");
	if (!f) {
		return 1;   /* 没有名单文件 = 不做 CN 级筛选，见头文件 */
	}
	while (fgets(line, sizeof line, f)) {
		char *p = line;
		size_t l;

		while (*p == ' ' || *p == '\t') {
			p++;
		}
		if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') {
			continue;
		}
		l = strlen(p);
		while (l > 0 && (p[l - 1] == '\n' || p[l - 1] == '\r' ||
		                 p[l - 1] == ' ' || p[l - 1] == '\t')) {
			p[--l] = '\0';
		}
		/* CN 不是秘密（它就在对端出示的证书里，攻击者本来就知道自己写的是
		 * 什么），所以这里用普通比较即可，不需要常量时间。真正需要常量时间
		 * 的那件事 —— 证明"我持有这把私钥" —— 由 TLS 的签名验证做，
		 * 不在这一层。 */
		if (strcmp(p, cn) == 0) {
			allowed = 1;
			break;
		}
	}
	fclose(f);
	return allowed;
}

int pqcs_tls_read_all(SSL *ssl, void *buf, size_t n)
{
	unsigned char *b = buf;
	size_t got = 0;

	while (got < n) {
		int r = SSL_read(ssl, b + got, (int)(n - got));

		if (r <= 0) {
			return -1;
		}
		got += (size_t)r;
	}
	return 0;
}

int pqcs_tls_write_all(SSL *ssl, const void *buf, size_t n)
{
	const unsigned char *b = buf;
	size_t put = 0;

	while (put < n) {
		int r = SSL_write(ssl, b + put, (int)(n - put));

		if (r <= 0) {
			return -1;
		}
		put += (size_t)r;
	}
	return 0;
}
