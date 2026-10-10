/* hsmops.c —— 见 hsmops.h */
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "hsmops.h"
#include "sdfe.h"

#define SYM_SLOT 3                /* 每次会话把对称密钥装进槽 3 */
#define CHUNK    16368u           /* 单次请求数据上限（MAXPAY 16384 减 16 字节 IV） */
#define FILE_MAX (sizeof(hsm_key) + 32)

static char g_err[256];
static SDFE_HANDLE g_dev, g_ses;

static int fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(g_err, sizeof g_err, fmt, ap);
	va_end(ap);
	return -1;
}

static int fail_rv(const char *what, int rv)
{
	return fail("%s：%s", what, SDFE_StrError(rv));
}

const char *hsm_err(void) { return g_err; }

const char *hsm_keystore(void)
{
	const char *d = getenv("HSM_KEYSTORE");
	return d && *d ? d : "keystore";
}

static int ses(SDFE_HANDLE *out)
{
	int rv;

	if (!g_ses) {
		if ((rv = SDFE_OpenDevice(&g_dev)) != SDR_OK)
			return fail_rv("连不上密码机", rv);
		if ((rv = SDFE_OpenSession(g_dev, &g_ses)) != SDR_OK) {
			SDFE_CloseDevice(g_dev);
			g_dev = NULL;
			return fail_rv("开不了会话", rv);
		}
	}
	*out = g_ses;
	return 0;
}

void hsm_close(void)
{
	if (g_ses) {
		SDFE_CloseSession(g_ses);
		SDFE_CloseDevice(g_dev);
		g_ses = g_dev = NULL;
	}
}

int hsm_name_ok(const char *n)
{
	size_t i, l = n ? strlen(n) : 0;

	if (l == 0 || l > 32)
		return 0;
	for (i = 0; i < l; i++) {
		char c = n[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '_' || c == '-'))
			return 0;
	}
	return 1;
}

static int key_path(char *out, size_t cap, const char *name)
{
	if (!hsm_name_ok(name))
		return fail("密钥名不合法（只许字母数字 _ -，1-32 个字符）");
	snprintf(out, cap, "%s/%s.key", hsm_keystore(), name);
	return 0;
}

const char *hsm_alg_name(const hsm_key *k)
{
	static const char *const mk[3] = { "ML-KEM-512", "ML-KEM-768", "ML-KEM-1024" };
	static const char *const md[3] = { "ML-DSA-44", "ML-DSA-65", "ML-DSA-87" };
	static const char *const sy[3] = { "AES-128", "AES-256", "SM4" };

	if (k->kind == HSM_KIND_MLKEM && k->param < 3) return mk[k->param];
	if (k->kind == HSM_KIND_MLDSA && k->param < 3) return md[k->param];
	if (k->kind == HSM_KIND_SYM && k->param < 3) return sy[k->param];
	return "?";
}

/* ---- 密钥文件 ---- */
static int key_save(const char *name, const hsm_key *k)
{
	char p[512];
	uint8_t buf[FILE_MAX];
	uint32_t n = 0;
	FILE *f;

	if (key_path(p, sizeof p, name))
		return -1;
	mkdir(hsm_keystore(), 0700);
	memcpy(buf, "PQK1", 4); n = 4;
	buf[n++] = k->kind; buf[n++] = k->param;
	memcpy(buf + n, &k->sym_len, 2); n += 2;
	memcpy(buf + n, &k->created, 4); n += 4;
	memcpy(buf + n, &k->pk_len, 4); n += 4;
	memcpy(buf + n, &k->blob_len, 4); n += 4;
	memcpy(buf + n, k->pk, k->pk_len); n += k->pk_len;
	memcpy(buf + n, k->blob, k->blob_len); n += k->blob_len;
	f = fopen(p, "wb");
	if (!f)
		return fail("写不了 %s：%s", p, strerror(errno));
	if (fwrite(buf, 1, n, f) != n) {
		fclose(f);
		return fail("写 %s 失败", p);
	}
	fclose(f);
	chmod(p, 0600);
	return 0;
}

