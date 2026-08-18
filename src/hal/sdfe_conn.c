#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "sdfe_conn.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static SDFE_HANDLE g_dev, g_ses;

void sdfe_conn_lock(void)   { pthread_mutex_lock(&g_lock); }
void sdfe_conn_unlock(void) { pthread_mutex_unlock(&g_lock); }

void sdfe_conn_drop(void)
{
	if (g_ses) { SDFE_CloseSession(g_ses); g_ses = NULL; }
	if (g_dev) { SDFE_CloseDevice(g_dev);  g_dev = NULL; }
}

SDFE_HANDLE sdfe_conn_get(void)
{
	const char *host = getenv("PQCHSM_SDFE_HOST");
	/* ⚠️ 以前是 PQCHSM_SDFE_TOKEN（一条明文口令）。远程口改成 mTLS 之后
	 * 那个变量**不再有任何作用**，取而代之的是一个凭据目录，里面要有
	 * hsm_ca.crt / client.crt / client.key（与 sdf_demo 用的是同一套）。
	 * 老变量不做兼容：留着它会让人以为设了就生效，而其实连不上。 */
	const char *pki  = getenv("PQCHSM_SDFE_PKI");
	const char *ps   = getenv("PQCHSM_SDFE_PORT");
	const char *dcn  = getenv("PQCHSM_SDFE_DEVICE_CN");
	SDFE_TLS_CREDS creds;
	/* 无需清零：这三个是**文件路径**（.../client.key 这种字符串），
	 * 不是密钥材料。密钥本身从头到尾在 OpenSSL 里，本函数一个字节都没碰过。
	 * 变量名里的 key 只是"哪个文件"的意思。 */
	char cred_ca[512], cred_crt[512], cred_key[512];
	int rv;

	if (g_ses)
		return g_ses;
	if (host && pki) {
		snprintf(cred_ca,  sizeof cred_ca,  "%s/hsm_ca.crt", pki);
		snprintf(cred_crt, sizeof cred_crt, "%s/client.crt", pki);
		snprintf(cred_key, sizeof cred_key, "%s/client.key", pki);
		creds.ca_file   = cred_ca;
		creds.cert_file = cred_crt;
		creds.key_file  = cred_key;
		creds.expect_cn = dcn;          /* 不设就只验签发链 */
		rv = SDFE_OpenDeviceRemote(&g_dev, host, ps ? atoi(ps) : 0, &creds);
	} else {
		rv = SDFE_OpenDevice(&g_dev);
	}
	if (rv != SDR_OK) { g_dev = NULL; return NULL; }
	if (SDFE_OpenSession(g_dev, &g_ses) != SDR_OK) { sdfe_conn_drop(); return NULL; }
	return g_ses;
}
