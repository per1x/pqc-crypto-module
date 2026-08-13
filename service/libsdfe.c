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
 * 真正的多会话在 daemon 侧（见 docs/服务层设计.md §5）。 */
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
	default:              return "未知错误";
	}
}
