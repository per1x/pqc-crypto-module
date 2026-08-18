/* tls_test.c —— mTLS 传输层的回归（不需要板子、不需要 FPGA）
 *
 * 这一层要挡的四件事，每一件单独立一个用例。**否定用例是主体** ——
 * "能连上"只证明配置没写错，"连不上"才证明它在挡人：
 *
 *   ① 正常：本 CA 签发的客户端证书 → 握手成功，数据完整往返；
 *   ② 别家 CA 签的客户端证书       → 必须被拒；
 *   ③ 干脆不出示客户端证书          → 必须被拒（这条最容易被配漏：
 *      少写 SSL_VERIFY_FAIL_IF_NO_PEER_CERT 的话它会握手成功）；
 *   ④ CN 不在 ACL 名单里            → 必须被拒。
 *
 * 用法：tls_test <pki目录> <case>
 *   case ∈ {ok, wrongca, nocert, acl}
 * 父进程 fork 一个服务端，子进程当客户端，走本机 127.0.0.1 上的一个临时端口。
 */
#define _GNU_SOURCE
#include "pqcs_tls.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *PKI;

static void path(char *out, size_t cap, const char *f)
{
	snprintf(out, cap, "%s/%s", PKI, f);
}

/* 服务端：接一个连接、握手、验 ACL、把收到的字节原样回送。
 * 成功返回 0。 */
static int run_server(int lfd, const char *acl_file, int expect_ok)
{
	char ca[512], crt[512], key[512], err[256] = {0}, cn[128] = {0};
	SSL_CTX *ctx;
	SSL *ssl;
	int fd, rc = 1;
	unsigned char buf[64];

	path(ca, sizeof ca, "hsm_ca.crt");
	path(crt, sizeof crt, "hsm_device.crt");
	path(key, sizeof key, "hsm_device.key");

	ctx = pqcs_tls_server_ctx(ca, crt, key, err, sizeof err);
	if (!ctx) {
		fprintf(stderr, "server ctx: %s\n", err);
		return 1;
	}
	fd = accept(lfd, NULL, NULL);
	if (fd < 0) {
		SSL_CTX_free(ctx);
		return 1;
	}
	ssl = SSL_new(ctx);
	SSL_set_fd(ssl, fd);
	if (SSL_accept(ssl) != 1) {
		pqcs_tls_last_error(err, sizeof err);
		fprintf(stderr, "server: 握手被拒（%s）\n", err);
		rc = expect_ok ? 1 : 0;      /* 该拒的拒了就是通过 */
		goto out;
	}
	if (pqcs_tls_peer_cn(ssl, cn, sizeof cn) != 0) {
		fprintf(stderr, "server: 拿不到客户端 CN\n");
		rc = expect_ok ? 1 : 0;
		goto out;
	}
	if (!pqcs_tls_acl_allows(acl_file, cn)) {
		fprintf(stderr, "server: CN=\"%s\" 被 ACL 拒\n", cn);
		rc = expect_ok ? 1 : 0;
		goto out;
	}
	if (pqcs_tls_read_all(ssl, buf, sizeof buf) == 0 &&
	    pqcs_tls_write_all(ssl, buf, sizeof buf) == 0) {
		fprintf(stderr, "server: CN=\"%s\" 通过，数据已回送\n", cn);
		rc = expect_ok ? 0 : 1;
	}
out:
	SSL_free(ssl);
	close(fd);
	SSL_CTX_free(ctx);
	return rc;
}

