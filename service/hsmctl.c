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
 * 业务语义全在 hsmops.c（与 HTTPS 网关 hsm_httpd 共用），这里只是终端外壳。
 * 完整手册：仓库根 README.zh-CN.md 的「命令行：hsmctl 使用手册」。
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "hsmops.h"

static void die(void)
{
	fprintf(stderr, "✗ %s\n", hsm_err());
	hsm_close();
	exit(1);
}

static void die_msg(const char *m)
{
	fprintf(stderr, "✗ %s\n", m);
	hsm_close();
	exit(1);
}

static uint8_t *slurp(const char *path, uint32_t *len, uint32_t max)
{
	FILE *f = fopen(path, "rb");
	uint8_t *b;
	long n;

	if (!f) { fprintf(stderr, "✗ 打不开 %s：%s\n", path, strerror(errno)); exit(1); }
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	if (n < 0 || (uint32_t)n > max) die_msg("文件太大");
	b = malloc((size_t)n + 1);
	if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) die_msg("读文件失败");
	fclose(f);
	*len = (uint32_t)n;
	return b;
}

static void spill(const char *path, const uint8_t *b, uint32_t len, int secret)
{
	FILE *f = fopen(path, "wb");

	if (!f) { fprintf(stderr, "✗ 写不了 %s：%s\n", path, strerror(errno)); exit(1); }
	if (fwrite(b, 1, len, f) != len) die_msg("写文件失败");
	fclose(f);
	if (secret)
		chmod(path, 0600);
}

static void hexline(const char *label, const uint8_t *p, uint32_t n, uint32_t show)
{
	uint32_t i;

	printf("%s", label);
	for (i = 0; i < n && i < show; i++)
		printf("%02x", p[i]);
	printf("%s\n", n > show ? "…" : "");
}

/* ---- 命令 ---- */
static int c_keygen(const char *alg, const char *name)
{
	hsm_key k;

	if (hsm_key_gen(alg, name, &k))
		die();
	printf("已生成 %s 密钥「%s」→ %s/%s.key\n", hsm_alg_name(&k), name, hsm_keystore(), name);
	if (k.pk_len)
		printf("  公钥 %u 字节（可公开）\n", k.pk_len);
	printf("  %s：%u 字节 blob（TA 用 KEK 包裹的密文，本机以外解不开）\n",
	       k.kind == HSM_KIND_SYM ? "密钥" : "私钥（种子）", k.blob_len);
	return 0;
}

static void list_cb(const char *name, const hsm_key *k, void *u)
{
	char ts[32];
	time_t t = (time_t)k->created;

	(void)u;
	strftime(ts, sizeof ts, "%m-%d %H:%M", localtime(&t));
	printf("%-14s %-12s %-8u %-8u %s\n", name, hsm_alg_name(k), k->pk_len, k->blob_len, ts);
}

static int c_list(void)
{
	int n;

	printf("名字           算法         公钥     blob     创建时间\n");
	n = hsm_key_list(list_cb, NULL);
	printf("共 %d 把\n", n < 0 ? 0 : n);
	return 0;
}

static int c_show(const char *name)
{
	hsm_key k;
	uint8_t d[32];

	if (hsm_key_load(name, &k))
		die();
	printf("名字   %s\n算法   %s\n", name, hsm_alg_name(&k));
	if (k.pk_len) {
		hexline("公钥   ", k.pk, k.pk_len, 24);
		if (hsm_sm3(k.pk, k.pk_len, d) == 0)
			hexline("指纹   SM3:", d, 16, 16);
	}
	hexline("blob   ", k.blob, k.blob_len, 32);
	printf("       ↑ \"PWRP\" 头 + AES-256-GCM 密文。KEK 在 TA 里，这个文件本身不含任何明文密钥。\n");
	return 0;
}

