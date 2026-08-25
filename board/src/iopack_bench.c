/* iopack_bench —— D11：4 字节打包口的上板 A/B
 *
 * ============================================================================
 * 【为什么这个测量必须是同一进程里的 A/B】
 * ============================================================================
 * 要回答的问题是"打包省下多少"，而不是"这块板今天有多快"。跨两次运行、跨两份
 * 位流去比，会把时钟、缓存、位流布线、板子温度全都混进差值里。所以这里**同一
 * 个进程、同一份位流、同一批输入**，逐字节跑一遍、打包跑一遍，紧挨着。
 *
 * 而且每一对都**逐字节比对两条路的输出**。这一条比时间数字更要紧：打包最容易
 * 出的错是字节序或错位，它不会报错，只会让 ek/密文变成另一个完全合法的值。
 * 一个"打包更快但结果不对"的实现在纯计时的测量里是看不出来的。
 *
 * 两种通路各测一遍，因为它们的瓶颈不是同一个东西：
 *   · **`/dev/mem` 直连**（演示形态）—— 每字节一笔 AXI。这是 docs/TESTING 里
 *     那张表用的通路，所以拿它跟历史数字对得上。
 *   · **`/dev/secmmio` → EL3**（交付通路）—— 每字节一次 ioctl + 一次 SMC。
 *     打包在这条路上省的是**系统调用与世界切换**，收益应当更大。
 *
 * 用法：iopack_bench [次数]        默认 20 次，与 docs/TESTING 那张表一致
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "kmod/secmmio_uapi.h"
#include "kat_mlkem_all.h"

#define PL_BASE   0x80000000UL
#define PL_SPAN   0x00050000UL
#define S_MLKEM   0x30000

#define MK_VER     (S_MLKEM + 0x00)
#define MK_CTRL    (S_MLKEM + 0x04)
#define MK_STATUS  (S_MLKEM + 0x08)
#define MK_MODE    (S_MLKEM + 0x0C)
#define MK_INDATA  (S_MLKEM + 0x10)
#define MK_INPTR   (S_MLKEM + 0x14)
#define MK_OUTDAT  (S_MLKEM + 0x18)
#define MK_OUTLEN  (S_MLKEM + 0x1C)
#define MK_INDATA4 (S_MLKEM + 0x40)
#define MK_OUTDAT4 (S_MLKEM + 0x44)

#define MKC_START  1u
#define MKC_INRST  4u
#define MKS_DONE   2u

static volatile uint8_t *mm;      /* /dev/mem 映射 */
static int sfd = -1;              /* /dev/secmmio */
static int via_el3;               /* 当前这一轮走哪条通路 */

static uint32_t rd(unsigned off)
{
	if (via_el3) {
		struct secmmio_op op = { .addr = (uint32_t)(PL_BASE + off), .val = 0 };
		if (ioctl(sfd, SECMMIO_RD, &op) < 0) return 0xFFFFFFFFu;
		return op.val;
	}
	return *(volatile uint32_t *)(mm + off);
}

static void wr(unsigned off, uint32_t v)
{
	if (via_el3) {
		struct secmmio_op op = { .addr = (uint32_t)(PL_BASE + off), .val = v };
		(void)ioctl(sfd, SECMMIO_WR, &op);
		return;
	}
	*(volatile uint32_t *)(mm + off) = v;
}

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

/* packed = 0 逐字节；packed = 1 打包（尾巴退回逐字节） */
static int mlkem_run(int packed, int mode, int pset,
		     const unsigned char *in, size_t inlen,
		     unsigned char *out, size_t outcap, size_t *outlen)
{
	size_t i; long spin;

	wr(MK_MODE, (uint32_t)(mode | (pset << 2)));
	wr(MK_CTRL, MKC_INRST);
	i = 0;
	if (packed) {
		for (; i + 4 <= inlen; i += 4)
			wr(MK_INDATA4, (uint32_t)in[i] | ((uint32_t)in[i+1] << 8)
				       | ((uint32_t)in[i+2] << 16)
				       | ((uint32_t)in[i+3] << 24));
	}
	for (; i < inlen; i++)
		wr(MK_INDATA, in[i]);
	if (rd(MK_INPTR) != inlen) return -1;

	wr(MK_CTRL, MKC_START);
	for (spin = 0; spin < 200000000L; spin++)
		if (rd(MK_STATUS) & MKS_DONE) break;
	if (!(rd(MK_STATUS) & MKS_DONE)) return -2;

	*outlen = rd(MK_OUTLEN);
	if (*outlen > outcap) return -3;
	i = 0;
	if (packed) {
		for (; i + 4 <= *outlen; i += 4) {
			uint32_t v = rd(MK_OUTDAT4);
			out[i]   = (unsigned char)(v & 0xFF);
			out[i+1] = (unsigned char)((v >> 8) & 0xFF);
			out[i+2] = (unsigned char)((v >> 16) & 0xFF);
			out[i+3] = (unsigned char)((v >> 24) & 0xFF);
		}
	}
	for (; i < *outlen; i++)
		out[i] = (unsigned char)(rd(MK_OUTDAT) & 0xFF);
	return 0;
}

static unsigned char inbuf[8192], out_a[8192], out_b[8192];

static int fail;

