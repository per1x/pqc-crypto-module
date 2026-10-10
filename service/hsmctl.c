/* hsmctl —— 密码机命令行：一条命令一个业务动作
 *
 *   hsmctl info                               设备信息
 *   hsmctl keygen <算法> <名字>                生成密钥（sm4 aes128 aes256 mlkem512/768/1024 mldsa44/65/87）
 *   hsmctl list                               列出密钥库
 *   hsmctl show <名字>                        看一把密钥的文件内容（证明里面只有密文 blob）
 *   hsmctl encrypt <名字> <明文> <密文>       对称加密（CBC + PKCS#7，IV 取自 PL TRNG）
 *   hsmctl decrypt <名字> <密文> <明文>
 *   hsmctl sign    <名字> <文件> <签名>       ML-DSA 签名
 *   hsmctl verify  <名字> <文件> <签名>
 *   hsmctl encap   <名字> <封装密文>          ML-KEM 封装（发送方），打印共享密钥
 *   hsmctl decap   <名字> <封装密文>          ML-KEM 解封装（接收方），打印共享密钥
 *   hsmctl hash    <文件>                     SM3
 *   hsmctl random  <字节数>                   PL TRNG
 *   hsmctl backup  <备份包>                   把密钥库打成备份包（每项带 SM3 校验）
 *   hsmctl restore <备份包>                   校验后恢复
 *   hsmctl destroy <名字>                     销毁一把密钥
 *
 * 密钥库是一个目录（$HSM_KEYSTORE，默认 ./keystore），一把密钥一个文件。
 * **文件里没有明文密钥**：对称密钥是 TA 用 KEK 包裹的 blob；后量子密钥是公钥 +
 * TA 包裹的种子 blob。KEK 不出 TA，所以这些文件拷到哪里都只是密文 ——
 * 也只有这块板上的 TA 解得开。
 *
 * 本程序只链接 libsdfe，**一行密码运算都不做**：加密、签名、杂凑、随机数都在
 * 密码机里（密钥在 TEE、运算在 FPGA），这里只做文件读写与 PKCS#7 填充。
 */
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "sdfe.h"

#define KIND_MLKEM 0
#define KIND_MLDSA 1
#define KIND_SYM   2
#define SYM_SLOT   3          /* 本工具每次会话把对称密钥装进槽 3 */
#define CHUNK      16368u     /* 单次请求数据上限（MAXPAY 16384 减 16 字节 IV） */

struct keyfile {
	uint8_t  kind, param;     /* param：ML-KEM/ML-DSA 的 pset，或 SDFE_ALG_* */
	uint16_t sym_len;         /* 对称密钥长度（字节） */
	uint32_t created;
	uint32_t pk_len, blob_len;
	uint8_t  pk[2592];
	uint8_t  blob[256];
};

static const char *ks_dir(void)
{
	const char *d = getenv("HSM_KEYSTORE");
	return d && *d ? d : "keystore";
}

static void die(const char *what, int rv)
{
	if (rv)
		fprintf(stderr, "✗ %s：%s\n", what, SDFE_StrError(rv));
	else
		fprintf(stderr, "✗ %s\n", what);
	exit(1);
}

static SDFE_HANDLE g_dev, g_ses;

static SDFE_HANDLE ses(void)
{
	int rv;

	if (g_ses)
		return g_ses;
	if ((rv = SDFE_OpenDevice(&g_dev)) != SDR_OK)
		die("连不上密码机", rv);
	if ((rv = SDFE_OpenSession(g_dev, &g_ses)) != SDR_OK)
		die("开不了会话", rv);
	return g_ses;
}

/* ---- 文件 ---- */
static uint8_t *slurp(const char *path, uint32_t *len, uint32_t max)
{
	FILE *f = fopen(path, "rb");
	uint8_t *b;
	long n;

	if (!f) { fprintf(stderr, "✗ 打不开 %s：%s\n", path, strerror(errno)); exit(1); }
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	if (n < 0 || (uint32_t)n > max) die("文件太大", 0);
	b = malloc((size_t)n + 32);
	if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) die("读文件失败", 0);
	fclose(f);
	*len = (uint32_t)n;
	return b;
}

static void spill(const char *path, const uint8_t *b, uint32_t len, int secret_mode)
{
	FILE *f = fopen(path, "wb");

	if (!f) { fprintf(stderr, "✗ 写不了 %s：%s\n", path, strerror(errno)); exit(1); }
	if (fwrite(b, 1, len, f) != len) die("写文件失败", 0);
	fclose(f);
	if (secret_mode)
		chmod(path, 0600);
}