static int c_crypt(int enc, const char *name, const char *in, const char *out)
{
	hsm_key k;
	uint8_t *src, *dst;
	uint32_t n, outlen;

	if (hsm_key_load(name, &k))
		die();
	src = slurp(in, &n, 4u << 20);
	if ((enc ? hsm_encrypt : hsm_decrypt)(name, src, n, &dst, &outlen))
		die();
	spill(out, dst, outlen, 0);
	if (enc)
		printf("%s-CBC 加密：明文 %u 字节 → 密文 %u 字节（16 字节 IV + %u 字节）\n",
		       hsm_alg_name(&k), n, outlen, outlen - 16);
	else
		printf("%s-CBC 解密：密文 %u 字节 → 明文 %u 字节\n", hsm_alg_name(&k), n, outlen);
	free(src); free(dst);
	return 0;
}

static int c_sign(const char *name, const char *file, const char *sigf)
{
	hsm_key k;
	uint8_t sig[4627], *m;
	uint32_t n, sl = sizeof sig;

	if (hsm_key_load(name, &k))
		die();
	m = slurp(file, &n, 8192);
	if (hsm_sign(name, m, n, sig, &sl))
		die();
	spill(sigf, sig, sl, 0);
	printf("%s 签名：%s（%u 字节）→ %s（%u 字节）\n", hsm_alg_name(&k), file, n, sigf, sl);
	free(m);
	return 0;
}

static int c_verify(const char *name, const char *file, const char *sigf)
{
	uint8_t *m, *sig;
	uint32_t n, sl;
	int rv;

	m = slurp(file, &n, 8192);
	sig = slurp(sigf, &sl, 8192);
	rv = hsm_verify(name, m, n, sig, sl);
	if (rv < 0)
		die();
	if (rv == 0) {
		printf("✓ 验签通过：%s 确实由「%s」签署，且一个字节都没改过\n", file, name);
		return 0;
	}
	printf("✗ 验签不通过：文件或签名被改过，或者不是这把密钥签的\n");
	return 2;
}

static int c_encap(const char *name, const char *ctf)
{
	hsm_key k;
	uint8_t ss[32], ct[1568];
	uint32_t cl = sizeof ct;

	if (hsm_key_load(name, &k) || hsm_encap(name, ct, &cl, ss))
		die();
	spill(ctf, ct, cl, 0);
	printf("%s 封装（只用公钥）：封装密文 %u 字节 → %s\n", hsm_alg_name(&k), cl, ctf);
	hexline("发送方得到的共享密钥  ", ss, 32, 32);
	return 0;
}

static int c_decap(const char *name, const char *ctf)
{
	hsm_key k;
	uint8_t ss[32], *ct;
	uint32_t cl;

	if (hsm_key_load(name, &k))
		die();
	ct = slurp(ctf, &cl, 4096);
	if (hsm_decap(name, ct, cl, ss))
		die();
	printf("%s 解封装（用私钥）：%s\n", hsm_alg_name(&k), ctf);
	hexline("接收方得到的共享密钥  ", ss, 32, 32);
	free(ct);
	return 0;
}

static int c_hash(const char *file)
{
	uint8_t *m, d[32];
	uint32_t n;

	m = slurp(file, &n, 16384);
	if (hsm_sm3(m, n, d))
		die();
	hexline("SM3 ", d, 32, 32);
	free(m);
	return 0;
}

static int c_random(const char *ns)
{
	uint8_t b[256];
	uint32_t n = (uint32_t)atoi(ns);

	if (n == 0 || n > sizeof b)
		die_msg("字节数 1..256");
	if (hsm_random(n, b))
		die();
	hexline("", b, n, n);
	return 0;
}

/* 备份包：每把密钥一项 {名字长度, 名字, 数据长度, 数据, SM3(数据)}，SM3 由密码机算。
 * 恢复时逐项重算比对，一个字节不对就整包拒绝。包里本来就是 TA 包裹的 blob ——
 * 丢了也泄不了密，但也只有这块板上的 TA 解得开（跨设备迁移尚未实现）。 */
struct bk { uint8_t *out; uint32_t o, cnt; };

