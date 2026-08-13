// trngrestart —— 采 SP 800-90B §3.1.4.3 的重启矩阵（1000 次重启 × 1000 样本）
//
//   trngrestart [行数] [每行样本数] [输出文件] [-a]
//   默认 1000 1000 /media/sd-mmcblk1p2/hsm/restart.bin
//
//   不带 -a：重启后**立刻**取样（含启动暂态）—— 表征噪声源的原始行为
//   带 -a  ：等 startup_done 之后再取样（**运行态**）—— 表征实际进熵池的那些样本
//
// ============================================================================
// 【这个测试要回答什么】
// ============================================================================
// 顺序采集那一套（board/trngraw.c + tools/sp800_90b.py）估的是**一条长序列**
// 的最小熵。它答不了一个问题：**噪声源每次重启之后，是不是都从同一个地方
// 开始？** 如果环振上电后有一段确定性的暖机行为，那顺序数据看着很随机，
// 而"重启后第 k 个样本"这一列可能高度可预测 —— 真实攻击者恰恰能反复重启。
//
// 所以 §3.1.4.3 要求另采一个矩阵：重启 1000 次，每次取前 1000 个样本，
// 按**行**（一次重启内）和按**列**（跨重启的同一位置）各算一次最小熵，取最小。
//
// ============================================================================
// 【⚠️ 这里的 "restart" 是代理，不是原物 —— 必须写在数据旁边】
// ============================================================================
// 标准说的 restart 是**噪声源重新上电**。1000 次真 POR 需要有人拔 1000 次电，
// 这块板上做不到（也没有能自动化 POR 的台子）。
//
// 这里做的是 **TRNG 复位**：写 CTRL 的 ZEROIZE 位，它会
//   · 清空并逐地址擦除输出 FIFO 与原始抽头 FIFO；
//   · 复位调理器的海绵状态；
//   · 让启动健康检测重新跑一遍；
//   · 环振随 enable 停振再起振（trng_source 的 enable 接 !zeroize_active）。
//
// 也就是说**噪声源确实被停掉又起来了**，但芯片没断电、温度没变、电源没重新
// 建立。它覆盖得到"每次起振的相位/暖机是否可预测"，覆盖不到"上电瞬态"。
//
// 这个区别会连同数据一起写进输出文件的头，并在文档里如实标 ——
// 一个代理测试被当成原物报出去，比不做还糟。
//
// ============================================================================
// 【两种取样时机，必须都采、分开报】
// ============================================================================
// **默认（不带 -a）：重启后立刻取。** 要的就是最早那批样本 —— 可预测性若存在
// 就在那里。原始抽头接在 src_valid/src_bit 上、不受 startup_done 门控，
// 所以复位一放开就有数据。
//
// 实测结果：**列 0 完全恒定**（1000 次重启后的第一个样本全是 0），列 1..48
// 熵值递减恢复，约第 49 列之后正常。也就是环振有一段确定性启动暂态，
// 大约前 50 个样本（DECIM=8 @75 MHz ≈ 5.3 µs）。
//
// **带 -a：等 startup_done 之后再取。** 这才是**实际进熵池**的那些样本：
// trng_top 把调理器的 bit_valid 门控在 startup_done 上，而启动检测要连续吃
// 满 STARTUP_SAMPLES=1024 个样本才放行 —— 1024 远大于 48。
//
// 两个都要采：只采前者会得出"熵源不合格"的错误结论（那些样本根本没被用）；
// 只采后者则是在回避问题（等于假设暂态存在但不去量它）。
// 分开采、分开报，才能说清"§4.3 的丢弃不是形式，它盖住了一段实测存在的暂态"。
#define _GNU_SOURCE
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

#define PL_BASE    0x80000000UL
#define S_TRNG     0x00000

#define REG_CTRL   (S_TRNG + 0x00)
#define REG_STATUS (S_TRNG + 0x04)
#define REG_RAW    (S_TRNG + 0x30)
#define REG_DROPS  (S_TRNG + 0x34)

#define C_ENABLE   (1u << 0)
#define C_ZEROIZE  (1u << 1)
/* 位定义抄自 trng_axi.v 的 status_word（248 行起）：
 *   [9] raw_valid  [5] startup_done  [2] alarm  [0] ready
 * ⚠️ 别把 startup_done 记成 bit 2 —— 那是 alarm。写错的话这一轮
 *   会一直等一个永远不来的位，白跑一次板子。 */
#define ST_ALARM     (1u << 2)
#define ST_STARTUP   (1u << 5)
#define ST_RAW_VALID (1u << 9)

static int sec_fd = -1;

static uint32_t rd(unsigned off)
{
	struct secmmio_op op = { .addr = (uint32_t)(PL_BASE + off), .val = 0 };

	if (ioctl(sec_fd, SECMMIO_RD, &op) < 0) {
		fprintf(stderr, "EL3 读 0x%08lx 被拒\n", PL_BASE + off);
		exit(3);
	}
	return op.val;
}

static void wr(unsigned off, uint32_t v)
{
	struct secmmio_op op = { .addr = (uint32_t)(PL_BASE + off), .val = v };

	if (ioctl(sec_fd, SECMMIO_WR, &op) < 0) {
		fprintf(stderr, "EL3 写 0x%08lx 被拒\n", PL_BASE + off);
		exit(3);
	}
}

