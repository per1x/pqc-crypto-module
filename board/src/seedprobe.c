/* seedprobe —— 批 1 种子暂存口的上板探针（**演示位流专用**）
 *
 * ============================================================================
 * 【它验的是哪几句话】
 * ============================================================================
 * 这个程序全部经 /dev/mem 直接发事务 —— 也就是**普通世界自己发的、
 * AxPROT[1]=1 的非安全事务**。演示位流（SECURE_ONLY=0）下整块从机对普通世界
 * 是敞开的，于是这里能单独测到种子口自己那道门，而不是防火墙。
 * 送检位流下跑它没有意义（防火墙会先把一切拦掉），所以开头会自己判形态。
 *
 *   ① 非安全世界**写不进** SEED_DATA：字计数一步不动、被拒计数涨；
 *   ② **而且不产生总线错误** —— 这一条才是今天最要紧的。第一版 RTL 让被拒的
 *      写回 SLVERR，而 AXI 的写是 posted 的，SLVERR 会以 SError 打回内核、
 *      只能 panic，代价是一次断电。改成 RAZ/WI 之后，"这个程序跑完板子还活着"
 *      本身就是判据。所以它**故意去写那个口**，写完还继续跑。
 *   ③ SEED_DATA **读回恒 0**（没有读回路径）；
 *   ④ 整个寄存器窗口里读不到任何"像种子"的东西；
 *   ⑤ ZEROIZE 之后 WIPING 的**实际持续时间**（擦除拍数确证，第 1/2 项的
 *      上板 pending）。
 *
 * ⚠️ 它不碰 CTRL 的 SEED_LOCK / DK_LOCK：那两位一次性、只有重配位流能放开，
 *    在探针里置上等于给后面的测试挖坑。
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#define MLKEM_BASE 0x80030000UL
#define A_VERSION  0x00
#define A_CTRL     0x04
#define A_STATUS   0x08
#define A_SEEDDATA 0x38
#define A_SEEDSTAT 0x3C

#define C_ZEROIZE  (1u << 1)
#define ST_WIPING  (1u << 4)

static volatile uint8_t *m;

static uint32_t rd(unsigned off) { return *(volatile uint32_t *)(m + off); }
static void     wr(unsigned off, uint32_t v)
{
	*(volatile uint32_t *)(m + off) = v;
	/* 写完读一下，逼这笔 posted 写真的走完总线 —— 否则"没崩"可能只是
	 * 因为它还堵在写缓冲里，测不到我们想测的东西。 */
	(void)rd(A_VERSION);
}

static long us_since(struct timeval *t0)
{
	struct timeval t;
	gettimeofday(&t, NULL);
	return (t.tv_sec - t0->tv_sec) * 1000000L + (t.tv_usec - t0->tv_usec);
}

