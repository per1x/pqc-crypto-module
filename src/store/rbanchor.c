/* rbanchor.c —— 防回滚锚点的抽象与默认（文件）实现。设计见 rbanchor.h。 */
#include "pqchsm/rbanchor.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ---- 文件 provider ------------------------------------------------------ */

static void anchor_path(const char *scope, char *out, size_t cap)
{
	snprintf(out, cap, "%s.epoch", scope ? scope : "keystore");
}

static int file_read(void *user, const char *scope, uint64_t *out)
{
	char p[512];
	unsigned long long v = 0;
	FILE *f;

	(void)user;
	if (!out) {
		return -1;
	}
	anchor_path(scope, p, sizeof p);
	f = fopen(p, "r");
	if (!f) {
		/* 读不到按 0 处理 —— 首次运行的正常情形。**删掉锚点文件**因此
		 * 是一次可观测的降级（epoch 会从 1 重新开始），keystore 那边会记审计。 */
		*out = 0;
		return 0;
	}
	if (fscanf(f, "%llu", &v) != 1) {
		v = 0;
	}
	fclose(f);
	*out = (uint64_t)v;
	return 0;
}

/* 写：先写临时文件再 rename，掉电不会留下半个数。 */
static int file_write(const char *scope, uint64_t v)
{
	char p[512], t[544];
	FILE *f;

	anchor_path(scope, p, sizeof p);
	snprintf(t, sizeof t, "%s.tmp", p);
	f = fopen(t, "w");
	if (!f) {
		return -1;
	}
	if (fprintf(f, "%llu\n", (unsigned long long)v) < 0) {
		fclose(f);
		remove(t);
		return -1;
	}
	fflush(f);
	fsync(fileno(f));
	fclose(f);
	if (rename(t, p) != 0) {
		remove(t);
		return -1;
	}
	return 0;
}

static int file_bump(void *user, const char *scope, uint64_t *out)
{
	uint64_t cur = 0;

	if (file_read(user, scope, &cur) != 0) {
		return -1;
	}
	cur++;
	if (file_write(scope, cur) != 0) {
		return -1;
	}
	if (out) {
		*out = cur;
	}
	return 0;
}

static const pqc_rbanchor_provider_t g_file = {
	.name               = "file(<keystore>.epoch, NOT hardware-monotonic)",
	.hardware_monotonic = 0,
	.read               = file_read,
	.bump               = file_bump,
	.user               = NULL,
};

const pqc_rbanchor_provider_t *pqc_rbanchor_provider_file(void)
{
	return &g_file;
}

/* ---- 当前 provider ------------------------------------------------------ */

static const pqc_rbanchor_provider_t *g_provider;

void pqc_rbanchor_set_provider(const pqc_rbanchor_provider_t *p)
{
	g_provider = p;
}

const pqc_rbanchor_provider_t *pqc_rbanchor_get_provider(void)
{
	if (!g_provider) {
		g_provider = &g_file;
	}
	return g_provider;
}

int pqc_rbanchor_is_hardware_monotonic(void)
{
	const pqc_rbanchor_provider_t *p = pqc_rbanchor_get_provider();

	return p && p->hardware_monotonic;
}

int pqc_rbanchor_read(const char *scope, uint64_t *out)
{
	const pqc_rbanchor_provider_t *p = pqc_rbanchor_get_provider();

	if (!p || !p->read || !out) {
		return -1;
	}
	return p->read(p->user, scope, out);
}

int pqc_rbanchor_bump(const char *scope, uint64_t *out)
{
	const pqc_rbanchor_provider_t *p = pqc_rbanchor_get_provider();

	if (!p || !p->bump) {
		return -1;
	}
	return p->bump(p->user, scope, out);
}