static void key_path(char *out, size_t cap, const char *name)
{
	snprintf(out, cap, "%s/%s.key", ks_dir(), name);
}

static void key_save(const char *name, const struct keyfile *k)
{
	char p[512];
	uint8_t buf[sizeof *k + 32];
	uint32_t n = 0;

	mkdir(ks_dir(), 0700);
	memcpy(buf, "PQK1", 4); n = 4;
	buf[n++] = k->kind; buf[n++] = k->param;
	memcpy(buf + n, &k->sym_len, 2); n += 2;
	memcpy(buf + n, &k->created, 4); n += 4;
	memcpy(buf + n, &k->pk_len, 4); n += 4;
	memcpy(buf + n, &k->blob_len, 4); n += 4;
	memcpy(buf + n, k->pk, k->pk_len); n += k->pk_len;
	memcpy(buf + n, k->blob, k->blob_len); n += k->blob_len;
	key_path(p, sizeof p, name);
	spill(p, buf, n, 1);
}

static void key_parse(const uint8_t *b, uint32_t n, struct keyfile *k, const char *who)
{
	uint32_t o = 20;

	if (n < 20 || memcmp(b, "PQK1", 4)) die(who, 0);
	k->kind = b[4]; k->param = b[5];
	memcpy(&k->sym_len, b + 6, 2);
	memcpy(&k->created, b + 8, 4);
	memcpy(&k->pk_len, b + 12, 4);
	memcpy(&k->blob_len, b + 16, 4);
	if (k->pk_len > sizeof k->pk || k->blob_len > sizeof k->blob ||
	    o + k->pk_len + k->blob_len != n)
		die(who, 0);
	memcpy(k->pk, b + o, k->pk_len); o += k->pk_len;
	memcpy(k->blob, b + o, k->blob_len);
}

static void key_load(const char *name, struct keyfile *k)
{
	char p[512];
	uint32_t n;
	uint8_t *b;

	key_path(p, sizeof p, name);
	b = slurp(p, &n, sizeof *k + 32);
	key_parse(b, n, k, "密钥文件格式不对");
	free(b);
}

static const char *alg_name(const struct keyfile *k)
{
	static const char *const mk[3] = { "ML-KEM-512", "ML-KEM-768", "ML-KEM-1024" };
	static const char *const md[3] = { "ML-DSA-44", "ML-DSA-65", "ML-DSA-87" };
	static const char *const sy[3] = { "AES-128", "AES-256", "SM4" };

	if (k->kind == KIND_MLKEM && k->param < 3) return mk[k->param];
	if (k->kind == KIND_MLDSA && k->param < 3) return md[k->param];
	if (k->kind == KIND_SYM && k->param < 3) return sy[k->param];
	return "?";
}

static void hexline(const char *label, const uint8_t *p, uint32_t n, uint32_t show)
{
	uint32_t i;

	printf("%s", label);
	for (i = 0; i < n && i < show; i++)
		printf("%02x", p[i]);
	printf("%s\n", n > show ? "…" : "");
}

/* 对称密钥：从 blob 装进槽 SYM_SLOT（TA 解包 → 安全通路 → FPGA 金库） */
static void sym_load(const struct keyfile *k)
{
	int rv = SDFE_LoadKey(ses(), SYM_SLOT, k->blob, k->blob_len);

	if (rv != SDR_OK) die("TA 拒绝装载这把密钥（blob 被改过，或不是本机生成的）", 0);
}

static uint32_t pqc_handle(const struct keyfile *k)
{
	uint32_t h = 0;
	int rv = SDFE_ImportKeyBlob(ses(), k->kind, k->param, k->blob, k->blob_len, &h);

	if (rv != SDR_OK) die("导入种子 blob 失败", rv);
	return h;
}

