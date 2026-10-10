/* hsm_httpd —— 密码机的 HTTPS（JSON）网关
 *
 *   hsm_httpd [HTTPS端口 [本机HTTP端口]]     默认 8443、8080；端口给 0 = 不开那一个
 *
 * 让任何会发 HTTP 的上层应用（curl、Python、Java、网页后端……）直接调用密码机，
 * 不用链接 libsdfe。业务语义与 hsmctl 完全相同 —— 两者共用 hsmops.c。
 *
 * ============================================================================
 * 【只有 HTTPS + 双向证书，没有明文 HTTP】
 * ============================================================================
 * 与 daemon 的 9797 远程口同一套 PKI、同一个口径（service/pqcs_tls.h）：
 *   · 服务端证书 hsm/pki/hsm_device.crt + key，客户端证书必须由 hsm_ca.crt 签发；
 *   · 可选白名单 hsm/pki/hsm_acl（按客户端证书 CN）；
 *   · **三样凭据缺一就不监听**（fail-closed）—— 宁可 HTTP 用不了，也不要一个
 *     谁都能连、能驱动密码机的明文端口。
 *
 * 唯一的例外是**本机回环口** 127.0.0.1:8080（明文 HTTP、无证书）：只绑在 lo 上，
 * 网线那头连不到，给板上的应用 / 演示脚本直接调用（板上没有 curl，用 nc 发即可）。
 * 能连它的只有本机进程 —— 与本机能直接跑 hsmctl 是同一个信任边界。
 * 不需要时启动参数第二个端口给 0。
 *
 * ============================================================================
 * 【接口】（全部 JSON；二进制字段用 base64，摘要 / 共享密钥用十六进制）
 * ============================================================================
 *   GET    /v1/info
 *   GET    /v1/keys                         列出密钥
 *   POST   /v1/keys                         {"name","alg"} 生成密钥
 *   GET    /v1/keys/{name}                  一把密钥的信息（含公钥）
 *   DELETE /v1/keys/{name}                  销毁
 *   POST   /v1/keys/{name}/encrypt          {"plaintext" | "plaintext_b64"}
 *   POST   /v1/keys/{name}/decrypt          {"ciphertext_b64"}
 *   POST   /v1/keys/{name}/sign             {"message" | "message_b64"}
 *   POST   /v1/keys/{name}/verify           {"message" | "message_b64", "signature_b64"}
 *   POST   /v1/keys/{name}/encapsulate
 *   POST   /v1/keys/{name}/decapsulate      {"ciphertext_b64"}
 *   POST   /v1/sm3                          {"data" | "data_b64"}
 *   POST   /v1/random                       {"length"}
 *
 * 任何请求加 ?trace=1，响应里多一个 "trace" 数组：这次请求在 TEE / TEE→PL /
 * PL / NW 各执行了哪几步（取自 daemon 的分层日志，只在那一层成功返回后才写）。
 *
 * 单线程、一次一个请求（daemon 那侧本来就串行化硬件访问），每个请求一个会话，
 * 请求结束会话即关，daemon 随即擦掉 PL 里的残留。
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/ssl.h>

#include "hsmops.h"
#include "pqcs_tls.h"

#define HSM_LOG    "/media/sd-mmcblk1p2/hsm/hsm-boot.log"
#define HDR_MAX    8192
#define BODY_MAX   (256u * 1024u)

/* ---------------------------------------------------------------- 日志 */
static void logf_(const char *fmt, ...)
{
	char ts[16];
	time_t t = time(NULL);
	va_list ap;

	strftime(ts, sizeof ts, "%H:%M:%S", localtime(&t));
	fprintf(stderr, "[%s] [HTTP] ", ts);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

/* ---------------------------------------------------------------- JSON 输出 */
struct buf { char *p; size_t n, cap; };

static void bput(struct buf *b, const char *s, size_t n)
{
	if (b->n + n + 1 > b->cap) {
		size_t nc = (b->cap ? b->cap * 2 : 4096);
		while (nc < b->n + n + 1) nc *= 2;
		b->p = realloc(b->p, nc);
		if (!b->p) { logf_("内存不足"); exit(1); }
		b->cap = nc;
	}
	memcpy(b->p + b->n, s, n);
	b->n += n;
	b->p[b->n] = 0;
}

static void bstr(struct buf *b, const char *s) { bput(b, s, strlen(s)); }

static void bfmt(struct buf *b, const char *fmt, ...)
{
	char t[512];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(t, sizeof t, fmt, ap);
	va_end(ap);
	bput(b, t, n < 0 ? 0 : (size_t)n < sizeof t ? (size_t)n : sizeof t - 1);
}

static void bjstr(struct buf *b, const char *s)      /* 带引号、转义的 JSON 字符串 */
{
	bput(b, "\"", 1);
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"') bstr(b, "\\\"");
		else if (c == '\\') bstr(b, "\\\\");
		else if (c == '\n') bstr(b, "\\n");
		else if (c == '\r') bstr(b, "\\r");
		else if (c == '\t') bstr(b, "\\t");
		else if (c < 0x20) bfmt(b, "\\u%04x", c);
		else bput(b, (const char *)&c, 1);
	}
	bput(b, "\"", 1);
}

