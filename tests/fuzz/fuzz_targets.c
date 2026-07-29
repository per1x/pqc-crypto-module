/* fuzz 靶子：三个"从不可信来源读定长/变长字段"的解析入口
 *
 *   1. 密钥库文件   hsm_keystore_load
 *   2. 备份文件     hsm_backup_restore
 *   3. 审计日志     audit_verify_file / audit_read
 *   4. 命令协议帧   pqc_proto_dispatch      ← 唯一一个真正来自网络的
 *
 * 这四处是本项目最典型的内存安全面：都在按文件/报文里自称的长度去索引缓冲。
 *
 * 【双模式】
 * Apple 的 clang 不带 libFuzzer，所以本文件同时支持两种驱动：
 *   - 有 libFuzzer（brew install llvm 的 clang）：编成 LLVMFuzzerTestOneInput，
 *     覆盖引导 + 语料变异，效果最好；
 *   - 没有：编成独立驱动（-DFUZZ_STANDALONE），用确定性 PRNG 做
 *     "合法语料 + 随机变异"，跑在 ASan 下也能抓到越界与 UAF。
 * 独立驱动是接进 ctest 的那个 —— CI 里不能依赖 brew 装没装 llvm。
 */
#include "pqchsm/audit.h"
#include "pqchsm/backup.h"
#include "pqchsm/keystore.h"
#include "pqchsm/proto.h"
#include "pqchsm/slot.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_tmp[256];

static void write_tmp(const uint8_t *d, size_t n)
{
	FILE *f = fopen(g_tmp, "wb");
	if (f) {
		fwrite(d, 1, n, f);
		fclose(f);
	}
}

/* 一次投喂：按首字节选一个靶子，其余字节是载荷。
 * 这样单一入口就能覆盖四个解析器，语料也能互相变异过去。 */
static void feed(const uint8_t *data, size_t size)
{
	if (size < 2) {
		return;
	}
	uint8_t which = data[0] % 4;
	const uint8_t *body = data + 1;
	size_t n = size - 1;

	switch (which) {
	case 0: {   /* 密钥库 */
		write_tmp(body, n);
		hsm_token_t *tok = hsm_token_new(2);
		if (tok) {
			(void)hsm_keystore_load(tok, g_tmp);
			hsm_token_free(tok);
		}
		break;
	}
	case 1: {   /* 备份文件 —— 分片也从同一段字节里取，让两边一起被变异 */
		write_tmp(body, n);
		hsm_token_t *tok = hsm_token_new(2);
		if (tok) {
			uint8_t shares[3 * HSM_SHARE_CAP];
			size_t lens[3];
			memset(shares, 0, sizeof(shares));
			for (int i = 0; i < 3; i++) {
				size_t off = (size_t)i * HSM_SHARE_CAP;
				size_t take = n > HSM_SHARE_LEN ? HSM_SHARE_LEN : n;
				memcpy(shares + off, body, take);
				lens[i] = HSM_SHARE_LEN;
			}
			(void)hsm_backup_restore(tok, g_tmp, shares, HSM_SHARE_CAP, lens, 3, NULL);
			hsm_token_free(tok);
		}
		break;
	}
	case 2: {   /* 审计日志 */
		write_tmp(body, n);
		uint64_t bad = 0;
		(void)audit_verify_file(g_tmp, &bad);
		uint64_t seq = n ? body[0] : 0;
		char detail[AUDIT_DETAIL_LEN + 1];
		uint64_t ts;
		uint32_t op, role, slot, res;
		(void)audit_read(g_tmp, seq, &ts, &op, &role, &slot, &res, detail);
		uint8_t h[AUDIT_HASH_LEN];
		(void)audit_hash_at(g_tmp, seq, h);
		break;
	}
	case 3: {   /* 命令协议帧：唯一真正来自网络的入口 */
		hsm_token_t *tok = hsm_token_new(2);
		if (tok) {
			pqc_proto_ctx_t ctx = { .tok = tok, .keystore_path = NULL };
			static uint8_t resp[1 << 16];
			size_t rlen = 0;
			(void)pqc_proto_dispatch(&ctx, body, n, resp, sizeof(resp), &rlen);
			/* 分帧也要喂：daemon 就是靠它决定读多少 */
			(void)pqc_proto_frame_len(body, n);
			hsm_token_free(tok);
		}
		break;
	}
	default:
		break;
	}
}

#ifdef FUZZ_STANDALONE

/* xorshift64*：确定性、可复现，不依赖平台 rand() */
static uint64_t g_state = 0x2026072900000001ULL;

static uint64_t rnd(void)
{
	uint64_t x = g_state;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	g_state = x;
	return x * 0x2545F4914F6CDD1DULL;
}

/* 造一份合法语料：真的存一个密钥库、一份备份、一条审计日志出来，
 * 这样变异是"从合法结构出发"，比纯随机字节有效得多。 */