static void backup_cb(const char *name, const hsm_key *k, void *u)
{
	struct bk *b = u;
	char p[512];
	uint8_t *d, dg[32];
	uint32_t n, nl;

	(void)k;
	snprintf(p, sizeof p, "%s/%s.key", hsm_keystore(), name);
	d = slurp(p, &n, 8192);
	if (hsm_sm3(d, n, dg))
		die();
	nl = (uint32_t)strlen(name) + 4;
	memcpy(b->out + b->o, &nl, 4); b->o += 4;
	memcpy(b->out + b->o, name, nl - 4); b->o += nl - 4;
	memcpy(b->out + b->o, ".key", 4); b->o += 4;
	memcpy(b->out + b->o, &n, 4); b->o += 4;
	memcpy(b->out + b->o, d, n); b->o += n;
	memcpy(b->out + b->o, dg, 32); b->o += 32;
	printf("  + %-18s %5u 字节  SM3 %02x%02x%02x%02x…\n", p + strlen(hsm_keystore()) + 1, n,
	       dg[0], dg[1], dg[2], dg[3]);
	free(d);
	b->cnt++;
}

static int c_backup(const char *bundle)
{
	struct bk b = { malloc(1u << 20), 8, 0 };

	if (!b.out)
		die_msg("内存不足");
	memcpy(b.out, "PQB1", 4);
	hsm_key_list(backup_cb, &b);
	if (b.cnt == 0)
		die_msg("密钥库为空");
	memcpy(b.out + 4, &b.cnt, 4);
	spill(bundle, b.out, b.o, 1);
	printf("备份包 %s：%u 把密钥，%u 字节（内容全是 TA 包裹的密文）\n", bundle, b.cnt, b.o);
	free(b.out);
	return 0;
}

static int c_restore(const char *bundle)
{
	uint8_t *b, dg[32];
	uint32_t n, o = 8, cnt, i;

	b = slurp(bundle, &n, 1u << 20);
	if (n < 8 || memcmp(b, "PQB1", 4))
		die_msg("不是备份包");
	memcpy(&cnt, b + 4, 4);
	/* 先整包校验，全部通过才写盘：不允许"恢复了一半" */
	for (i = 0; i < cnt; i++) {
		uint32_t nl, dl;

		if (o + 4 > n) die_msg("备份包被截断");
		memcpy(&nl, b + o, 4); o += 4 + nl;
		if (o + 4 > n) die_msg("备份包被截断");
		memcpy(&dl, b + o, 4); o += 4;
		if (o + dl + 32 > n) die_msg("备份包被截断");
		if (hsm_sm3(b + o, dl, dg))
			die();
		if (memcmp(dg, b + o + dl, 32)) {
			printf("✗ 第 %u 项 SM3 校验不符 —— 备份包被改过，整包拒绝恢复（一个文件都没写）\n", i + 1);
			return 2;
		}
		o += dl + 32;
	}
	mkdir(hsm_keystore(), 0700);
	for (o = 8, i = 0; i < cnt; i++) {
		uint32_t nl, dl;
		char name[256], p[512];

		memcpy(&nl, b + o, 4); o += 4;
		snprintf(name, sizeof name, "%.*s", (int)nl, (const char *)b + o); o += nl;
		memcpy(&dl, b + o, 4); o += 4;
		if (nl < 5 || strcmp(name + nl - 4, ".key")) die_msg("备份包里有不合法的文件名");
		name[nl - 4] = 0;
		if (!hsm_name_ok(name)) die_msg("备份包里有不合法的文件名");
		snprintf(p, sizeof p, "%s/%s.key", hsm_keystore(), name);
		spill(p, b + o, dl, 1);
		printf("  ✓ %-18s SM3 校验通过，已恢复\n", p + strlen(hsm_keystore()) + 1);
		o += dl + 32;
	}
	printf("恢复完成：%u 把密钥\n", cnt);
	free(b);
	return 0;
}

static int c_destroy(const char *name)
{
	if (hsm_key_destroy(name))
		die();
	printf("已销毁「%s」（覆写后删除）。这把密钥只以这份 blob 存在，现在它没有了。\n", name);
	return 0;
}

static int c_info(void)
{
	char info[256];

	if (hsm_info(info, sizeof info))
		die();
	printf("%s\n密钥库目录 %s\n", info, hsm_keystore());
	return 0;
}

int main(int argc, char **argv)
{
	int rc = 1;
	const char *c = argc > 1 ? argv[1] : "";

#define NEED(k) if (argc < (k) + 2) die_msg("参数不够，见 hsmctl 文件头的用法")
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
	hsm_close();
	return rc;
}