static void bkey(struct buf *b, const char *k, int first)
{
	if (!first) bput(b, ",", 1);
	bjstr(b, k);
	bput(b, ":", 1);
}

static void bb64(struct buf *b, const uint8_t *d, uint32_t n)
{
	char *t = malloc(4 * ((n + 2) / 3) + 1);
	int m = EVP_EncodeBlock((unsigned char *)t, d, (int)n);
	bput(b, "\"", 1); bput(b, t, (size_t)m); bput(b, "\"", 1);
	free(t);
}

static void bhex(struct buf *b, const uint8_t *d, uint32_t n)
{
	uint32_t i;
	bput(b, "\"", 1);
	for (i = 0; i < n; i++) bfmt(b, "%02x", d[i]);
	bput(b, "\"", 1);
}

/* ---------------------------------------------------------------- JSON 输入 */
/* 取一个顶层字符串字段（只认扁平对象 —— 本网关的请求都是扁平的）。 */
static int jget(const char *body, const char *key, char **out, size_t *outlen)
{
	char pat[80];
	const char *p, *q;
	char *o;
	size_t n = 0;

	snprintf(pat, sizeof pat, "\"%s\"", key);
	p = strstr(body, pat);
	if (!p) return -1;
	p += strlen(pat);
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
	if (*p != ':') return -1;
	p++;
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
	if (*p != '"') {                         /* 数字 */
		q = p;
		while ((*q >= '0' && *q <= '9') || *q == '-') q++;
		if (q == p) return -1;
		o = malloc((size_t)(q - p) + 1);
		memcpy(o, p, (size_t)(q - p)); o[q - p] = 0;
		*out = o; if (outlen) *outlen = (size_t)(q - p);
		return 0;
	}
	p++;
	o = malloc(strlen(p) + 1);
	for (q = p; *q && *q != '"'; q++) {
		if (*q == '\\' && q[1]) {
			q++;
			o[n++] = *q == 'n' ? '\n' : *q == 't' ? '\t' : *q == 'r' ? '\r' : *q;
		} else {
			o[n++] = *q;
		}
	}
	if (*q != '"') { free(o); return -1; }
	o[n] = 0;
	*out = o; if (outlen) *outlen = n;
	return 0;
}

static uint8_t *b64dec(const char *s, uint32_t *n)
{
	size_t l = strlen(s), pad = 0;
	uint8_t *o;
	int m;

	if (l % 4) return NULL;
	if (l && s[l - 1] == '=') pad++;
	if (l > 1 && s[l - 2] == '=') pad++;
	o = malloc(l / 4 * 3 + 1);
	m = EVP_DecodeBlock(o, (const unsigned char *)s, (int)l);
	if (m < 0) { free(o); return NULL; }
	*n = (uint32_t)m - (uint32_t)pad;
	return o;
}