/* 客户端。cert/key 传 NULL 表示**不出示客户端证书**（用例 ③）。 */
static int run_client(int port, const char *ca_file, const char *cert_file,
                      const char *key_file, int expect_ok)
{
	char err[256] = {0};
	SSL_CTX *ctx;
	SSL *ssl;
	int fd, rc = 1;
	struct sockaddr_in sa;
	unsigned char tx[64], rx[64];
	size_t i;

	if (cert_file) {
		ctx = pqcs_tls_client_ctx(ca_file, cert_file, key_file, err, sizeof err);
	} else {
		/* 不带证书的客户端 ctx —— 故意不走 pqcs_tls_client_ctx，
		 * 因为那个函数强制要证书。这里要模拟的正是一个"不肯出示证书"的对端。 */
		ctx = SSL_CTX_new(TLS_client_method());
		if (ctx) {
			SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
			SSL_CTX_load_verify_locations(ctx, ca_file, NULL);
			SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
		}
	}
	if (!ctx) {
		fprintf(stderr, "client ctx: %s\n", err);
		return expect_ok ? 1 : 0;
	}

	fd = socket(AF_INET, SOCK_STREAM, 0);
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		perror("connect");
		close(fd);
		SSL_CTX_free(ctx);
		return 1;
	}
	ssl = SSL_new(ctx);
	SSL_set_fd(ssl, fd);
	if (SSL_connect(ssl) != 1) {
		pqcs_tls_last_error(err, sizeof err);
		fprintf(stderr, "client: 握手失败（%s）\n", err);
		rc = expect_ok ? 1 : 0;
		goto out;
	}
	if (SSL_get_verify_result(ssl) != X509_V_OK) {
		fprintf(stderr, "client: 设备证书验不过\n");
		rc = expect_ok ? 1 : 0;
		goto out;
	}
	for (i = 0; i < sizeof tx; i++) {
		tx[i] = (unsigned char)(i * 7 + 3);
	}
	if (pqcs_tls_write_all(ssl, tx, sizeof tx) != 0 ||
	    pqcs_tls_read_all(ssl, rx, sizeof rx) != 0 ||
	    memcmp(tx, rx, sizeof tx) != 0) {
		fprintf(stderr, "client: 数据没能完整往返\n");
		rc = expect_ok ? 1 : 0;
		goto out;
	}
	fprintf(stderr, "client: 通过，64 字节完整往返\n");
	rc = expect_ok ? 0 : 1;
out:
	SSL_free(ssl);
	close(fd);
	SSL_CTX_free(ctx);
	return rc;
}

int main(int argc, char **argv)
{
	int lfd, port, srv_rc = 1, cli_rc = 1, expect_ok;
	struct sockaddr_in sa;
	socklen_t sl = sizeof sa;
	pid_t pid;
	char ca[512], crt[512], key[512], acl[512];
	const char *mode;

	if (argc < 3) {
		fprintf(stderr, "用法: %s <pki目录> <ok|wrongca|nocert|acl>\n", argv[0]);
		return 2;
	}
	PKI = argv[1];
	mode = argv[2];
	expect_ok = strcmp(mode, "ok") == 0;

	SSL_load_error_strings();
	OpenSSL_add_ssl_algorithms();

	path(ca, sizeof ca, "hsm_ca.crt");
	path(acl, sizeof acl, strcmp(mode, "acl") == 0 ? "hsm_acl" : "no_such_acl");
	if (strcmp(mode, "wrongca") == 0) {
		path(crt, sizeof crt, "rogue_client.crt");
		path(key, sizeof key, "rogue_client.key");
	} else {
		path(crt, sizeof crt, "client.crt");
		path(key, sizeof key, "client.key");
	}

	lfd = socket(AF_INET, SOCK_STREAM, 0);
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port = 0;                       /* 让内核挑一个空闲端口 */
	if (bind(lfd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(lfd, 1) < 0) {
		perror("bind/listen");
		return 2;
	}
	if (getsockname(lfd, (struct sockaddr *)&sa, &sl) < 0) {
		perror("getsockname");
		return 2;
	}
	port = ntohs(sa.sin_port);

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return 2;
	}
	if (pid == 0) {
		close(lfd);
		_exit(run_client(port,
		                 strcmp(mode, "wrongca") == 0 ? ca : ca,
		                 strcmp(mode, "nocert") == 0 ? NULL : crt,
		                 strcmp(mode, "nocert") == 0 ? NULL : key,
		                 expect_ok));
	}
	srv_rc = run_server(lfd, acl, expect_ok);
	close(lfd);
	{
		int st = 0;

		waitpid(pid, &st, 0);
		cli_rc = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
	}
	/* 两侧都要给出"符合预期"的结论。
	 * 只看一侧的话，"客户端连不上"与"服务端把谁都放进来了"分不开。 */
	if (srv_rc == 0 && cli_rc == 0) {
		printf("tls_test[%s]: 通过\n", mode);
		return 0;
	}
	printf("tls_test[%s]: 失败（server=%d client=%d）\n", mode, srv_rc, cli_rc);
	return 1;
}