int main(int argc, char **argv)
{
	int after_startup = 0;
	long rows = (argc > 1) ? atol(argv[1]) : 1000;
	long cols = (argc > 2) ? atol(argv[2]) : 1000;
	const char *out = (argc > 3) ? argv[3]
				     : "/media/sd-mmcblk1p2/hsm/restart.bin";
	long r, got_bits, guard;
	uint32_t w, drops0, drops1;
	unsigned char *row;
	FILE *f, *st;
	char state[64] = {0};
	int i;

	for (i = 1; i < argc; i++)
		if (strcmp(argv[i], "-a") == 0) after_startup = 1;

	/* 闸门：先确认 PL 已 programmed 再发第一笔 SMC（这一步不碰 PL 总线） */
	st = fopen("/sys/class/fpga_manager/fpga0/state", "r");
	if (!st || !fgets(state, sizeof state, st)) {
		fprintf(stderr, "读不到 fpga_manager state\n"); return 2;
	}
	fclose(st);
	if (!strstr(state, "operating")) {
		fprintf(stderr, "PL 不是 operating（%s），拒绝发 SMC\n", state);
		return 2;
	}

	sec_fd = open("/dev/secmmio", O_RDWR);
	if (sec_fd < 0) { perror("/dev/secmmio"); return 2; }
	if (ioctl(sec_fd, SECMMIO_ARM) < 0) { perror("ARM"); return 2; }

	row = malloc((cols + 31) / 8 + 4);
	if (!row) return 1;

	f = fopen(out, "wb");
	if (!f) { perror(out); return 1; }

	/* 文件头：把"这里的 restart 是什么"写进数据本身，
	 * 免得日后有人拿它当真 POR 的重启测试用。 */
	fprintf(f, "#SP800-90B-restart-matrix rows=%ld cols=%ld\n", rows, cols);
	fprintf(f, "#restart=TRNG-ZEROIZE (noise source stopped+restarted, "
		   "NOT a power-on reset; chip stayed powered, temperature "
		   "unchanged)\n");
	fprintf(f, "#tap=RAW (pre-conditioning, src_valid/src_bit)\n");
	fprintf(f, "#sampling=%s\n", after_startup
		? "AFTER startup_done AND draining 128 raw words (operational: "
		  "these are samples produced after the startup health test passed)"
		: "IMMEDIATELY after restart (includes the ring-oscillator "
		  "startup transient; these samples are discarded in operation "
		  "by the 1024-sample startup health test)");
	fprintf(f, "#binary payload follows: rows x ceil(cols/8) bytes, "
		   "MSB-first within each byte\n");

	wr(REG_CTRL, C_ENABLE);
	drops0 = rd(REG_DROPS);

	for (r = 0; r < rows; r++) {
		/* ---- 重启噪声源 ---- */
		wr(REG_CTRL, C_ENABLE | C_ZEROIZE);
		wr(REG_CTRL, C_ENABLE);

		/* ---- -a：等启动检测过，**并且把抽头 FIFO 抽干** ---- */
		if (after_startup) {
			guard = 0;
			while (!(rd(REG_STATUS) & ST_STARTUP) && guard++ < 5000000)
				;
			if (guard >= 5000000) {
				fprintf(stderr, "第 %ld 行等不到 startup_done\n", r);
				fclose(f); return 5;
			}
			/* ⚠️ 只等 startup_done 是不够的 —— 抽头 FIFO 深 64 字，
			 * 满了**丢新的、留旧的**。等待期间它早被重启后最早那
			 * 2048 个样本填满了，等完再读，读到的还是那批。
			 *
			 * 第一版就是这么错的，症状很清楚：运行态矩阵的列 0 依然
			 * 全 0（与"立刻采"那版一模一样），而各行内容又互不相同 ——
			 * 只有"第一个样本恒定、其余随机"能同时解释这两件事。
			 *
			 * 所以先丢掉 128 个字（4096 样本，是 FIFO 深度的两倍），
			 * 保证之后读到的确实是启动检测通过之后产生的样本。 */
			for (i = 0; i < 128; i++) {
				guard = 0;
				while (!(rd(REG_STATUS) & ST_RAW_VALID)
				       && guard++ < 100000)
					;
				(void)rd(REG_RAW);
			}
		}

		/* ---- 取 cols 个样本 ---- */
		memset(row, 0, (cols + 31) / 8 + 4);
		got_bits = 0;
		guard = 0;
		while (got_bits < cols && guard++ < 2000000) {
			if (!(rd(REG_STATUS) & ST_RAW_VALID))
				continue;
			w = rd(REG_RAW);
			for (i = 31; i >= 0 && got_bits < cols; i--) {
				if ((w >> i) & 1u)
					row[got_bits >> 3] |= 0x80u >> (got_bits & 7);
				got_bits++;
			}
		}
		if (got_bits < cols) {
			fprintf(stderr, "第 %ld 行只取到 %ld 个样本，中止\n",
				r, got_bits);
			fclose(f);
			return 4;
		}
		fwrite(row, 1, (size_t)((cols + 7) / 8), f);

		if ((r % 100) == 0) {
			printf("  %ld/%ld 行\n", r, rows);
			fflush(stdout);
		}
	}
	drops1 = rd(REG_DROPS);
	fclose(f);

	printf("采完：%ld x %ld，写到 %s\n", rows, cols, out);
	printf("取样 FIFO 溢出计数 %u -> %u（差 %u）\n",
	       drops0, drops1, drops1 - drops0);
	printf("注意：这里的 restart 是 TRNG 复位，**不是**真 POR —— "
	       "已写进文件头，分析与文档必须照此口径。\n");
	return 0;
}