/* 先找 <key>_b64（二进制），再找 <key>（UTF-8 文本） */
static uint8_t *jbytes(const char *body, const char *key, uint32_t *n)
{
	char k[64], *v;
	size_t vl;
	uint8_t *o;

	snprintf(k, sizeof k, "%s_b64", key);
	if (jget(body, k, &v, NULL) == 0) {
		o = b64dec(v, n);
		free(v);
		return o;
	}
	if (jget(body, key, &v, &vl) == 0) {
		*n = (uint32_t)vl;
		return (uint8_t *)v;
	}
	return NULL;
}

/* ---------------------------------------------------------------- trace */
static long log_lines(void)
{
	FILE *f = fopen(HSM_LOG, "r");
	long n = 0;
	int c;

	if (!f) return 0;
	while ((c = getc(f)) != EOF)
		if (c == '\n') n++;
	fclose(f);
	return n;
}

static void add_trace(struct buf *b, long mark)
{
	static const struct { const char *tag, *layer; } T[] = {
		{ "[TEE→PL]", "TEE→PL" }, { "[TEE]", "TEE" }, { "[PL]", "PL" }, { "[NW]", "NW" } };
	FILE *f = fopen(HSM_LOG, "r");
	char line[1024];
	long n = 0;
	int first = 1;
	size_t i;

	bstr(b, ",\"trace\":[");
	if (f) {
		while (fgets(line, sizeof line, f)) {
			char *p;
			if (n++ < mark) continue;
			line[strcspn(line, "\n")] = 0;
			p = strchr(line, ']');           /* 跳过 "[hh:mm:ss]" */
			if (!p) continue;
			p++;
			while (*p == ' ') p++;
			for (i = 0; i < sizeof T / sizeof T[0]; i++) {
				size_t tl = strlen(T[i].tag);
				if (!strncmp(p, T[i].tag, tl)) {
					p += tl;
					while (*p == ' ') p++;
					if (!first) bput(b, ",", 1);
					first = 0;
					bstr(b, "{\"layer\":"); bjstr(b, T[i].layer);
					bstr(b, ",\"detail\":"); bjstr(b, p);
					bput(b, "}", 1);
					break;
				}
			}
		}
		fclose(f);
	}
	bput(b, "]", 1);
}

/* ---------------------------------------------------------------- 路由 */
struct resp { int code; struct buf body; };

static void err(struct resp *r, int code, const char *msg)
{
	r->code = code;
	r->body.n = 0;
	bstr(&r->body, "{\"error\":");
	bjstr(&r->body, msg);
}

static int err_code(void)
{
	const char *e = hsm_err();
	if (strstr(e, "没有这把密钥")) return 404;
	if (strstr(e, "已存在")) return 409;
	if (strstr(e, "不合法") || strstr(e, "不认识") || strstr(e, "不是") ||
	    strstr(e, "长度不对") || strstr(e, "太长") || strstr(e, "字节数")) return 400;
	if (strstr(e, "TA 拒绝") || strstr(e, "解密失败")) return 422;
	return 500;
}

static void key_json(struct buf *b, const char *name, const hsm_key *k, int with_pk)
{
	bkey(b, "name", 1); bjstr(b, name);
	bkey(b, "alg", 0); bjstr(b, hsm_alg_name(k));
	bkey(b, "type", 0);
	bjstr(b, k->kind == HSM_KIND_SYM ? "symmetric" : k->kind == HSM_KIND_MLDSA ? "signature" : "kem");
	bkey(b, "public_key_bytes", 0); bfmt(b, "%u", k->pk_len);
	bkey(b, "blob_bytes", 0); bfmt(b, "%u", k->blob_len);
	bkey(b, "created", 0); bfmt(b, "%u", k->created);
	if (with_pk && k->pk_len) { bkey(b, "public_key_b64", 0); bb64(b, k->pk, k->pk_len); }
}

