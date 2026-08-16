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

struct dev { int fd; };

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

static int call(struct dev *d, uint32_t op, uint32_t a0, uint32_t a1,
		const uint8_t *in, uint32_t in_len,
		uint8_t *out, uint32_t out_cap, uint32_t *out_len)
{
	struct pqcs_req q = { PQCS_MAGIC, op, a0, a1, in_len };
	struct pqcs_resp rp;

	if (!d || d->fd < 0)
		return SDR_OPENDEVICE;
	if (rw_all(d->fd, &q, sizeof q, 1))
		return SDR_COMMFAIL;
	if (in_len && rw_all(d->fd, (void *)in, in_len, 1))
		return SDR_COMMFAIL;
	if (rw_all(d->fd, &rp, sizeof rp, 0))
		return SDR_COMMFAIL;
	if (rp.magic != PQCS_MAGIC)
		return SDR_COMMFAIL;
	if (rp.len > out_cap)
		return SDR_INARGERR;
	if (rp.len && rw_all(d->fd, out, rp.len, 0))
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

/* 远程打开：连另一台机器上的密码机。
 *
 * 连上之后先发一条 OP_AUTH。**这一步失败就把 fd 关掉、返回错误** ——
 * 不留一个"连上了但没认证"的半开句柄：那种句柄的后续调用会一条条被
 * 服务端拒绝并断开，报出来的错是 COMMFAIL，把真正的原因（口令不对）
 * 藏起来了。
 *
 * 除了这个函数，远程和本机对调用方**完全一样** —— 上层不需要知道
 * 自己在跟谁说话，这正是把认证放在 OpenDevice 而不是每次调用里的理由。 */
int SDFE_OpenDeviceRemote(SDFE_HANDLE *phDev, const char *host,
			  int port, const char *token)
{
	struct dev *d;
	struct sockaddr_in sa;
	struct pqcs_req q;
	struct pqcs_resp rp;
	size_t tl;
	int one = 1;

	if (!phDev || !host || !token)
		return SDR_INARGERR;
	tl = strlen(token);
	if (tl < 8 || tl > PQCS_TOKEN_MAX)
		return SDR_INARGERR;

	d = calloc(1, sizeof *d);
	if (!d)
		return SDR_UNKNOWERR;
	d->fd = socket(AF_INET, SOCK_STREAM, 0);
	if (d->fd < 0) { free(d); return SDR_OPENDEVICE; }

	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)(port ? port : PQCS_TCP_PORT));
	if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
		close(d->fd); free(d); return SDR_INARGERR;
	}
	if (connect(d->fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		close(d->fd); free(d); return SDR_OPENDEVICE;
	}
	setsockopt(d->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

	q.magic = PQCS_MAGIC; q.op = OP_AUTH; q.a0 = 0; q.a1 = 0;
	q.len = (uint32_t)tl;
	if (rw_all(d->fd, &q, sizeof q, 1) ||
	    rw_all(d->fd, (void *)token, tl, 1) ||
	    rw_all(d->fd, &rp, sizeof rp, 0)) {
		close(d->fd); free(d); return SDR_COMMFAIL;
	}
	if (rp.magic != PQCS_MAGIC || rp.status != SDR_OK) {
		close(d->fd); free(d);
		return rp.status == SDR_AUTHFAIL ? SDR_AUTHFAIL : SDR_COMMFAIL;
	}
	*phDev = d;
	return SDR_OK;
}

int SDFE_CloseDevice(SDFE_HANDLE hDev)
{
	struct dev *d = hDev;

	if (!d)
		return SDR_INARGERR;
	close(d->fd);
	free(d);
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

const char *SDFE_StrError(int rv)
{
	switch ((unsigned)rv) {
	case SDR_OK:          return "成功";
	case SDR_OPENDEVICE:  return "连不上密码机服务（daemon 没起？）";
	case SDR_COMMFAIL:    return "通信失败";
	case SDR_INARGERR:    return "参数错误";
	case SDR_KEYNOTEXIST: return "句柄不存在";
	case SDR_HARDFAIL:    return "硬件运算失败";
	case SDR_AUTHFAIL:    return "远程口令不对";
	case SDR_VERIFYFAIL:  return "验签不通过（是结果，不是故障）";
	default:              return "未知错误";
	}
}