int hsm_key_load(const char *name, hsm_key *k)
{
	char p[512];
	uint8_t b[FILE_MAX];
	uint32_t o = 20;
	size_t n;
	FILE *f;

	if (key_path(p, sizeof p, name))
		return -1;
	f = fopen(p, "rb");
	if (!f)
		return fail("没有这把密钥：%s", name);
	n = fread(b, 1, sizeof b, f);
	fclose(f);
	if (n < 20 || memcmp(b, "PQK1", 4))
		return fail("密钥文件格式不对：%s", name);
	k->kind = b[4]; k->param = b[5];
	memcpy(&k->sym_len, b + 6, 2);
	memcpy(&k->created, b + 8, 4);
	memcpy(&k->pk_len, b + 12, 4);
	memcpy(&k->blob_len, b + 16, 4);
	if (k->pk_len > sizeof k->pk || k->blob_len > sizeof k->blob ||
	    o + k->pk_len + k->blob_len != n)
		return fail("密钥文件格式不对：%s", name);
	memcpy(k->pk, b + o, k->pk_len); o += k->pk_len;
	memcpy(k->blob, b + o, k->blob_len);
	return 0;
}

int hsm_key_gen(const char *alg, const char *name, hsm_key *k)
{
	char p[512];
	SDFE_HANDLE s;
	int rv;

	if (key_path(p, sizeof p, name))
		return -1;
	if (access(p, F_OK) == 0)
		return fail("同名密钥已存在：%s", name);
	memset(k, 0, sizeof *k);
	k->created = (uint32_t)time(NULL);
	if (!strcmp(alg, "sm4") || !strcmp(alg, "aes128") || !strcmp(alg, "aes256")) {
		k->kind = HSM_KIND_SYM;
		k->param = !strcmp(alg, "sm4") ? SDFE_ALG_SM4 :
		           !strcmp(alg, "aes128") ? SDFE_ALG_AES128 : SDFE_ALG_AES256;
		k->sym_len = k->param == SDFE_ALG_AES256 ? 32 : 16;
		k->blob_len = sizeof k->blob;
		if (ses(&s))
			return -1;
		rv = SDFE_GenerateKey(s, SYM_SLOT, k->sym_len, k->blob, &k->blob_len);
		if (rv != SDR_OK)
			return fail_rv("生成对称密钥", rv);
	} else if (!strcmp(alg, "mlkem512") || !strcmp(alg, "mlkem768") ||
	           !strcmp(alg, "mlkem1024") || !strcmp(alg, "mldsa44") ||
	           !strcmp(alg, "mldsa65") || !strcmp(alg, "mldsa87")) {
		int kem = alg[2] == 'k';
		const char *ps = alg + 5;
		uint32_t h, pset = !strcmp(ps, "512") || !strcmp(ps, "44") ? 0 :
		                   !strcmp(ps, "768") || !strcmp(ps, "65") ? 1 : 2;

		k->kind = kem ? HSM_KIND_MLKEM : HSM_KIND_MLDSA;
		k->param = (uint8_t)pset;
		k->pk_len = sizeof k->pk;
		if (ses(&s))
			return -1;
		rv = kem ? SDFE_GenerateKeyPair_MLKEM(s, pset, k->pk, &k->pk_len, &h)
		         : SDFE_GenerateKeyPair_MLDSA(s, pset, k->pk, &k->pk_len, &h);
		if (rv != SDR_OK)
			return fail_rv("生成密钥对", rv);
		k->blob_len = sizeof k->blob;
		rv = SDFE_ExportKeyBlob(s, k->kind, h, k->blob, &k->blob_len);
		if (rv != SDR_OK)
			return fail_rv("导出种子 blob", rv);
	} else {
		return fail("不认识的算法：%s（sm4 aes128 aes256 mlkem512/768/1024 mldsa44/65/87）", alg);
	}
	return key_save(name, k);
}

int hsm_key_destroy(const char *name)
{
	char p[512];
	uint8_t z[FILE_MAX];
	long n;
	FILE *f;

	if (key_path(p, sizeof p, name))
		return -1;
	f = fopen(p, "r+b");
	if (!f)
		return fail("没有这把密钥：%s", name);
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	memset(z, 0, sizeof z);
	if (n > 0 && (size_t)n <= sizeof z)
		fwrite(z, 1, (size_t)n, f);          /* 先覆写再删除 */
	fclose(f);
	unlink(p);
	return 0;
}