int main(void)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	uint32_t ver, ss0, ss1, st;
	int fail = 0, i;

	if (fd < 0) { perror("open /dev/mem"); return 2; }
	m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, MLKEM_BASE);
	if (m == MAP_FAILED) { perror("mmap"); return 2; }

	ver = rd(A_VERSION);
	printf("mlkem_axi VERSION = 0x%08x\n", ver);
	if (ver != 0x00010000u) {
		printf("  ✗ 读不到 VERSION —— 要么不是演示位流（送检形态下普通世界\n"
		       "     读回 0，那是对的），要么位流没装。这个探针只在演示位流下\n"
		       "     有意义，就此停下。\n");
		return 1;
	}

	ss0 = rd(A_SEEDSTAT);
	printf("SEED_STAT 初值 = 0x%08x（字数=%u 被拒计数=%u）\n",
	       ss0, ss0 & 0x1F, ss0 >> 16);

	/* ---- ①② 非安全写：写不进去，而且不能把板子搞崩 ---- */
	printf("\n[1] 用非安全事务往 SEED_DATA 写 4 笔"
	       "（这一步在旧 RTL 上会 SError → panic → 断电）\n");
	fflush(stdout); sync();
	for (i = 0; i < 4; i++) {
		wr(A_SEEDDATA, 0xDEADBE00u + i);
	}
	printf("    ✓ 四笔写完，板子还活着 —— 被拒的写没有产生总线错误\n");

	ss1 = rd(A_SEEDSTAT);
	printf("    SEED_STAT 现在 = 0x%08x（字数=%u 被拒计数=%u）\n",
	       ss1, ss1 & 0x1F, ss1 >> 16);
	if ((ss1 & 0x1F) != 0) {
		printf("    ✗ 字计数动了 —— 非安全世界把种子写进去了\n"); fail = 1;
	} else {
		printf("    ✓ 字计数一步没动\n");
	}
	if ((ss1 >> 16) != (ss0 >> 16) + 4) {
		printf("    ✗ 被拒计数从 %u 变成 %u，应当 +4\n",
		       ss0 >> 16, ss1 >> 16); fail = 1;
	} else {
		printf("    ✓ 被拒计数 +4，留痕了\n");
	}

	/* ---- ③ 读回恒 0 ---- */
	printf("\n[2] 读 SEED_DATA（应当恒为 0，它没有读回路径）\n");
	for (i = 0; i < 4; i++) {
		uint32_t v = rd(A_SEEDDATA);
		if (v != 0) { printf("    ✗ 第 %d 次读到 0x%08x\n", i, v); fail = 1; }
	}
	if (!fail) printf("    ✓ 四次读回全是 0\n");

	/* ---- ④ 整个窗口扫一遍，不许出现刚才写进去的那几个值 ---- */
	printf("\n[3] 扫整个寄存器窗口 0x00-0x3C，不许出现 0xDEADBE0x\n");
	{
		int leak = 0;
		for (i = 0; i <= 0x3C; i += 4) {
			uint32_t v = rd((unsigned)i);
			if ((v & 0xFFFFFF00u) == 0xDEADBE00u) {
				printf("    ✗ 偏移 0x%02x 读到 0x%08x\n", i, v);
				leak = 1; fail = 1;
			}
		}
		if (!leak) printf("    ✓ 一处都没有\n");
	}

	/* ---- ⑤ 擦除拍数 ---- */
	printf("\n[4] ZEROIZE → 量 WIPING 实际持续多久"
	       "（RTL 数出来是 65536 拍 @75 MHz ≈ 874 µs，加三个核自己那几台）\n");
	{
		struct timeval t0;
		long us_rise = -1, us_fall = -1;
		long spin;

		gettimeofday(&t0, NULL);
		wr(A_CTRL, C_ZEROIZE);
		for (spin = 0; spin < 20000000L; spin++) {
			st = rd(A_STATUS);
			if (us_rise < 0 && (st & ST_WIPING)) us_rise = us_since(&t0);
			if (us_rise >= 0 && !(st & ST_WIPING)) { us_fall = us_since(&t0); break; }
		}
		if (us_rise < 0) {
			printf("    ✗ WIPING 从来没起来 —— 擦除机没被触发\n"); fail = 1;
		} else if (us_fall < 0) {
			printf("    ✗ WIPING 起来了但一直不落\n"); fail = 1;
		} else {
			printf("    ✓ WIPING 起于 %ld µs、落于 %ld µs，持续约 %ld µs\n",
			       us_rise, us_fall, us_fall - us_rise);
			printf("      （含软件轮询开销；RTL 计数是 65536 拍 ≈ 874 µs）\n");
		}
	}

	/* 擦完之后种子暂存也该是空的 */
	ss1 = rd(A_SEEDSTAT);
	printf("\n[5] 擦除之后 SEED_STAT = 0x%08x（字数应为 0）\n", ss1);
	if ((ss1 & 0x1F) != 0) { printf("    ✗ 字数没清\n"); fail = 1; }
	else printf("    ✓ 字数为 0\n");

	printf("\n%s\n", fail ? "seedprobe: 有失败" : "seedprobe: 全部通过");
	fflush(stdout); sync();
	return fail;
}