/* 返回每次毫秒；同时把输出留在 dst 里供比对 */
static double bench(int packed, int mode, int pset,
		    const unsigned char *in, size_t inlen,
		    unsigned char *dst, size_t *dlen, int iters)
{
	double t0, t1;
	int i, rc;

	rc = mlkem_run(packed, mode, pset, in, inlen, dst, sizeof out_a, dlen);
	if (rc) { printf("    ✗ 预热失败 rc=%d（packed=%d mode=%d pset=%d）\n",
			 rc, packed, mode, pset); fail = 1; return -1.0; }
	t0 = now_ms();
	for (i = 0; i < iters; i++)
		mlkem_run(packed, mode, pset, in, inlen, dst, sizeof out_a, dlen);
	t1 = now_ms();
	return (t1 - t0) / iters;
}

static void one(const char *path, int iters)
{
	static const char *SET[3] = { "ML-KEM-512", "ML-KEM-768", "ML-KEM-1024" };
	static const char *OPN[3] = { "KeyGen", "Encaps", "Decaps" };
	int p, op;
	unsigned k;

	printf("\n=== 通路：%s（每项 %d 次）===\n", path, iters);
	printf("  %-12s %-8s %10s %10s %8s\n",
	       "参数集", "操作", "逐字节", "打包", "加速");

	for (p = 0; p < 3; p++) {
		const mlkem_kg_vec *kg = NULL;
		const mlkem_en_vec *en = NULL;
		const mlkem_de_vec *de = NULL;

		for (k = 0; k < sizeof MLKEM_KG / sizeof MLKEM_KG[0]; k++)
			if (MLKEM_KG[k].pset == p) { kg = &MLKEM_KG[k]; break; }
		for (k = 0; k < sizeof MLKEM_EN / sizeof MLKEM_EN[0]; k++)
			if (MLKEM_EN[k].pset == p) { en = &MLKEM_EN[k]; break; }
		for (k = 0; k < sizeof MLKEM_DE / sizeof MLKEM_DE[0]; k++)
			if (MLKEM_DE[k].pset == p) { de = &MLKEM_DE[k]; break; }
		if (!kg || !en || !de) continue;

		for (op = 0; op < 3; op++) {
			size_t inlen, la = 0, lb = 0;
			double ta, tb;

			if (op == 0) {
				memcpy(inbuf, kg->d, 32);
				memcpy(inbuf + 32, kg->z, 32);
				inlen = 64;
			} else if (op == 1) {
				memcpy(inbuf, en->m, 32);
				memcpy(inbuf + 32, en->ek, en->ek_len);
				inlen = 32 + en->ek_len;
			} else {
				memcpy(inbuf, de->dk, de->dk_len);
				memcpy(inbuf + de->dk_len, de->c, de->c_len);
				inlen = de->dk_len + de->c_len;
			}

			ta = bench(0, op, p, inbuf, inlen, out_a, &la, iters);
			tb = bench(1, op, p, inbuf, inlen, out_b, &lb, iters);
			if (ta < 0 || tb < 0) continue;

			/* ⚠️ 判据的主体在这里，不在时间上 */
			if (la != lb || memcmp(out_a, out_b, la) != 0) {
				printf("    ✗ %s %s：两条路的输出不一致"
				       "（%zu vs %zu 字节）—— 打包错位或字节序装反\n",
				       SET[p], OPN[op], la, lb);
				fail = 1;
			}
			printf("  %-12s %-8s %8.2fms %8.2fms %7.2f×\n",
			       SET[p], OPN[op], ta, tb, ta / tb);
			fflush(stdout);
		}
	}
}

int main(int argc, char **argv)
{
	int iters = (argc > 1) ? atoi(argv[1]) : 20;
	int fd;
	uint32_t v;

	if (iters < 1) iters = 20;

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("open /dev/mem"); return 2; }
	mm = mmap(NULL, PL_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PL_BASE);
	if (mm == MAP_FAILED) { perror("mmap"); return 2; }

	via_el3 = 0;
	v = rd(MK_VER);
	printf("mlkem_axi VERSION（/dev/mem）= 0x%08x\n", v);
	if (v != 0x00010000u) {
		printf("✗ 这不是演示位流（送检形态下普通世界读回 0，那是对的）。\n"
		       "  本测量要的是同一进程里两条路的对比，就此停下。\n");
		return 1;
	}
	one("/dev/mem 直连（每字节一笔 AXI）", iters);

	sfd = open("/dev/secmmio", O_RDWR);
	if (sfd < 0) {
		printf("\n（没有 /dev/secmmio，跳过 EL3 那一轮）\n");
	} else if (ioctl(sfd, SECMMIO_ARM, 0) < 0) {
		printf("\n（SECMMIO_ARM 失败，跳过 EL3 那一轮）\n");
	} else {
		via_el3 = 1;
		v = rd(MK_VER);
		printf("\nmlkem_axi VERSION（经 EL3）= 0x%08x\n", v);
		if (v == 0x00010000u)
			one("/dev/secmmio → EL3（每字节一次 ioctl + SMC）", iters);
		else
			printf("（经 EL3 读不到 VERSION，跳过）\n");
		via_el3 = 0;
	}

	printf("\n%s\n", fail ? "iopack_bench: 有失败" : "iopack_bench: 两条路输出全部一致");
	return fail;
}