static void list_cb(const char *name, const hsm_key *k, void *u)
{
	struct buf *b = u;
	if (b->p[b->n - 1] != '[') bput(b, ",", 1);
	bput(b, "{", 1);
	key_json(b, name, k, 0);
	bput(b, "}", 1);
}

static void route(const char *m, const char *path, const char *body, struct resp *r)
{
	struct buf *b = &r->body;
	char name[64] = "", op[32] = "";
	const char *rest;

	r->code = 200;
	bstr(b, "{");

	if (!strcmp(path, "/v1/info") && !strcmp(m, "GET")) {
		char info[256];
		if (hsm_info(info, sizeof info)) { err(r, 503, hsm_err()); return; }
		bkey(b, "device", 1); bjstr(b, info);
		bkey(b, "keystore", 0); bjstr(b, hsm_keystore());
		bkey(b, "architecture", 0);
		bjstr(b, "密钥在 TEE（OP-TEE TA）里生成、包裹、装载；运算在 FPGA（PL）里做；本网关与 daemon 只搬句柄和密文 blob");
		return;
	}
	if (!strcmp(path, "/v1/random") && !strcmp(m, "POST")) {
		char *v; uint8_t o[1024]; uint32_t n;
		if (jget(body, "length", &v, NULL)) { err(r, 400, "缺 length"); return; }
		n = (uint32_t)atoi(v); free(v);
		if (hsm_random(n, o)) { err(r, err_code(), hsm_err()); return; }
		bkey(b, "random_hex", 1); bhex(b, o, n);
		bkey(b, "source", 0); bjstr(b, "FPGA 环振 TRNG + SP 800-90B 健康检测");
		return;
	}
	if (!strcmp(path, "/v1/sm3") && !strcmp(m, "POST")) {
		uint32_t n; uint8_t d[32], *in = jbytes(body, "data", &n);
		if (!in) { err(r, 400, "缺 data 或 data_b64"); return; }
		if (hsm_sm3(in, n, d)) { free(in); err(r, err_code(), hsm_err()); return; }
		free(in);
		bkey(b, "sm3_hex", 1); bhex(b, d, 32);
		bkey(b, "bytes", 0); bfmt(b, "%u", n);
		return;
	}
	if (!strcmp(path, "/v1/keys")) {
		if (!strcmp(m, "GET")) {
			bkey(b, "keystore", 1); bjstr(b, hsm_keystore());
			bkey(b, "keys", 0); bput(b, "[", 1);
			hsm_key_list(list_cb, b);
			bput(b, "]", 1);
			return;
		}
		if (!strcmp(m, "POST")) {
			char *nm = NULL, *alg = NULL; hsm_key k;
			if (jget(body, "name", &nm, NULL) || jget(body, "alg", &alg, NULL)) {
				free(nm); free(alg); err(r, 400, "需要 name 与 alg"); return;
			}
			if (hsm_key_gen(alg, nm, &k)) { free(nm); free(alg); err(r, err_code(), hsm_err()); return; }
			r->code = 201;
			key_json(b, nm, &k, 1);
			bkey(b, "note", 0);
			bjstr(b, k.kind == HSM_KIND_SYM ? "密钥由 TA 生成并用 KEK 包裹；本网关只保存密文 blob"
			                                : "私钥种子由 TA 生成并用 KEK 包裹；本网关只保存公钥与密文 blob");
			free(nm); free(alg);
			return;
		}
		err(r, 405, "只支持 GET / POST");
		return;
	}
	if (!strncmp(path, "/v1/keys/", 9)) {
		rest = path + 9;
		sscanf(rest, "%63[^/]/%31s", name, op);
		if (!hsm_name_ok(name)) { err(r, 400, "密钥名不合法"); return; }
	} else {
		err(r, 404, "没有这个接口");
		return;
	}

	if (!op[0]) {
		hsm_key k;
		if (!strcmp(m, "GET")) {
			if (hsm_key_load(name, &k)) { err(r, err_code(), hsm_err()); return; }
			key_json(b, name, &k, 1);
			bkey(b, "blob_head_hex", 0); bhex(b, k.blob, k.blob_len < 24 ? k.blob_len : 24);
			return;
		}
		if (!strcmp(m, "DELETE")) {
			if (hsm_key_destroy(name)) { err(r, err_code(), hsm_err()); return; }
			bkey(b, "destroyed", 1); bjstr(b, name);
			return;
		}
		err(r, 405, "只支持 GET / DELETE");
		return;
	}
	if (strcmp(m, "POST")) { err(r, 405, "只支持 POST"); return; }

	if (!strcmp(op, "encrypt") || !strcmp(op, "decrypt")) {
		int enc = op[0] == 'e';
		uint32_t n, on; uint8_t *in, *o;
		in = jbytes(body, enc ? "plaintext" : "ciphertext", &n);
		if (!in) { err(r, 400, enc ? "缺 plaintext 或 plaintext_b64" : "缺 ciphertext_b64"); return; }
		if ((enc ? hsm_encrypt : hsm_decrypt)(name, in, n, &o, &on)) {
			free(in); err(r, err_code(), hsm_err()); return;
		}
		free(in);
		if (enc) {
			bkey(b, "ciphertext_b64", 1); bb64(b, o, on);
			bkey(b, "format", 0); bjstr(b, "IV(16) ‖ CBC 密文（PKCS#7）");
		} else {
			uint32_t i; int text = 1;
			bkey(b, "plaintext_b64", 1); bb64(b, o, on);
			for (i = 0; i < on; i++) if (o[i] < 0x09 || (o[i] > 0x0d && o[i] < 0x20)) text = 0;
			if (text) { char *t = malloc(on + 1); memcpy(t, o, on); t[on] = 0;
				    bkey(b, "plaintext", 0); bjstr(b, t); free(t); }
		}
		bkey(b, "bytes", 0); bfmt(b, "%u", on);
		free(o);
		return;
	}
	if (!strcmp(op, "sign")) {
		uint32_t n, sl = 4627; uint8_t sig[4627], *msg = jbytes(body, "message", &n);
		if (!msg) { err(r, 400, "缺 message 或 message_b64"); return; }
		if (hsm_sign(name, msg, n, sig, &sl)) { free(msg); err(r, err_code(), hsm_err()); return; }
		free(msg);
		bkey(b, "signature_b64", 1); bb64(b, sig, sl);
		bkey(b, "signature_bytes", 0); bfmt(b, "%u", sl);
		return;
	}
	if (!strcmp(op, "verify")) {
		uint32_t n, sl; int v; char *s64;
		uint8_t *msg = jbytes(body, "message", &n), *sig;
		if (!msg || jget(body, "signature_b64", &s64, NULL)) { free(msg); err(r, 400, "需要 message 与 signature_b64"); return; }
		sig = b64dec(s64, &sl); free(s64);
		if (!sig) { free(msg); err(r, 400, "signature_b64 不是合法 base64"); return; }
		v = hsm_verify(name, msg, n, sig, sl);
		free(msg); free(sig);
		if (v < 0) { err(r, err_code(), hsm_err()); return; }
		bkey(b, "valid", 1); bstr(b, v == 0 ? "true" : "false");
		return;
	}
	if (!strcmp(op, "encapsulate")) {
		uint8_t ct[1568], ss[32]; uint32_t cl = sizeof ct;
		if (hsm_encap(name, ct, &cl, ss)) { err(r, err_code(), hsm_err()); return; }
		bkey(b, "ciphertext_b64", 1); bb64(b, ct, cl);
		bkey(b, "shared_secret_hex", 0); bhex(b, ss, 32);
		return;
	}
	if (!strcmp(op, "decapsulate")) {
		uint32_t cl; uint8_t ss[32], *ct; char *c64;
		if (jget(body, "ciphertext_b64", &c64, NULL)) { err(r, 400, "缺 ciphertext_b64"); return; }
		ct = b64dec(c64, &cl); free(c64);
		if (!ct) { err(r, 400, "ciphertext_b64 不是合法 base64"); return; }
		if (hsm_decap(name, ct, cl, ss)) { free(ct); err(r, err_code(), hsm_err()); return; }
		free(ct);
		bkey(b, "shared_secret_hex", 1); bhex(b, ss, 32);
		return;
	}
	err(r, 404, "没有这个操作");
}

