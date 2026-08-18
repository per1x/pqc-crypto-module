// libsdfe.c —— SDF 风格接口的客户端实现
//
// 它只做一件事：把 SDFE_* 调用翻译成一条到 pqchsm_fpgad 的请求，等回应。
// **一行密码运算都不做** —— 做了就等于在软件里留一条影子实现，
// 那种实现最危险：硬件坏了它还能给出"正确"结果，于是没人发现硬件坏了。
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "sdfe.h"
#include "wire.h"
#include "pqcs_tls.h"

/* ssl 非 NULL = 远程（mTLS）。本机 UNIX socket 那条路不套 TLS：
 * 它是 0600 的、只在本机，能连上就已经是 root —— 加密它挡不住任何人，
 * 只会让板上的排查工具多一份证书要管。这条取舍写在 docs/SECURITY.md。 */
struct dev {
	int      fd;
	SSL     *ssl;
	SSL_CTX *ctx;
};

static int rw_all(int fd, void *p, size_t n, int wr)
{
	uint8_t *b = p;
	size_t d = 0;

	while (d < n) {
		ssize_t r = wr ? write(fd, b + d, n - d) : read(fd, b + d, n - d);

		if (r <= 0)
			return -1;
		d += (size_t)r;
	}
	return 0;
}

static int dev_rw(struct dev *d, void *p, size_t n, int wr)
{
	if (d->ssl)
		return wr ? pqcs_tls_write_all(d->ssl, p, n)
			  : pqcs_tls_read_all(d->ssl, p, n);
	return rw_all(d->fd, p, n, wr);
}

static void dev_free(struct dev *d)
{
	if (!d)
		return;
	if (d->ssl) {
		SSL_shutdown(d->ssl);
		SSL_free(d->ssl);
	}
	if (d->ctx)
		SSL_CTX_free(d->ctx);
	if (d->fd >= 0)
		close(d->fd);
	free(d);
}

static int call(struct dev *d, uint32_t op, uint32_t a0, uint32_t a1,
		const uint8_t *in, uint32_t in_len,
		uint8_t *out, uint32_t out_cap, uint32_t *out_len)
{
	struct pqcs_req q = { PQCS_MAGIC, op, a0, a1, in_len };
	struct pqcs_resp rp;

	if (!d || d->fd < 0)
		return SDR_OPENDEVICE;
	if (dev_rw(d, &q, sizeof q, 1))
		return SDR_COMMFAIL;
	if (in_len && dev_rw(d, (void *)in, in_len, 1))
		return SDR_COMMFAIL;
	if (dev_rw(d, &rp, sizeof rp, 0))
		return SDR_COMMFAIL;
	if (rp.magic != PQCS_MAGIC)
		return SDR_COMMFAIL;
	if (rp.len > out_cap)
		return SDR_INARGERR;
	if (rp.len && dev_rw(d, out, rp.len, 0))
		return SDR_COMMFAIL;
	if (out_len)
		*out_len = rp.len;
	return (int)rp.status;
}