/* ---- 命令 ---- */
static int c_keygen(const char *alg, const char *name)
{
	struct keyfile k;
	char p[512];
	int rv;

	memset(&k, 0, sizeof k);
	key_path(p, sizeof p, name);
	if (access(p, F_OK) == 0) die("同名密钥已存在", 0);
	k.created = (uint32_t)time(NULL);
	if (!strcmp(alg, "sm4") || !strcmp(alg, "aes128") || !strcmp(alg, "aes256")) {
		k.kind = KIND_SYM;
		k.param = !strcmp(alg, "sm4") ? SDFE_ALG_SM4 :
		          !strcmp(alg, "aes128") ? SDFE_ALG_AES128 : SDFE_ALG_AES256;
		k.sym_len = k.param == SDFE_ALG_AES256 ? 32 : 16;
		k.blob_len = sizeof k.blob;
		rv = SDFE_GenerateKey(ses(), SYM_SLOT, k.sym_len, k.blob, &k.blob_len);
		if (rv != SDR_OK) die("生成对称密钥", rv);
	} else if (!strncmp(alg, "mlkem", 5) || !strncmp(alg, "mldsa", 5)) {
		int kem = alg[2] == 'k';
		const char *ps = alg + 5;
		uint32_t h, pset = !strcmp(ps, "512") || !strcmp(ps, "44") ? 0 :
		                   !strcmp(ps, "768") || !strcmp(ps, "65") ? 1 : 2;

		k.kind = kem ? KIND_MLKEM : KIND_MLDSA;
		k.param = (uint8_t)pset;
		k.pk_len = sizeof k.pk;
		rv = kem ? SDFE_GenerateKeyPair_MLKEM(ses(), pset, k.pk, &k.pk_len, &h)
		         : SDFE_GenerateKeyPair_MLDSA(ses(), pset, k.pk, &k.pk_len, &h);
		if (rv != SDR_OK) die("生成密钥对", rv);
		k.blob_len = sizeof k.blob;
		rv = SDFE_ExportKeyBlob(ses(), k.kind, h, k.blob, &k.blob_len);
		if (rv != SDR_OK) die("导出种子 blob", rv);
	} else {
		die("不认识的算法（sm4 aes128 aes256 mlkem512/768/1024 mldsa44/65/87）", 0);
	}
	key_save(name, &k);
	printf("已生成 %s 密钥「%s」→ %s\n", alg_name(&k), name, p);
	if (k.pk_len)
		printf("  公钥 %u 字节（可公开）\n", k.pk_len);
	printf("  %s：%u 字节 blob（TA 用 KEK 包裹的密文，本机以外解不开）\n",
	       k.kind == KIND_SYM ? "密钥" : "私钥（种子）", k.blob_len);
	return 0;
}

static int c_list(void)
{
	DIR *d = opendir(ks_dir());
	struct dirent *e;
	int n = 0;

	if (!d) { printf("（密钥库 %s 为空）\n", ks_dir()); return 0; }
	printf("名字           算法         公钥     blob     创建时间\n");
	while ((e = readdir(d))) {
		size_t l = strlen(e->d_name);
		char name[256];
		struct keyfile k;
		char ts[32];
		time_t t;

		if (l < 5 || strcmp(e->d_name + l - 4, ".key"))
			continue;
		snprintf(name, sizeof name, "%.*s", (int)(l - 4), e->d_name);
		key_load(name, &k);
		t = (time_t)k.created;
		strftime(ts, sizeof ts, "%m-%d %H:%M", localtime(&t));
		printf("%-14s %-12s %-8u %-8u %s\n", name, alg_name(&k), k.pk_len, k.blob_len, ts);
		n++;
	}
	closedir(d);
	printf("共 %d 把\n", n);
	return 0;
}

static int c_show(const char *name)
{
	struct keyfile k;
	uint8_t d[32];

	key_load(name, &k);
	printf("名字   %s\n算法   %s\n", name, alg_name(&k));
	if (k.pk_len) {
		hexline("公钥   ", k.pk, k.pk_len, 24);
		if (SDFE_Hash_SM3(ses(), k.pk, k.pk_len, d) == SDR_OK)
			hexline("指纹   SM3:", d, 16, 16);
	}
	hexline("blob   ", k.blob, k.blob_len, 32);
	printf("       ↑ \"PWRP\" 头 + AES-256-GCM 密文。KEK 在 TA 里，这个文件本身不含任何明文密钥。\n");
	return 0;
}