/* ---------------------------------------------------------------- HTTP */
/* 一条连接：TLS（ssl 非空）或本机回环明文（只有 fd） */
struct conn { SSL *ssl; int fd; };

static int c_read(struct conn *c, char *b, size_t n)
{
	return c->ssl ? SSL_read(c->ssl, b, (int)n) : (int)read(c->fd, b, n);
}

static int c_read_all(struct conn *c, char *b, size_t n)
{
	if (c->ssl) return pqcs_tls_read_all(c->ssl, b, n);
	while (n) {
		ssize_t k = read(c->fd, b, n);
		if (k <= 0) return -1;
		b += k; n -= (size_t)k;
	}
	return 0;
}

static void c_write_all(struct conn *c, const char *b, size_t n)
{
	if (c->ssl) { pqcs_tls_write_all(c->ssl, b, n); return; }
	while (n) {
		ssize_t k = write(c->fd, b, n);
		if (k <= 0) return;
		b += k; n -= (size_t)k;
	}
}
static const char *reason(int c)
{
	switch (c) {
	case 200: return "OK"; case 201: return "Created"; case 400: return "Bad Request";
	case 403: return "Forbidden"; case 404: return "Not Found"; case 405: return "Method Not Allowed";
	case 409: return "Conflict"; case 413: return "Payload Too Large"; case 422: return "Unprocessable Entity";
	case 503: return "Service Unavailable"; default: return "Internal Server Error";
	}
}