int hsm_key_list(void (*cb)(const char *, const hsm_key *, void *), void *u)
{
	DIR *d = opendir(hsm_keystore());
	struct dirent *e;
	int n = 0;

	if (!d)
		return 0;
	while ((e = readdir(d))) {
		size_t l = strlen(e->d_name);
		char name[64];
		hsm_key k;

		if (l < 5 || l > 36 || strcmp(e->d_name + l - 4, ".key"))
			continue;
		snprintf(name, sizeof name, "%.*s", (int)(l - 4), e->d_name);
		if (hsm_key_load(name, &k))
			continue;
		cb(name, &k, u);
		n++;
	}
	closedir(d);
	return n;
}

/* ---- 运算 ---- */
static int load_kind(const char *name, hsm_key *k, int kind, const char *what)
{
	if (hsm_key_load(name, k))
		return -1;
	if (k->kind != kind)
		return fail("「%s」不是%s", name, what);
	return 0;
}

static int sym_load(SDFE_HANDLE s, const hsm_key *k)
{
	if (SDFE_LoadKey(s, SYM_SLOT, k->blob, k->blob_len) != SDR_OK)
		return fail("TA 拒绝装载这把密钥（blob 被改过，或不是本机生成的）");
	return 0;
}

static int pqc_handle(SDFE_HANDLE s, const hsm_key *k, uint32_t *h)
{
	int rv = SDFE_ImportKeyBlob(s, k->kind, k->param, k->blob, k->blob_len, h);
	return rv == SDR_OK ? 0 : fail_rv("导入种子 blob", rv);
}

int hsm_encrypt(const char *name, const uint8_t *in, uint32_t n, uint8_t **out, uint32_t *outlen)
{
	hsm_key k;
	SDFE_HANDLE s;
	uint8_t iv[16], *src, *dst;
	uint32_t pad, len, off = 0, i;
	int rv;

	if (load_kind(name, &k, HSM_KIND_SYM, "对称密钥") || ses(&s) || sym_load(s, &k))
		return -1;
	if ((rv = SDFE_GenerateRandom(s, 16, iv)) != SDR_OK)
		return fail_rv("取 IV", rv);
	pad = 16 - n % 16;
	len = n + pad;
	src = malloc(len);
	dst = malloc(len + 16);
	if (!src || !dst) { free(src); free(dst); return fail("内存不足"); }
	memcpy(src, in, n);
	for (i = 0; i < pad; i++)
		src[n + i] = (uint8_t)pad;                  /* PKCS#7 */
	memcpy(dst, iv, 16);
	while (off < len) {
		uint32_t c = len - off > CHUNK ? CHUNK : len - off;
		rv = SDFE_EncryptMode(s, k.param, SDFE_MODE_CBC, SYM_SLOT,
		                      off ? dst + off : iv, src + off, c, dst + 16 + off);
		if (rv != SDR_OK) { free(src); free(dst); return fail_rv("加密", rv); }
		off += c;
	}
	free(src);
	*out = dst;
	*outlen = len + 16;
	return 0;
}

int hsm_decrypt(const char *name, const uint8_t *in, uint32_t n, uint8_t **out, uint32_t *outlen)
{
	hsm_key k;
	SDFE_HANDLE s;
	uint8_t *dst, pad;
	uint32_t len, off = 0, i;
	int rv;

	if (n < 32 || (n - 16) % 16)
		return fail("密文长度不对（应为 16 字节 IV + 16 的整数倍）");
	if (load_kind(name, &k, HSM_KIND_SYM, "对称密钥") || ses(&s) || sym_load(s, &k))
		return -1;
	len = n - 16;
	dst = malloc(len);
	if (!dst)
		return fail("内存不足");
	while (off < len) {
		uint32_t c = len - off > CHUNK ? CHUNK : len - off;
		rv = SDFE_DecryptMode(s, k.param, SDFE_MODE_CBC, SYM_SLOT,
		                      in + off, in + 16 + off, c, dst + off);
		if (rv != SDR_OK) { free(dst); return fail_rv("解密", rv); }
		off += c;
	}
	pad = dst[len - 1];
	if (pad == 0 || pad > 16) { free(dst); return fail("解密失败（填充不对：密钥不对或密文被改）"); }
	for (i = 0; i < pad; i++)
		if (dst[len - 1 - i] != pad) {
			free(dst);
			return fail("解密失败（填充不对：密钥不对或密文被改）");
		}
	*out = dst;
	*outlen = len - pad;
	return 0;
}