static int c_crypt(int enc, const char *name, const char *in, const char *out)
{
	struct keyfile k;
	uint8_t iv[16], *src, *dst;
	uint32_t n, len, off = 0, outlen;
	int rv;

	key_load(name, &k);
	if (k.kind != KIND_SYM) die("这不是对称密钥", 0);
	src = slurp(in, &n, 4u << 20);
	sym_load(&k);
	if (enc) {
		uint32_t pad = 16 - n % 16, i;

		if ((rv = SDFE_GenerateRandom(ses(), 16, iv)) != SDR_OK) die("取 IV", rv);
		for (i = 0; i < pad; i++)
			src[n + i] = (uint8_t)pad;          /* PKCS#7 */
		len = n + pad;
		dst = malloc(len + 16);
		memcpy(dst, iv, 16);
		while (off < len) {
			uint32_t c = len - off > CHUNK ? CHUNK : len - off;
			rv = SDFE_EncryptMode(ses(), k.param, SDFE_MODE_CBC, SYM_SLOT,
			                      off ? dst + 16 + off - 16 : iv,
			                      src + off, c, dst + 16 + off);
			if (rv != SDR_OK) die("加密", rv);
			off += c;
		}
		outlen = len + 16;
		printf("%s-CBC 加密：明文 %u 字节 → 密文 %u 字节（16 字节 IV + %u 字节）\n",
		       alg_name(&k), n, outlen, len);
	} else {
		if (n < 32 || (n - 16) % 16) die("密文长度不对", 0);
		len = n - 16;
		dst = malloc(len);
		while (off < len) {
			uint32_t c = len - off > CHUNK ? CHUNK : len - off;
			rv = SDFE_DecryptMode(ses(), k.param, SDFE_MODE_CBC, SYM_SLOT,
			                      off ? src + off : src,
			                      src + 16 + off, c, dst + off);
			if (rv != SDR_OK) die("解密", rv);
			off += c;
		}
		{
			uint8_t pad = dst[len - 1];
			uint32_t i;

			if (pad == 0 || pad > 16) die("解密失败（填充不对：密钥不对或密文被改）", 0);
			for (i = 0; i < pad; i++)
				if (dst[len - 1 - i] != pad)
					die("解密失败（填充不对：密钥不对或密文被改）", 0);
			outlen = len - pad;
		}
		printf("%s-CBC 解密：密文 %u 字节 → 明文 %u 字节\n", alg_name(&k), n, outlen);
	}
	spill(out, dst, outlen, 0);
	free(src); free(dst);
	return 0;
}

static int c_sign(const char *name, const char *file, const char *sigf)
{
	struct keyfile k;
	uint8_t sig[4627], *m;
	uint32_t n, sl = sizeof sig, h;
	int rv;

	key_load(name, &k);
	if (k.kind != KIND_MLDSA) die("这不是签名密钥", 0);
	m = slurp(file, &n, 8192);
	h = pqc_handle(&k);
	if ((rv = SDFE_Sign_MLDSA(ses(), h, m, n, NULL, 0, sig, &sl)) != SDR_OK) die("签名", rv);
	spill(sigf, sig, sl, 0);
	printf("%s 签名：%s（%u 字节）→ %s（%u 字节）\n", alg_name(&k), file, n, sigf, sl);
	free(m);
	return 0;
}

static int c_verify(const char *name, const char *file, const char *sigf)
{
	struct keyfile k;
	uint8_t *m, *sig;
	uint32_t n, sl;
	int rv;

	key_load(name, &k);
	if (k.kind != KIND_MLDSA) die("这不是签名密钥", 0);
	m = slurp(file, &n, 8192);
	sig = slurp(sigf, &sl, 8192);
	rv = SDFE_Verify_MLDSA(ses(), k.param, k.pk, k.pk_len, m, n, NULL, 0, sig, sl);
	if (rv == SDR_OK) {
		printf("✓ 验签通过：%s 确实由「%s」签署，且一个字节都没改过\n", file, name);
		return 0;
	}
	if (rv == SDR_VERIFYFAIL)
		printf("✗ 验签不通过：文件或签名被改过，或者不是这把密钥签的\n");
	else
		printf("✗ 验签出错：%s\n", SDFE_StrError(rv));
	return 2;
}

static int c_encap(const char *name, const char *ctf)
{
	struct keyfile k;
	uint8_t ss[32], ct[1568];
	uint32_t sl = 32, cl = sizeof ct;
	int rv;

	key_load(name, &k);
	if (k.kind != KIND_MLKEM) die("这不是 ML-KEM 密钥", 0);
	rv = SDFE_Encapsulate_MLKEM(ses(), k.param, k.pk, k.pk_len, ss, &sl, ct, &cl);
	if (rv != SDR_OK) die("封装", rv);
	spill(ctf, ct, cl, 0);
	printf("%s 封装（只用公钥）：封装密文 %u 字节 → %s\n", alg_name(&k), cl, ctf);
	hexline("发送方得到的共享密钥  ", ss, sl, 32);
	return 0;
}