static size_t make_seed_corpus(uint8_t **out, size_t *lens, size_t max)
{
	size_t n = 0;
	char base[256];
	snprintf(base, sizeof(base), "/tmp/pqchsm_fuzzseed_%d", (int)getpid());

	/* 密钥库 */
	if (n < max) {
		char p[300];
		snprintf(p, sizeof(p), "%s.ks", base);
		hsm_token_t *tok = hsm_token_new(2);
		if (tok && hsm_slot_init_token(tok, 0, "fuzz", "so-secret-01") == HSM_OK &&
		    hsm_keystore_save(tok, p) == HSM_OK) {
			FILE *f = fopen(p, "rb");
			if (f) {
				fseek(f, 0, SEEK_END);
				long sz = ftell(f);
				rewind(f);
				uint8_t *b = malloc((size_t)sz + 1);
				b[0] = 0;   /* 选靶子 0 */
				if (fread(b + 1, 1, (size_t)sz, f) == (size_t)sz) {
					out[n] = b;
					lens[n] = (size_t)sz + 1;
					n++;
				} else {
					free(b);
				}
				fclose(f);
			}
		}
		hsm_token_free(tok);
		unlink(p);
	}

	/* 审计日志 */
	if (n < max) {
		char p[300];
		snprintf(p, sizeof(p), "%s.log", base);
		unlink(p);
		audit_log_t *log = audit_open(p);
		if (log) {
			for (int i = 0; i < 5; i++) {
				audit_append(log, 1700000000ull + (uint64_t)i, AUDIT_OP_SIGN,
				             1, (uint32_t)i, 0, "seed");
			}
			audit_close(log);
			FILE *f = fopen(p, "rb");
			if (f) {
				fseek(f, 0, SEEK_END);
				long sz = ftell(f);
				rewind(f);
				uint8_t *b = malloc((size_t)sz + 1);
				b[0] = 2;   /* 选靶子 2 */
				if (fread(b + 1, 1, (size_t)sz, f) == (size_t)sz) {
					out[n] = b;
					lens[n] = (size_t)sz + 1;
					n++;
				} else {
					free(b);
				}
				fclose(f);
			}
			unlink(p);
		}
	}

	/* 协议帧 */
	if (n < max) {
		uint8_t pl[64];
		tlv_writer_t w;
		tlv_init(&w, pl, sizeof(pl));
		tlv_put_u32(&w, TAG_SLOT, 0);
		uint8_t *b = malloc(256);
		size_t fl = 0;
		b[0] = 3;
		if (pqc_proto_build_req(CMD_SLOT_INFO, 1, pl, w.len, b + 1, 255, &fl) == 0) {
			out[n] = b;
			lens[n] = fl + 1;
			n++;
		} else {
			free(b);
		}
	}
	return n;
}

int main(int argc, char **argv)
{
	long iters = argc > 1 ? strtol(argv[1], NULL, 10) : 4000;
	snprintf(g_tmp, sizeof(g_tmp), "/tmp/pqchsm_fuzz_%d.bin", (int)getpid());

	uint8_t *corpus[8];
	size_t clen[8];
	size_t ncorp = make_seed_corpus(corpus, clen, 8);
	printf("fuzz（独立驱动）：%zu 份合法语料，%ld 轮\n", ncorp, iters);

	size_t maxlen = 0;
	for (size_t i = 0; i < ncorp; i++) {
		if (clen[i] > maxlen) {
			maxlen = clen[i];
		}
	}
	if (maxlen < 64) {
		maxlen = 64;
	}
	uint8_t *buf = malloc(maxlen + 256);

	for (long it = 0; it < iters; it++) {
		size_t n;
		if (ncorp && (rnd() % 4)) {
			/* 从合法语料变异 */
			size_t k = rnd() % ncorp;
			n = clen[k];
			memcpy(buf, corpus[k], n);
			int nmut = 1 + (int)(rnd() % 8);
			for (int m = 0; m < nmut; m++) {
				size_t pos = rnd() % n;
				switch (rnd() % 3) {
				case 0: buf[pos] ^= (uint8_t)(1u << (rnd() % 8)); break;
				case 1: buf[pos] = (uint8_t)rnd(); break;
				default: buf[pos] = (uint8_t)((rnd() % 2) ? 0x00 : 0xff); break;
				}
			}
			/* 偶尔截断，专打"声明长度 > 实际长度"的路径 */
			if ((rnd() % 5) == 0 && n > 4) {
				n -= 1 + (rnd() % (n / 2));
			}
		} else {
			/* 纯随机 */
			n = 2 + (rnd() % 200);
			for (size_t i = 0; i < n; i++) {
				buf[i] = (uint8_t)rnd();
			}
		}
		feed(buf, n);
	}

	for (size_t i = 0; i < ncorp; i++) {
		free(corpus[i]);
	}
	free(buf);
	unlink(g_tmp);
	printf("fuzz 完成：%ld 轮，无崩溃\n", iters);
	return 0;
}

#else   /* libFuzzer */

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	(void)argc;
	(void)argv;
	snprintf(g_tmp, sizeof(g_tmp), "/tmp/pqchsm_fuzz_%d.bin", (int)getpid());
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	feed(data, size);
	return 0;
}

#endif