static void send_resp(struct conn *c, struct resp *r)
{
	char h[256];
	int n;

	bput(&r->body, "}\n", 2);
	n = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\n"
	             "Content-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",
	             r->code, reason(r->code), r->body.n);
	c_write_all(c, h, (size_t)n);
	c_write_all(c, r->body.p, r->body.n);
}

static void serve(struct conn *c, const char *cn)
{
	char hdr[HDR_MAX + 1], method[8] = "", target[512] = "", path[512];
	char *body = NULL, *q, *p;
	size_t got = 0, clen = 0, hlen;
	long mark;
	int trace;
	struct resp r = { 0, { NULL, 0, 0 } };
	struct timeval t0, t1;

	gettimeofday(&t0, NULL);
	/* 读到头结束 */
	while (got < HDR_MAX) {
		int k = c_read(c, hdr + got, HDR_MAX - got);
		if (k <= 0) return;
		got += (size_t)k;
		hdr[got] = 0;
		if (strstr(hdr, "\r\n\r\n")) break;
	}
	p = strstr(hdr, "\r\n\r\n");
	if (!p) return;
	hlen = (size_t)(p - hdr) + 4;
	if (sscanf(hdr, "%7s %511s", method, target) != 2) return;
	for (q = hdr; (q = strcasestr(q, "\ncontent-length:")); q++)
		clen = (size_t)strtoul(q + 16, NULL, 10);
	if (clen > BODY_MAX) {
		err(&r, 413, "请求体太大");
		send_resp(c, &r);
		free(r.body.p);
		return;
	}
	body = calloc(1, clen + 1);
	memcpy(body, hdr + hlen, got - hlen < clen ? got - hlen : clen);
	if (got - hlen < clen &&
	    c_read_all(c, body + (got - hlen), clen - (got - hlen))) {
		free(body);
		return;
	}

	snprintf(path, sizeof path, "%s", target);
	trace = 0;
	if ((q = strchr(path, '?'))) {
		trace = strstr(q, "trace=1") != NULL;
		*q = 0;
	}
	mark = trace ? log_lines() : 0;

	route(method, path, body, &r);
	hsm_close();                       /* 会话结束：daemon 擦 PL 里的残留 */
	if (trace) {
		char req[600];

		usleep(150000);                /* 会话结束那一行是断开之后才写的 */

		snprintf(req, sizeof req, "%s %s", method, path);
		bkey(&r.body, "request", 0); bjstr(&r.body, req);
		bkey(&r.body, "client_cn", 0); bjstr(&r.body, cn);
		add_trace(&r.body, mark);
	}
	send_resp(c, &r);
	gettimeofday(&t1, NULL);
	logf_("CN=%s %s %s → %d（%.1f ms）", cn, method, target, r.code,
	      (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_usec - t0.tv_usec) / 1e3);
	free(r.body.p);
	free(body);
}

