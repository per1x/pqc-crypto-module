// trngraw —— 从 PL 的 TRNG 导出**调理前**的原始噪声比特，供 SP 800-90B 评估
//
//   trngraw <字数> <输出文件>
//
// ============================================================================
// 【为什么必须是调理前的】
// ============================================================================
// SP 800-90B 评估的是**噪声源**，不是调理器。调理器（这里是 SHA-3 海绵）的
// 输出无论输入熵多低，看起来都像均匀随机 —— 拿 RDATA（0x08）跑
// EntropyAssessment 会得到一个非常漂亮、但**毫无意义**的数字，而且正好会
// 骗过想少做一步的人。所以读的是 RAW_DATA（0x30），它接在 src_valid/src_bit
// 上，也就是健康检测（RCT/APT）吃的同一条流。
//
// 这个寄存器只在 RAW_TAP=1 的**表征版 bitstream** 里存在；产品版里整条通路
// 连寄存器一起不存在（不是"读了返回 0"）。
//
// ============================================================================
// 【采集是有间隙的，这一点必须写进报告】
// ============================================================================
// 抽头 FIFO 只有 64 字深，满了**丢新的**（不回压噪声源 —— 回压会改变被评估
// 的那条流的统计性质）。软件读不过噪声源的产出速度，所以采到的是
// "一段一段"的样本，不是连续流。
//
// 对最小熵估计本身没有影响：那套估计器看的是样本序列的分布与相关性，
// 中间整段缺失只相当于换了一段采集窗口。但**重启测试（restart tests）不能
// 用这份数据**——那一项要求按规定的重启结构采集。报告里要照此说明。
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "kmod/secmmio_uapi.h"
#include <unistd.h>

#define TRNG_BASE 0x80000000UL
#define REG_CTRL   0x00
#define REG_STATUS 0x04
#define REG_RAW    0x30

#define ST_READY      (1u << 0)
#define ST_ALARM      (1u << 2)
#define ST_STARTUP    (1u << 5)
#define ST_RAW_VALID  (1u << 9)

/* ---- transport：直接 mmap 还是经 EL3 ----------------------------------------
 * 表征版 bitstream 里核仍然是 SECURE_ONLY=1，普通世界直接读一律 DECERR，
 * 所以默认经 /dev/secmmio 由 EL3 发。加 -d 才走直接 mmap（只对
 * SECURE_ONLY=0 的旧 bitstream 有意义）。 */
static int sec_fd = -1;
static volatile uint8_t *g_m;

static uint32_t rdreg(unsigned off)
{
	struct secmmio_op op = { .addr = (uint32_t)(TRNG_BASE + off), .val = 0 };

	if (sec_fd < 0)
		return *(volatile uint32_t *)(g_m + off);
	if (ioctl(sec_fd, SECMMIO_RD, &op) < 0) {
		fprintf(stderr, "EL3 读 0x%08lx 被拒\n", TRNG_BASE + off);
		exit(3);
	}
	return op.val;
}

static void wrreg(unsigned off, uint32_t v)
{
	struct secmmio_op op = { .addr = (uint32_t)(TRNG_BASE + off), .val = v };

	if (sec_fd < 0) { *(volatile uint32_t *)(g_m + off) = v; return; }
	if (ioctl(sec_fd, SECMMIO_WR, &op) < 0) {
		fprintf(stderr, "EL3 写 0x%08lx 被拒\n", TRNG_BASE + off);
		exit(3);
	}
}

static void sec_open(void)
{
	char st[64] = {0};
	FILE *f = fopen("/sys/class/fpga_manager/fpga0/state", "r");

	if (!f || !fgets(st, sizeof st, f)) {
		fprintf(stderr, "读不到 fpga_manager state\n"); exit(2);
	}
	fclose(f);
	if (!strstr(st, "operating")) {
		fprintf(stderr, "PL 不是 operating（%s），拒绝发 SMC\n", st); exit(2);
	}
	sec_fd = open("/dev/secmmio", O_RDWR);
	if (sec_fd < 0) { perror("/dev/secmmio"); exit(2); }
	if (ioctl(sec_fd, SECMMIO_ARM) < 0) { perror("ARM"); exit(2); }
}

int main(int argc, char **argv)
{
	unsigned long want = (argc > 1) ? strtoul(argv[1], NULL, 0) : 32768;
	const char *out = (argc > 2) ? argv[2] : "/tmp/trngraw.bin";
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	volatile uint8_t *m;
	uint32_t *buf;
	unsigned long got = 0, spins = 0;
	FILE *f;
	int direct = 0;
	int ai;

	for (ai = 1; ai < argc; ai++)
		if (strcmp(argv[ai], "-d") == 0) direct = 1;

	if (direct) {
		if (fd < 0) { perror("open /dev/mem"); return 1; }
		m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
			 TRNG_BASE);
		if (m == MAP_FAILED) { perror("mmap"); return 1; }
		g_m = m;
	} else {
		sec_open();
		m = NULL;
	}

	/* 使能，等启动健康检测过 */
	wrreg(REG_CTRL, 1);
	for (spins = 0; spins < 100000000UL; spins++) {
		uint32_t st = rdreg(REG_STATUS);
		if (st & ST_STARTUP) break;
	}
	{
		uint32_t st = rdreg(REG_STATUS);
		printf("STATUS=0x%08x  ready=%d startup=%d alarm=%d raw_valid=%d\n",
		       st, !!(st & ST_READY), !!(st & ST_STARTUP),
		       !!(st & ST_ALARM), !!(st & ST_RAW_VALID));
		fflush(stdout);
		if (!(st & ST_RAW_VALID)) {
			/*
			 * RAW_VALID 一直是 0 有两种可能，必须区分开，
			 * 否则会把"装错 bitstream"当成"熵源坏了"：
			 *   · 装的是产品版 bitstream（RAW_TAP=0）—— 这条通路不存在；
			 *   · 噪声源真的没在出样本。
			 * 前者更常见，所以先提示它。
			 */
			fprintf(stderr,
			        "RAW_VALID=0：多半是装了产品版 bitstream（RAW_TAP=0，"
			        "这条通路根本不存在）。要用 zu3eg_hsm_char.bit。\n");
			return 2;
		}
	}

	buf = malloc(want * sizeof(uint32_t));
	if (!buf) { perror("malloc"); return 1; }

	/* 尽量快地抽 —— FIFO 只有 64 字深，慢了就丢样本（丢是设计如此，见文件头）*/
	spins = 0;
	while (got < want) {
		uint32_t st = rdreg(REG_STATUS);
		if (st & ST_RAW_VALID) {
			buf[got++] = rdreg(REG_RAW);
		} else if (++spins > 2000000000UL) {
			fprintf(stderr, "等不到样本，只取到 %lu 字\n", got);
			break;
		}
	}

	f = fopen(out, "wb");
	if (!f) { perror("fopen"); return 1; }
	fwrite(buf, sizeof(uint32_t), got, f);
	fclose(f);
	printf("取到 %lu 字（%lu 比特）→ %s\n", got, got * 32, out);

	/* 采集期间健康检测有没有报警 —— 报警了这份数据就不能用来评估 */
	{
		uint32_t st = rdreg(REG_STATUS);
		printf("采集后 STATUS=0x%08x alarm=%d rct=%d apt=%d\n", st,
		       !!(st & ST_ALARM), !!(st & (1u << 3)), !!(st & (1u << 4)));
		if (st & ST_ALARM)
			printf("⚠️ 采集期间健康检测报警了 —— 这份数据不能用来出最小熵\n");
	}
	fflush(stdout);
	return 0;
}