int SDFE_OpenDevice(SDFE_HANDLE *phDev)
{
	struct dev *d;
	struct sockaddr_un sa;

	if (!phDev)
		return SDR_INARGERR;
	d = calloc(1, sizeof *d);
	if (!d)
		return SDR_UNKNOWERR;
	d->fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (d->fd < 0) { free(d); return SDR_OPENDEVICE; }
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	strncpy(sa.sun_path, PQCS_SOCK_PATH, sizeof sa.sun_path - 1);
	if (connect(d->fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		close(d->fd); free(d);
		return SDR_OPENDEVICE;
	}
	*phDev = d;
	return SDR_OK;
}

/* 远程打开：连另一台机器上的密码机，走 **mTLS**。
 *
 * 【为什么认证放在 OpenDevice 而不是每次调用里】
 * 认证是**连接**的属性，不是请求的属性。放在每次调用里意味着每条请求都要
 * 重新证明一次身份，而 TLS 已经在握手时把这件事做完了 —— 再做一遍既没有
 * 增加任何保证，又会让"这条连接到底认证过没有"变成一个可以被忘记检查的状态。
 *
 * 【握手失败就把整个句柄销毁，绝不返回半开的连接】
 * 老版本（预共享口令）这里就踩过：留一个"连上了但没认证"的句柄，后续调用
 * 会被服务端一条条拒绝并断开，报出来的错是 COMMFAIL —— 真正的原因（凭据不对）
 * 被藏起来了。所以这里失败即全拆，并且把 TLS 的原因回给调用方区分：
 *   SDR_AUTHFAIL  —— 握手/证书这一层不过（凭据不对、CN 不符、被 ACL 拒）
 *   SDR_OPENDEVICE—— 根本没连上（板子没起、端口不对、网络不通）
 *
 * 除了这个函数，远程和本机对调用方**完全一样**。 */
int SDFE_OpenDeviceRemote(SDFE_HANDLE *phDev, const char *host,
			  int port, const SDFE_TLS_CREDS *creds)
{
	struct dev *d;
	struct sockaddr_in sa;
	char err[256] = {0};
	int one = 1;

	if (!phDev || !host || !creds ||
	    !creds->ca_file || !creds->cert_file || !creds->key_file)
		return SDR_INARGERR;

	d = calloc(1, sizeof *d);
	if (!d)
		return SDR_UNKNOWERR;
	d->fd = -1;

	SSL_load_error_strings();
	OpenSSL_add_ssl_algorithms();
	d->ctx = pqcs_tls_client_ctx(creds->ca_file, creds->cert_file,
				     creds->key_file, err, sizeof err);
	if (!d->ctx) {
		fprintf(stderr, "TLS 凭据不可用：%s\n", err);
		dev_free(d);
		return SDR_AUTHFAIL;
	}

	d->fd = socket(AF_INET, SOCK_STREAM, 0);
	if (d->fd < 0) { dev_free(d); return SDR_OPENDEVICE; }

	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)(port ? port : PQCS_TCP_PORT));
	if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
		dev_free(d); return SDR_INARGERR;
	}
	if (connect(d->fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		dev_free(d); return SDR_OPENDEVICE;
	}
	setsockopt(d->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

	d->ssl = SSL_new(d->ctx);
	if (!d->ssl) { dev_free(d); return SDR_UNKNOWERR; }
	SSL_set_fd(d->ssl, d->fd);
	if (SSL_connect(d->ssl) != 1) {
		pqcs_tls_last_error(err, sizeof err);
		fprintf(stderr, "TLS 握手失败：%s\n", err);
		dev_free(d);
		return SDR_AUTHFAIL;
	}
	/* SSL_VERIFY_PEER 已经让握手在链验不过时失败，这里再确认一次结果码：
	 * 一行的代价，换掉"某天有人把 verify 模式改松了而没人发现"这个风险。 */
	if (SSL_get_verify_result(d->ssl) != X509_V_OK) {
		fprintf(stderr, "设备证书验证不通过\n");
		dev_free(d);
		return SDR_AUTHFAIL;
	}
	if (creds->expect_cn) {
		char cn[128] = {0};

		if (pqcs_tls_peer_cn(d->ssl, cn, sizeof cn) != 0 ||
		    strcmp(cn, creds->expect_cn) != 0) {
			fprintf(stderr, "设备身份不符：期望 CN=\"%s\"，实得 \"%s\"\n",
				creds->expect_cn, cn);
			dev_free(d);
			return SDR_AUTHFAIL;
		}
	}

	/* ---- 握手完了还要再走一个来回，才敢说"连上了" ----
	 *
	 * ⚠️ TLS 1.3 里 SSL_connect() 成功**不代表服务端接受了我们**。
	 *    客户端证书是在最后一段发出去的，客户端不等服务端的裁决就认为握手
	 *    结束了；服务端拒绝时发的是一条**握手后**的 alert，要等下一次读写
	 *    才会看到。（TLS 1.2 不是这样，所以照 1.2 的直觉写就会踩。）
	 *
	 *    不补这一下的症状极其难认：OpenDevice 返回成功，第一条请求"发出去了"，
	 *    然后读回来的是 alert 的字节 —— 上层把它当成响应解析，打印出一串乱码，
	 *    再往后全是 COMMFAIL。实测就是这么栽的一次（板子时钟比证书的
	 *    notBefore 早，服务端判"证书尚未生效"）。
	 *
	 *    所以这里主动做一次 OP_PING：一个真实的来回，把服务端的裁决逼出来。
	 *    代价是开设备时多一个 RTT，换来的是"OpenDevice 成功 = 这条通道真的能用"。 */
	{
		uint8_t probe[256];
		uint32_t plen = 0;
		int prv = call(d, OP_PING, 0, 0, NULL, 0, probe, sizeof probe, &plen);

		if (prv != SDR_OK) {
			pqcs_tls_last_error(err, sizeof err);
			fprintf(stderr,
				"握手之后的第一个来回失败 —— 多半是**对端拒绝了我们的证书**"
				"（TLS 1.3 的拒绝是握手后才送到的）。\n"
				"常见原因：客户端证书不是这台设备的 CA 签的；"
				"CN 不在板上的 ACL 里；两边时钟差太多导致证书\"尚未生效\"。\n"
				"TLS: %s\n", err[0] ? err : "(无更多信息)");
			dev_free(d);
			return SDR_AUTHFAIL;
		}
	}
	*phDev = d;
	return SDR_OK;
}

int SDFE_CloseDevice(SDFE_HANDLE hDev)
{
	struct dev *d = hDev;

	if (!d)
		return SDR_INARGERR;
	dev_free(d);
	return SDR_OK;
}

/* 会话：本原型里一个连接就是一个会话，句柄直接透传。
 * 真正的多会话在 daemon 侧（见 docs/API.md §5）。 */
int SDFE_OpenSession(SDFE_HANDLE hDev, SDFE_HANDLE *phSession)
{
	if (!hDev || !phSession)
		return SDR_INARGERR;
	*phSession = hDev;
	return SDR_OK;
}
int SDFE_CloseSession(SDFE_HANDLE hSession) { (void)hSession; return SDR_OK; }

int SDFE_GetDeviceInfo(SDFE_HANDLE hSession, char *buf, size_t cap)
{
	uint32_t n = 0;
	int rv = call(hSession, OP_PING, 0, 0, NULL, 0,
		      (uint8_t *)buf, (uint32_t)cap - 1, &n);

	if (rv == SDR_OK)
		buf[n] = 0;
	return rv;
}

int SDFE_GenerateRandom(SDFE_HANDLE hSession, uint32_t len, uint8_t *out)
{
	return call(hSession, OP_RANDOM, len, 0, NULL, 0, out, len, NULL);
}

int SDFE_GenerateKeyPair_MLKEM(SDFE_HANDLE hSession, uint32_t pset,
			       uint8_t *ek, uint32_t *ek_len,
			       uint32_t *key_handle)
{
	uint8_t buf[2048];
	uint32_t n = 0;
	int rv;

	if (!ek || !ek_len || !key_handle)
		return SDR_INARGERR;
	rv = call(hSession, OP_MLKEM_KEYGEN, pset, 0, NULL, 0,
		  buf, sizeof buf, &n);
	if (rv != SDR_OK)
		return rv;
	if (n < 4)
		return SDR_COMMFAIL;
	memcpy(key_handle, buf, 4);
	if (n - 4 > *ek_len)
		return SDR_INARGERR;
	memcpy(ek, buf + 4, n - 4);
	*ek_len = n - 4;
	return SDR_OK;
}

int SDFE_Encapsulate_MLKEM(SDFE_HANDLE hSession, uint32_t pset,
			   const uint8_t *ek, uint32_t ek_len,
			   uint8_t *ss, uint32_t *ss_len,
			   uint8_t *ct, uint32_t *ct_len)
{
	uint8_t buf[2048];
	uint32_t n = 0;
	int rv = call(hSession, OP_MLKEM_ENCAPS, pset, 0, ek, ek_len,
		      buf, sizeof buf, &n);

	if (rv != SDR_OK)
		return rv;
	if (n < 32 || n - 32 > *ct_len || *ss_len < 32)
		return SDR_INARGERR;
	memcpy(ss, buf, 32);
	*ss_len = 32;
	memcpy(ct, buf + 32, n - 32);
	*ct_len = n - 32;
	return SDR_OK;
}

int SDFE_Decapsulate_MLKEM(SDFE_HANDLE hSession, uint32_t key_handle,
			   const uint8_t *ct, uint32_t ct_len,
			   uint8_t *ss, uint32_t *ss_len)
{
	uint32_t n = 0;
	int rv = call(hSession, OP_MLKEM_DECAPS, key_handle, 0, ct, ct_len,
		      ss, *ss_len, &n);

	if (rv == SDR_OK)
		*ss_len = n;
	return rv;
}

/* ---- ML-DSA ---------------------------------------------------------------
 * 三个函数都只做打包/拆包，一行密码运算都没有（文件头第一段）。
 * 从机已落地（槽 6），2026-08-17 起这三个函数在真硬件上跑通（见 sdf_demo 第 [6] 段）。 */

/* pk 最大 2592（ML-DSA-87），加 4 字节句柄 */
#define SDFE_MLDSA_PK_MAX  2592
/* sig 最大 4627（ML-DSA-87） */
#define SDFE_MLDSA_SIG_MAX 4627

static const uint32_t sdfe_mldsa_pk[3]  = { 1312, 1952, 2592 };
static const uint32_t sdfe_mldsa_sig[3] = { 2420, 3309, 4627 };

int SDFE_GenerateKeyPair_MLDSA(SDFE_HANDLE hSession, uint32_t pset,
			       uint8_t *pk, uint32_t *pk_len,
			       uint32_t *key_handle)
{
	uint8_t buf[4 + SDFE_MLDSA_PK_MAX];
	uint32_t n = 0;
	int rv;

	if (!pk || !pk_len || !key_handle || pset > 2)
		return SDR_INARGERR;
	rv = call(hSession, OP_MLDSA_KEYGEN, pset, 0, NULL, 0,
		  buf, sizeof buf, &n);
	if (rv != SDR_OK)
		return rv;
	if (n < 4)
		return SDR_COMMFAIL;
	memcpy(key_handle, buf, 4);
	if (n - 4 > *pk_len)
		return SDR_INARGERR;
	/* 长度对不上就别把它当成公钥交出去：这里是软件侧唯一能自己核对
	 * "sk 没跟着出来"的地方（daemon 侧也断言了一遍，两处都要有）。 */
	if (n - 4 != sdfe_mldsa_pk[pset])
		return SDR_HARDFAIL;
	memcpy(pk, buf + 4, n - 4);
	*pk_len = n - 4;
	return SDR_OK;
}

int SDFE_Sign_MLDSA(SDFE_HANDLE hSession, uint32_t key_handle,
		    const uint8_t *msg, uint32_t msg_len,
		    const uint8_t *ctx, uint32_t ctx_len,
		    uint8_t *sig, uint32_t *sig_len)
{
	uint32_t n = 0;
	int rv;

	if (!sig || !sig_len || (!msg && msg_len))
		return SDR_INARGERR;
	if (ctx_len)                       /* 非空 ctx：见 sdfe.h 那段 */
		return SDR_INARGERR;
	(void)ctx;
	if (msg_len > PQCS_MAXPAY)
		return SDR_INARGERR;
	rv = call(hSession, OP_MLDSA_SIGN, key_handle, ctx_len,
		  msg, msg_len, sig, *sig_len, &n);
	if (rv != SDR_OK)
		return rv;
	*sig_len = n;
	return SDR_OK;
}

int SDFE_Verify_MLDSA(SDFE_HANDLE hSession, uint32_t pset,
		      const uint8_t *pk, uint32_t pk_len,
		      const uint8_t *msg, uint32_t msg_len,
		      const uint8_t *ctx, uint32_t ctx_len,
		      const uint8_t *sig, uint32_t sig_len)
{
	uint32_t total, n = 0;
	uint8_t *in;
	int rv;

	if (!pk || !sig || (!msg && msg_len) || pset > 2)
		return SDR_INARGERR;
	if (ctx_len)                       /* 非空 ctx：见 sdfe.h 那段 */
		return SDR_INARGERR;
	(void)ctx;
	if (pk_len != sdfe_mldsa_pk[pset] || sig_len != sdfe_mldsa_sig[pset])
		return SDR_INARGERR;
	if ((uint64_t)pk_len + sig_len + msg_len > PQCS_MAXPAY)
		return SDR_INARGERR;
	total = pk_len + sig_len + msg_len;
	/* 拼接缓冲最大 15 KB：不放栈上（板上线程栈不宽裕），也不放 static ——
	 * 本库对外声称无状态，一个函数级 static 会让多线程调用方悄悄互相踩。 */
	in = malloc(total);
	if (!in)
		return SDR_UNKNOWERR;
	/* 硬件要的字节流就是 pk‖sig‖msg，这里拼一次、原样送下去 */
	memcpy(in, pk, pk_len);
	memcpy(in + pk_len, sig, sig_len);
	if (msg_len)
		memcpy(in + pk_len + sig_len, msg, msg_len);
	/* 验不过时 daemon 回 SDR_VERIFYFAIL，原样透传：调用方按 != SDR_OK 判即可 */
	rv = call(hSession, OP_MLDSA_VERIFY, pset, ctx_len,
		  in, total, NULL, 0, &n);
	free(in);
	return rv;
}

int SDFE_ImportKey(SDFE_HANDLE hSession, uint32_t slot,
		   const uint8_t *key, uint32_t key_len)
{
	return call(hSession, OP_IMPORT_KEY, slot, 0, key, key_len,
		    NULL, 0, NULL);
}

static int sym(SDFE_HANDLE h, uint32_t alg, uint32_t slot, int dec,
	       const uint8_t *in, uint8_t *out)
{
	uint32_t n = 0;

	return call(h, OP_SYM_BLOCK, alg, slot | ((uint32_t)dec << 8),
		    in, 16, out, 16, &n);
}

int SDFE_Encrypt(SDFE_HANDLE h, uint32_t alg, uint32_t slot,
		 const uint8_t *in, uint8_t *out)
{
	return sym(h, alg, slot, 0, in, out);
}
int SDFE_Decrypt(SDFE_HANDLE h, uint32_t alg, uint32_t slot,
		 const uint8_t *in, uint8_t *out)
{
	return sym(h, alg, slot, 1, in, out);
}

/* ---- 工作模式 ----
 * 一次调用一段数据：IV 与数据拼成一个载荷发过去，模式状态留在 daemon 里。
 * 逐分组往返也能实现，但那会把反馈寄存器/计数器交到调用方手里 ——
 * 而调用方一旦复用它，CTR 那三种就是密钥流复用。 */
static int sym_mode(SDFE_HANDLE h, uint32_t alg, uint32_t mode, uint32_t slot,
                    int dec, const uint8_t *iv, const uint8_t *in,
                    uint32_t len, uint8_t *out)
{
	uint8_t req[PQCS_MAXPAY];
	uint32_t n = 0;
	int rv;

	if (!iv || !in || !out || len == 0)
		return SDR_INARGERR;
	if ((uint32_t)len + 16u > (uint32_t)sizeof req)
		return SDR_INARGERR;
	memcpy(req, iv, 16);
	memcpy(req + 16, in, len);
	rv = call(h, OP_SYM_CRYPT,
	          alg | ((uint32_t)(dec ? 1 : 0) << 8) | (mode << 16),
	          slot, req, len + 16u, out, len, &n);
	/* 载荷里有明文（或密文），用完抹掉本地这份。
	 * volatile 指针写，防 -O2 把"写完就不再读"的 memset 优化掉 ——
	 * tests/unit/test_zeroize.c 证过裸 memset 会被消除。 */
	{
		volatile uint8_t *z = req;
		size_t i;

		for (i = 0; i < sizeof req; i++)
			z[i] = 0;
	}
	if (rv != SDR_OK)
		return rv;
	if (n != len)
		return SDR_UNKNOWERR;
	return SDR_OK;
}

int SDFE_EncryptMode(SDFE_HANDLE h, uint32_t alg, uint32_t mode, uint32_t slot,
                     const uint8_t iv[16], const uint8_t *in, uint32_t len,
                     uint8_t *out)
{
	return sym_mode(h, alg, mode, slot, 0, iv, in, len, out);
}

int SDFE_DecryptMode(SDFE_HANDLE h, uint32_t alg, uint32_t mode, uint32_t slot,
                     const uint8_t iv[16], const uint8_t *in, uint32_t len,
                     uint8_t *out)
{
	return sym_mode(h, alg, mode, slot, 1, iv, in, len, out);
}

int SDFE_Hash_SM3(SDFE_HANDLE h, const uint8_t *in, uint32_t len, uint8_t out[32])
{
	uint32_t n = 0;
	int rv;

	if (!in || !out || len == 0)
		return SDR_INARGERR;
	rv = call(h, OP_SM3, 0, 0, in, len, out, 32, &n);
	if (rv != SDR_OK)
		return rv;
	return (n == 32) ? SDR_OK : SDR_UNKNOWERR;
}

int SDFE_GetDeviceDNA(SDFE_HANDLE h, uint8_t out[16])
{
	uint32_t n = 0;
	int rv;

	if (!out)
		return SDR_INARGERR;
	rv = call(h, OP_DEVICE_DNA, 0, 0, NULL, 0, out, 16, &n);
	if (rv != SDR_OK)
		return rv;
	return (n == 16) ? SDR_OK : SDR_UNKNOWERR;
}

const char *SDFE_StrError(int rv)
{
	switch ((unsigned)rv) {
	case SDR_OK:          return "成功";
	case SDR_OPENDEVICE:  return "连不上密码机服务（daemon 没起？）";
	case SDR_COMMFAIL:    return "通信失败";
	case SDR_INARGERR:    return "参数错误";
	case SDR_KEYNOTEXIST: return "句柄不存在";
	case SDR_HARDFAIL:    return "硬件运算失败";
	case SDR_AUTHFAIL:    return "远程通道认证失败（证书/身份/时钟）";
	case SDR_VERIFYFAIL:  return "验签不通过（是结果，不是故障）";
	default:              return "未知错误";
	}
}
