#define _GNU_SOURCE
#include <pthread.h>
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
	const char *tok  = getenv("PQCHSM_SDFE_TOKEN");
	const char *ps   = getenv("PQCHSM_SDFE_PORT");
	int rv;

	if (g_ses)
		return g_ses;
	rv = (host && tok)
	     ? SDFE_OpenDeviceRemote(&g_dev, host, ps ? atoi(ps) : 0, tok)
	     : SDFE_OpenDevice(&g_dev);
	if (rv != SDR_OK) { g_dev = NULL; return NULL; }
	if (SDFE_OpenSession(g_dev, &g_ses) != SDR_OK) { sdfe_conn_drop(); return NULL; }
	return g_ses;
}