static int c_decap(const char *name, const char *ctf)
{
	struct keyfile k;
	uint8_t ss[32], *ct;
	uint32_t sl = 32, cl, h;
	int rv;

	key_load(name, &k);
	if (k.kind != KIND_MLKEM) die("这不是 ML-KEM 密钥", 0);
	ct = slurp(ctf, &cl, 4096);
	h = pqc_handle(&k);
	if ((rv = SDFE_Decapsulate_MLKEM(ses(), h, ct, cl, ss, &sl)) != SDR_OK) die("解封装", rv);
	printf("%s 解封装（用私钥）：%s\n", alg_name(&k), ctf);
	hexline("接收方得到的共享密钥  ", ss, sl, 32);
	free(ct);
	return 0;
}

static int c_hash(const char *file)
{
	uint8_t *m, d[32];
	uint32_t n;
	int rv;

	m = slurp(file, &n, 16384);
	if ((rv = SDFE_Hash_SM3(ses(), m, n, d)) != SDR_OK) die("SM3", rv);
	hexline("SM3 ", d, 32, 32);
	free(m);
	return 0;
}

static int c_random(const char *ns)
{
	uint8_t b[256];
	uint32_t n = (uint32_t)atoi(ns);
	int rv;

	if (n == 0 || n > sizeof b) die("字节数 1..256", 0);
	if ((rv = SDFE_GenerateRandom(ses(), n, b)) != SDR_OK) die("取随机数", rv);
	hexline("", b, n, n);
	return 0;
}

/* 备份包：每把密钥一项 {名字长度, 名字, 数据长度, 数据, SM3(数据)}，
 * SM3 由密码机算。恢复时逐项重算比对，一个字节不对就整包拒绝。
 * 包里的密钥本来就是 TA 包裹的 blob —— 备份包丢了也泄不了密，
 * 但也只有这块板上的 TA 解得开（跨设备迁移需要 KEK 导出仪式，尚未实现）。 */
static int c_backup(const char *bundle)
{
	DIR *d = opendir(ks_dir());
	struct dirent *e;
	uint8_t *out = malloc(1u << 20);
	uint32_t o = 0, cnt = 0;

	if (!d) die("密钥库为空", 0);
	memcpy(out, "PQB1", 4); o = 8;
	while ((e = readdir(d))) {
		size_t l = strlen(e->d_name);
		char p[512];
		uint8_t *b, dg[32];
		uint32_t n, nl;
		int rv;

		if (l < 5 || strcmp(e->d_name + l - 4, ".key"))
			continue;
		snprintf(p, sizeof p, "%s/%s", ks_dir(), e->d_name);
		b = slurp(p, &n, 8192);
		if ((rv = SDFE_Hash_SM3(ses(), b, n, dg)) != SDR_OK) die("SM3", rv);
		nl = (uint32_t)l;
		memcpy(out + o, &nl, 4); o += 4;
		memcpy(out + o, e->d_name, nl); o += nl;
		memcpy(out + o, &n, 4); o += 4;
		memcpy(out + o, b, n); o += n;
		memcpy(out + o, dg, 32); o += 32;
		printf("  + %-18s %5u 字节  SM3 %02x%02x%02x%02x…\n", e->d_name, n,
		       dg[0], dg[1], dg[2], dg[3]);
		free(b);
		cnt++;
	}
	closedir(d);
	memcpy(out + 4, &cnt, 4);
	spill(bundle, out, o, 1);
	printf("备份包 %s：%u 把密钥，%u 字节（内容全是 TA 包裹的密文）\n", bundle, cnt, o);
	free(out);
	return 0;
}

