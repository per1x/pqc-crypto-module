/* kat_runner —— 用 ACVP 最终版黄金向量逐条驱动 crypto 后端
 *
 * 用法：kat_runner <file.kat> [file.kat ...]
 *
 * ：整机 KAT 一律用最终版 FIPS 203/204 向量。
 * 后端不支持的组由 tools/acvp_to_kat.py 在文件头以 `# SKIPPED:` 显式记录，
 * 这里把它们原样打印出来 —— 不能让"少测了一半"看起来像"全绿"。
 */
#include "pqchsm/kat.h"
#include "pqchsm/pqc.h"
#include "pqchsm/util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail, g_skip;

static void fail(const kat_record_t *rec, const char *fmt, ...)
{
	const char *alg  = kat_str(rec, "alg");
	const char *tcid = kat_str(rec, "tcid");
	fprintf(stderr, "  FAIL %s tcId=%s: ", alg ? alg : "?", tcid ? tcid : "?");
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	g_fail++;
}

/* 取出字段并 hex 解码到新分配的缓冲。字段不存在返回 NULL。 */
static uint8_t *dup_field(const kat_record_t *rec, const char *name, size_t *len_out)
{
	long n = kat_len(rec, name);
	if (n < 0) {
		return NULL;
	}
	/* 空字段（如 context = ）也要返回非 NULL，用 1 字节占位 */
	uint8_t *buf = malloc((size_t)n ? (size_t)n : 1);
	if (!buf) {
		return NULL;
	}
	if (kat_bytes(rec, name, buf, (size_t)n ? (size_t)n : 1) != n) {
		free(buf);
		return NULL;
	}
	*len_out = (size_t)n;
	return buf;
}

/* 取字段到已在外层声明并初始化为 NULL 的 var，同时引入 var##_len。
 * 失败即 goto done —— 外层变量已置 NULL，清理路径永远安全。 */