int hsm_sign(const char *name, const uint8_t *m, uint32_t n, uint8_t *sig, uint32_t *siglen)
{
	hsm_key k;
	SDFE_HANDLE s;
	uint32_t h;
	int rv;

	if (n > 8192)
		return fail("消息太长（≤ 8192 字节）");
	if (load_kind(name, &k, HSM_KIND_MLDSA, "签名密钥") || ses(&s) || pqc_handle(s, &k, &h))
		return -1;
	if ((rv = SDFE_Sign_MLDSA(s, h, m, n, NULL, 0, sig, siglen)) != SDR_OK)
		return fail_rv("签名", rv);
	return 0;
}

int hsm_verify(const char *name, const uint8_t *m, uint32_t n, const uint8_t *sig, uint32_t sl)
{
	hsm_key k;
	SDFE_HANDLE s;
	int rv;

	if (load_kind(name, &k, HSM_KIND_MLDSA, "签名密钥") || ses(&s))
		return -1;
	rv = SDFE_Verify_MLDSA(s, k.param, k.pk, k.pk_len, m, n, NULL, 0, sig, sl);
	if (rv == SDR_OK)
		return 0;
	if (rv == SDR_VERIFYFAIL)
		return HSM_NO;
	return fail_rv("验签", rv);
}

int hsm_encap(const char *name, uint8_t *ct, uint32_t *ctlen, uint8_t ss[32])
{
	hsm_key k;
	SDFE_HANDLE s;
	uint32_t sl = 32;
	int rv;

	if (load_kind(name, &k, HSM_KIND_MLKEM, "ML-KEM 密钥") || ses(&s))
		return -1;
	rv = SDFE_Encapsulate_MLKEM(s, k.param, k.pk, k.pk_len, ss, &sl, ct, ctlen);
	return rv == SDR_OK ? 0 : fail_rv("封装", rv);
}

int hsm_decap(const char *name, const uint8_t *ct, uint32_t ctlen, uint8_t ss[32])
{
	hsm_key k;
	SDFE_HANDLE s;
	uint32_t sl = 32, h;
	int rv;

	if (load_kind(name, &k, HSM_KIND_MLKEM, "ML-KEM 密钥") || ses(&s) || pqc_handle(s, &k, &h))
		return -1;
	rv = SDFE_Decapsulate_MLKEM(s, h, ct, ctlen, ss, &sl);
	return rv == SDR_OK ? 0 : fail_rv("解封装", rv);
}

int hsm_sm3(const uint8_t *d, uint32_t n, uint8_t out[32])
{
	SDFE_HANDLE s;
	int rv;

	if (n > 16384)
		return fail("数据太长（≤ 16384 字节）");
	if (ses(&s))
		return -1;
	rv = SDFE_Hash_SM3(s, d, n, out);
	return rv == SDR_OK ? 0 : fail_rv("SM3", rv);
}

int hsm_random(uint32_t n, uint8_t *out)
{
	SDFE_HANDLE s;
	int rv;

	if (n == 0 || n > 1024)
		return fail("字节数应为 1..1024");
	if (ses(&s))
		return -1;
	rv = SDFE_GenerateRandom(s, n, out);
	return rv == SDR_OK ? 0 : fail_rv("取随机数", rv);
}

int hsm_info(char *buf, size_t cap)
{
	SDFE_HANDLE s;

	if (ses(&s))
		return -1;
	SDFE_GetDeviceInfo(s, buf, cap);
	return 0;
}