static int listen_on(uint32_t addr, int port)
{
	int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	struct sockaddr_in a;

	if (ls < 0) return -1;
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons((uint16_t)port);
	a.sin_addr.s_addr = htonl(addr);
	if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 16)) {
		logf_("监听 %d 失败：%s", port, strerror(errno));
		close(ls);
		return -1;
	}
	return ls;
}

static void on_tls(SSL_CTX *ctx, int c)
{
	char cn[128] = "?", e[256];
	struct conn k = { SSL_new(ctx), c };

	SSL_set_fd(k.ssl, c);
	if (SSL_accept(k.ssl) == 1) {
		if (pqcs_tls_peer_cn(k.ssl, cn, sizeof cn) == 0 &&
		    pqcs_tls_acl_allows(PQCS_ACL_FILE, cn)) {
			serve(&k, cn);
		} else {
			struct resp r = { 0, { NULL, 0, 0 } };
			err(&r, 403, "客户端证书不在白名单");
			send_resp(&k, &r);
			free(r.body.p);
			logf_("CN=%s 被白名单拒绝", cn);
		}
		SSL_shutdown(k.ssl);
	} else {
		pqcs_tls_last_error(e, sizeof e);
		logf_("TLS 握手失败：%s", e);
	}
	SSL_free(k.ssl);
}

int main(int argc, char **argv)
{
	int port = argc > 1 ? atoi(argv[1]) : 8443;
	int lport = argc > 2 ? atoi(argv[2]) : 8080;
	int ls = -1, ll = -1;
	char e[256];
	SSL_CTX *ctx = NULL;

	signal(SIGPIPE, SIG_IGN);
	if (port > 0) {
		ctx = pqcs_tls_server_ctx(PQCS_CA_FILE, PQCS_DEV_CERT, PQCS_DEV_KEY, e, sizeof e);
		if (!ctx)
			logf_("凭据不全或不可用（%s）—— HTTPS 口不监听（fail-closed：对外不开明文）", e);
		else if ((ls = listen_on(INADDR_ANY, port)) >= 0)
			logf_("HTTPS 网关监听 :%d（mTLS，客户端证书须由本机 CA 签发）", port);
	}
	if (lport > 0 && (ll = listen_on(INADDR_LOOPBACK, lport)) >= 0)
		logf_("本机 HTTP 监听 127.0.0.1:%d（只在回环口，外部连不到）", lport);
	if (ls < 0 && ll < 0)
		return 1;
	logf_("密钥库 %s", hsm_keystore());

	for (;;) {
		struct timeval tv = { 15, 0 };
		fd_set rs;
		int c, from_tls;

		FD_ZERO(&rs);
		if (ls >= 0) FD_SET(ls, &rs);
		if (ll >= 0) FD_SET(ll, &rs);
		if (select((ls > ll ? ls : ll) + 1, &rs, NULL, NULL, NULL) <= 0)
			continue;
		from_tls = ls >= 0 && FD_ISSET(ls, &rs);
		c = accept(from_tls ? ls : ll, NULL, NULL);
		if (c < 0) continue;
		setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
		setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
		if (from_tls) {
			on_tls(ctx, c);
		} else {
			struct conn k = { NULL, c };
			serve(&k, "local");
		}
		close(c);
	}
}