#define NEED(var, field)                                            \
	size_t var##_len = 0;                                       \
	var = dup_field(rec, field, &var##_len);                    \
	if (!var) { fail(rec, "缺少字段 %s", field); goto done; }

static void run_kem_keygen(const kat_record_t *rec, pqc_alg_t alg,
                           const pqc_alg_info_t *info)
{
	uint8_t *d = NULL, *z = NULL, *ek = NULL, *dk = NULL, *pk = NULL, *sk = NULL;
	uint8_t seed[64];
	{
		NEED(d, "d") NEED(z, "z") NEED(ek, "ek") NEED(dk, "dk")
		if (d_len != 32 || z_len != 32) {
			fail(rec, "d/z 长度应为 32，实际 %zu/%zu", d_len, z_len);
			goto done;
		}
		/* FIPS 203 KeyGen(d, z)；liboqs keypair_derand 的 coins = d‖z */
		memcpy(seed, d, 32);
		memcpy(seed + 32, z, 32);

		pk = malloc(info->pk_len);
		sk = malloc(info->sk_len);
		if (!pk || !sk) {
			fail(rec, "内存不足");
			goto done;
		}
		pqc_status_t st = pqc_keypair_from_seed(alg, seed, 64, pk, sk);
		if (st != PQC_OK) {
			fail(rec, "keypair_from_seed: %s", pqc_strerror(st));
			goto done;
		}
		if (ek_len != info->pk_len || dk_len != info->sk_len) {
			fail(rec, "向量长度与元数据不符 ek %zu/%zu dk %zu/%zu",
			     ek_len, info->pk_len, dk_len, info->sk_len);
			goto done;
		}
		if (memcmp(pk, ek, ek_len) != 0) {
			fail(rec, "ek 不匹配");
			goto done;
		}
		if (memcmp(sk, dk, dk_len) != 0) {
			fail(rec, "dk 不匹配");
			goto done;
		}
		g_pass++;
	}
done:
	pqc_secure_zero(seed, sizeof(seed));
	free(d); free(z); free(ek); free(dk); free(pk); free(sk);
}

static void run_sig_keygen(const kat_record_t *rec, pqc_alg_t alg,
                           const pqc_alg_info_t *info)
{
	uint8_t *seed = NULL, *pk_exp = NULL, *sk_exp = NULL, *pk = NULL, *sk = NULL;
	{
		NEED(seed, "seed") NEED(pk_exp, "pk") NEED(sk_exp, "sk")
		if (seed_len != 32) {
			fail(rec, "seed 长度应为 32，实际 %zu", seed_len);
			goto done;
		}
		pk = malloc(info->pk_len);
		sk = malloc(info->sk_len);
		if (!pk || !sk) {
			fail(rec, "内存不足");
			goto done;
		}
		pqc_status_t st = pqc_keypair_from_seed(alg, seed, 32, pk, sk);
		if (st == PQC_ERR_UNSUPPORTED) {
			g_skip++;
			goto done;
		}
		if (st != PQC_OK) {
			fail(rec, "keypair_from_seed: %s", pqc_strerror(st));
			goto done;
		}
		if (memcmp(pk, pk_exp, info->pk_len) != 0) {
			fail(rec, "pk 不匹配");
			goto done;
		}
		if (memcmp(sk, sk_exp, info->sk_len) != 0) {
			fail(rec, "sk 不匹配");
			goto done;
		}
		g_pass++;
	}
done:
	free(seed); free(pk_exp); free(sk_exp); free(pk); free(sk);
}

static void run_encaps(const kat_record_t *rec, pqc_alg_t alg,
                       const pqc_alg_info_t *info)
{
	uint8_t *ek = NULL, *m = NULL, *c_exp = NULL, *k_exp = NULL, *ct = NULL, *ss = NULL;
	{
		NEED(ek, "ek") NEED(m, "m") NEED(c_exp, "c") NEED(k_exp, "k")
		ct = malloc(info->ct_len);
		ss = malloc(info->ss_len);
		if (!ct || !ss) {
			fail(rec, "内存不足");
			goto done;
		}
		pqc_status_t st = pqc_encaps_derand(alg, ek, m, m_len, ct, ss);
		if (st != PQC_OK) {
			fail(rec, "encaps_derand: %s", pqc_strerror(st));
			goto done;
		}
		if (c_exp_len != info->ct_len || memcmp(ct, c_exp, c_exp_len) != 0) {
			fail(rec, "密文 c 不匹配");
			goto done;
		}
		if (k_exp_len != info->ss_len || memcmp(ss, k_exp, k_exp_len) != 0) {
			fail(rec, "共享秘密 k 不匹配");
			goto done;
		}
		g_pass++;
	}
done:
	if (ss) pqc_secure_zero(ss, info->ss_len);
	free(ek); free(m); free(c_exp); free(k_exp); free(ct); free(ss);
}

static void run_decaps(const kat_record_t *rec, pqc_alg_t alg,
                       const pqc_alg_info_t *info)
{
	uint8_t *dk = NULL, *c = NULL, *k_exp = NULL, *ss = NULL;
	{
		NEED(dk, "dk") NEED(c, "c") NEED(k_exp, "k")
		ss = malloc(info->ss_len);
		if (!ss) {
			fail(rec, "内存不足");
			goto done;
		}
		pqc_status_t st = pqc_decaps(alg, dk, c, ss);
		if (st != PQC_OK) {
			fail(rec, "decaps: %s", pqc_strerror(st));
			goto done;
		}
		/* 注意：ML-KEM 解封装对坏密文走隐式拒绝，仍返回成功但 k 不同，
		 * 所以这里逐字节比对是唯一的判据。 */
		if (k_exp_len != info->ss_len || memcmp(ss, k_exp, k_exp_len) != 0) {
			fail(rec, "共享秘密 k 不匹配");
			goto done;
		}
		g_pass++;
	}
done:
	if (ss) pqc_secure_zero(ss, info->ss_len);
	free(dk); free(c); free(k_exp); free(ss);
}

static void run_siggen(const kat_record_t *rec, pqc_alg_t alg,
                       const pqc_alg_info_t *info)
{
	uint8_t *sk = NULL, *msg = NULL, *ctx = NULL, *rnd = NULL, *sig_exp = NULL, *sig = NULL;
	{
		NEED(sk, "sk") NEED(msg, "msg") NEED(ctx, "context")
		NEED(rnd, "rnd") NEED(sig_exp, "sig")
		sig = malloc(info->sig_len);
		if (!sig) {
			fail(rec, "内存不足");
			goto done;
		}
		size_t sig_len = info->sig_len;
		pqc_status_t st = pqc_sign(alg, sk, msg, msg_len,
		                           ctx_len ? ctx : NULL, ctx_len,
		                           rnd, sig, &sig_len);
		if (st == PQC_ERR_UNSUPPORTED) {
			g_skip++;
			goto done;
		}
		if (st != PQC_OK) {
			fail(rec, "sign: %s", pqc_strerror(st));
			goto done;
		}
		if (sig_len != sig_exp_len || memcmp(sig, sig_exp, sig_len) != 0) {
			fail(rec, "签名不匹配 (len %zu vs %zu)", sig_len, sig_exp_len);
			goto done;
		}
		g_pass++;
	}
done:
	free(sk); free(msg); free(ctx); free(rnd); free(sig_exp); free(sig);
}

static void run_sigver(const kat_record_t *rec, pqc_alg_t alg,
                       const pqc_alg_info_t *info)
{
	(void)info;
	uint8_t *pk = NULL, *msg = NULL, *ctx = NULL, *sig = NULL;
	{
		NEED(pk, "pk") NEED(msg, "msg") NEED(ctx, "context") NEED(sig, "sig")
		const char *want = kat_str(rec, "result");
		if (!want) {
			fail(rec, "缺少 result 字段");
			goto done;
		}
		int want_pass = strcmp(want, "pass") == 0;

		pqc_status_t st = pqc_verify(alg, pk, msg, msg_len,
		                             ctx_len ? ctx : NULL, ctx_len, sig, sig_len);
		if (st != PQC_OK && st != PQC_ERR_VERIFY) {
			fail(rec, "verify 异常: %s", pqc_strerror(st));
			goto done;
		}
		int got_pass = (st == PQC_OK);
		if (got_pass != want_pass) {
			fail(rec, "期望 %s，实得 %s", want, got_pass ? "pass" : "fail");
			goto done;
		}
		g_pass++;
	}
done:
	free(pk); free(msg); free(ctx); free(sig);
}

static void print_skips(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		if (line[0] != '#') {
			break;      /* 注释头结束 */
		}
		if (strstr(line, "SKIPPED:")) {
			fputs("  ", stdout);
			fputs(line, stdout);
		}
	}
	fclose(f);
}