static int c_restore(const char *bundle)
{
	uint8_t *b, dg[32];
	uint32_t n, o = 8, cnt, i;

	b = slurp(bundle, &n, 1u << 20);
	if (n < 8 || memcmp(b, "PQB1", 4)) die("不是备份包", 0);
	memcpy(&cnt, b + 4, 4);
	/* 先整包校验，全部通过才写盘：不允许"恢复了一半" */
	for (i = 0; i < cnt; i++) {
		uint32_t nl, dl;
		int rv;

		if (o + 4 > n) die("备份包被截断", 0);
		memcpy(&nl, b + o, 4); o += 4 + nl;
		if (o + 4 > n) die("备份包被截断", 0);
		memcpy(&dl, b + o, 4); o += 4;
		if (o + dl + 32 > n) die("备份包被截断", 0);
		if ((rv = SDFE_Hash_SM3(ses(), b + o, dl, dg)) != SDR_OK) die("SM3", rv);
		if (memcmp(dg, b + o + dl, 32)) {
			printf("✗ 第 %u 项 SM3 校验不符 —— 备份包被改过，整包拒绝恢复（一个文件都没写）\n", i + 1);
			return 2;
		}
		o += dl + 32;
	}
	mkdir(ks_dir(), 0700);
	for (o = 8, i = 0; i < cnt; i++) {
		uint32_t nl, dl;
		char name[256], p[512];

		memcpy(&nl, b + o, 4); o += 4;
		snprintf(name, sizeof name, "%.*s", (int)nl, (const char *)b + o); o += nl;
		memcpy(&dl, b + o, 4); o += 4;
		snprintf(p, sizeof p, "%s/%s", ks_dir(), name);
		spill(p, b + o, dl, 1);
		printf("  ✓ %-18s SM3 校验通过，已恢复\n", name);
		o += dl + 32;
	}
	printf("恢复完成：%u 把密钥\n", cnt);
	free(b);
	return 0;
}

static int c_destroy(const char *name)
{
	char p[512];
	uint8_t z[8192];
	uint32_t n;
	uint8_t *b;

	key_path(p, sizeof p, name);
	b = slurp(p, &n, sizeof z);
	memset(z, 0, sizeof z);
	spill(p, z, n, 1);                  /* 先覆写再删除 */
	unlink(p);
	free(b);
	printf("已销毁「%s」（覆写后删除）。这把密钥只以这份 blob 存在，现在它没有了。\n", name);
	return 0;
}

static int c_info(void)
{
	char info[256];

	SDFE_GetDeviceInfo(ses(), info, sizeof info);
	printf("%s\n密钥库目录 %s\n", info, ks_dir());
	return 0;
}

int main(int argc, char **argv)
{
	int rc = 1;
	const char *c = argc > 1 ? argv[1] : "";

#define NEED(k) if (argc < (k) + 2) die("参数不够，见 hsmctl 文件头的用法", 0)
	if (!strcmp(c, "info"))         rc = c_info();
	else if (!strcmp(c, "keygen"))  { NEED(2); rc = c_keygen(argv[2], argv[3]); }
	else if (!strcmp(c, "list"))    rc = c_list();
	else if (!strcmp(c, "show"))    { NEED(1); rc = c_show(argv[2]); }
	else if (!strcmp(c, "encrypt")) { NEED(3); rc = c_crypt(1, argv[2], argv[3], argv[4]); }
	else if (!strcmp(c, "decrypt")) { NEED(3); rc = c_crypt(0, argv[2], argv[3], argv[4]); }
	else if (!strcmp(c, "sign"))    { NEED(3); rc = c_sign(argv[2], argv[3], argv[4]); }
	else if (!strcmp(c, "verify"))  { NEED(3); rc = c_verify(argv[2], argv[3], argv[4]); }
	else if (!strcmp(c, "encap"))   { NEED(2); rc = c_encap(argv[2], argv[3]); }
	else if (!strcmp(c, "decap"))   { NEED(2); rc = c_decap(argv[2], argv[3]); }
	else if (!strcmp(c, "hash"))    { NEED(1); rc = c_hash(argv[2]); }
	else if (!strcmp(c, "random"))  { NEED(1); rc = c_random(argv[2]); }
	else if (!strcmp(c, "backup"))  { NEED(1); rc = c_backup(argv[2]); }
	else if (!strcmp(c, "restore")) { NEED(1); rc = c_restore(argv[2]); }
	else if (!strcmp(c, "destroy")) { NEED(1); rc = c_destroy(argv[2]); }
	else {
		fprintf(stderr, "用法：hsmctl info|keygen|list|show|encrypt|decrypt|sign|verify|"
		                "encap|decap|hash|random|backup|restore|destroy …\n");
		return 1;
	}
	if (g_ses) { SDFE_CloseSession(g_ses); SDFE_CloseDevice(g_dev); }
	return rc;
}