static int run_file(const char *path)
{
	kat_reader_t *r = kat_open(path);
	if (!r) {
		fprintf(stderr, "打不开 %s（先跑 tools/acvp_to_kat.py）\n", path);
		return 1;
	}
	const char *base = strrchr(path, '/');
	printf("== %s\n", base ? base + 1 : path);
	print_skips(path);

	int p0 = g_pass, f0 = g_fail, s0 = g_skip;
	kat_record_t rec;
	int rc;
	while ((rc = kat_next(r, &rec)) == 1) {
		const char *alg_name = kat_str(&rec, "alg");
		const char *op       = kat_str(&rec, "op");
		if (!alg_name || !op) {
			fprintf(stderr, "  记录缺少 alg/op\n");
			g_fail++;
			continue;
		}
		pqc_alg_t alg = pqc_alg_by_name(alg_name);
		const pqc_alg_info_t *info = pqc_alg_info(alg);
		if (!info) {
			fprintf(stderr, "  未知算法 %s\n", alg_name);
			g_fail++;
			continue;
		}
		if (strcmp(op, "keygen") == 0) {
			if (info->kind == PQC_KIND_KEM) {
				run_kem_keygen(&rec, alg, info);
			} else {
				run_sig_keygen(&rec, alg, info);
			}
		} else if (strcmp(op, "encaps") == 0) {
			run_encaps(&rec, alg, info);
		} else if (strcmp(op, "decaps") == 0) {
			run_decaps(&rec, alg, info);
		} else if (strcmp(op, "siggen") == 0) {
			run_siggen(&rec, alg, info);
		} else if (strcmp(op, "sigver") == 0) {
			run_sigver(&rec, alg, info);
		} else if (strcmp(op, "keycheck") == 0) {
			/* liboqs 不暴露独立的公私钥格式校验 API（FIPS 203 /7.3），
			 * 这类向量留到 由硬件核的输入校验路径覆盖。 */
			g_skip++;
		} else {
			fprintf(stderr, "  未知 op %s\n", op);
			g_fail++;
		}
	}
	kat_close(r);
	if (rc < 0) {
		fprintf(stderr, "  解析错误\n");
		g_fail++;
	}
	printf("   pass=%d fail=%d skip=%d\n", g_pass - p0, g_fail - f0, g_skip - s0);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "用法: %s <file.kat> [...]\n", argv[0]);
		return 2;
	}
	printf("后端: %s\n", pqc_get_backend()->name);
	for (int i = 1; i < argc; i++) {
		if (run_file(argv[i]) != 0) {
			return 2;
		}
	}
	printf("总计: pass=%d fail=%d skip=%d\n", g_pass, g_fail, g_skip);
	return g_fail ? 1 : 0;
}
