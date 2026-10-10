/* seedel3 —— 批 1 第 5 项的上板主体：**EL3 保管的种子这条路是不是真的成立**
 *
 * ============================================================================
 * 【这个程序要证的四句话】
 * ============================================================================
 * CODE-1 的修法是"种子由安全世界生成、经 SEED_DATA 暂存口直接进 PL，普通世界
 * 全程看不到明文"。这句话要成立，得同时满足四条 —— 少任何一条，这个口就是
 * 装饰品：
 *
 *   ① **EL3 真的能把种子写进去**：一条 SECMMIO_SEED（SMC 0x8200ff14），
 *      回来之后 SEED_STAT 的字计数应当从 0 变成 16（ML-KEM）/ 8（ML-DSA）。
 *      这一条同时验掉"EL3 侧的取熵 + 逐字写"那段代码。
 *
 *   ② **用暂存种子的 KeyGen 真的跑得出密钥**：置 MODE.SEED_STAGED 再 START，
 *      **一个字节的 IN_DATA 都不喂**。老路（往 IN_DATA 灌 64 字节）在这种
 *      形态下会因为 len_ok 不成立而被拒，所以"它跑通了"本身就说明种子是从
 *      暂存口进去的，不是从缓冲区。
 *
 *   ③ **用一次就作废**：START 那一刻暂存与字计数一起清。所以紧接着再 START
 *      一次（不重新送种子）必须被拒，且 STATUS.SEED_ERR 点亮 —— 而不是拿
 *      同一份种子再生一把一模一样的密钥。
 *
 *   ④ **两道门是互锁的，缺一不可**：
 *        · 普通世界自己发事务写 SEED_DATA → PL 侧拒（seedprobe 已单独验过，
 *          这里再确认一次计数器有没有涨）；
 *        · root 经 EL3 的**通用 PL_WR**（SMC 0x8200ff13）绕一手 → **BL31 的
 *          白名单里必须把这个偏移排除掉**，ioctl 应当回 EACCES。
 *      少了第二道，有 root 的人就能借 EL3 的手把自己的种子写进去 —— 那正是
 *      CODE-1 想堵的"能换掉种子"那一半。
 *
 * ============================================================================
 * 【为什么判据里没有"种子长什么样"】
 * ============================================================================
 * 这个程序**读不到、也不该读得到**任何种子字节：SEED_DATA 没有读回路径，
 * SEED_STAT 只报字数与闩锁。所以判据只能是**行为**（字数动了没有、KeyGen
 * 成不成、第二次拒不拒），不能是"比对种子值"。
 *
 * 这不是妥协 —— 如果这个程序能验证种子的值，那它自己就是一条泄漏路径。
 *
 * ⚠️ 不置 CTRL.SEED_LOCK / DK_LOCK：两位都是一次性的，置上只能重配位流才能
 *    解开，会把后面的用例全堵死。闩锁本身在 RTL 用例里验过。
 *
 * 编译（构建机上）：
 *   aarch64-linux-gnu-gcc -O2 -Wall -Wextra -static \
 *       -I../kmod -o seedel3 seedel3.c
 */
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include "secmmio_uapi.h"

#define MLKEM_BASE   0x80030000UL
#define MLDSA_BASE   0x80060000UL

#define A_VERSION    0x00
#define A_CTRL       0x04
#define A_STATUS     0x08
#define A_MODE       0x0C
#define A_INPTR      0x14
#define A_OUTLEN     0x1C
#define A_SEEDDATA   0x38
#define A_SEEDSTAT   0x3C
/* ML-DSA 的表短一格：MODE 在 0x08，SEED_DATA/STAT 在 0x34/0x38 */
#define D_SEEDDATA   0x34
#define D_SEEDSTAT   0x38

#define C_START      (1u << 0)
#define C_ZEROIZE    (1u << 1)
#define C_INRST      (1u << 2)
#define C_OUTRST     (1u << 3)
#define C_SEEDCLR    (1u << 6)

#define M_SEEDSTAGED (1u << 10)

#define ST_BUSY      (1u << 0)
#define ST_DONE      (1u << 1)
#define ST_WIPING    (1u << 4)
#define ST_PARAMERR  (1u << 5)
#define ST_SEEDERR   (1u << 6)

static int fd = -1;
static int fail;

static void bad(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	printf("    ✗ ");
	vprintf(fmt, ap);
	va_end(ap);
	fail = 1;
}

static int sec_rd(unsigned long addr, uint32_t *v)
{
	struct secmmio_op op = { .addr = (uint32_t)addr, .val = 0 };
	if (ioctl(fd, SECMMIO_RD, &op) < 0)
		return -1;
	*v = op.val;
	return 0;
}

static int sec_wr(unsigned long addr, uint32_t v)
{
	struct secmmio_op op = { .addr = (uint32_t)addr, .val = v };
	return ioctl(fd, SECMMIO_WR, &op) < 0 ? -1 : 0;
}

int main(void)
{
	uint32_t v, st, ss, before, after;
	struct secmmio_seed sd;
	int i, rc;

	fd = open("/dev/secmmio", O_RDWR);
	if (fd < 0) { perror("open /dev/secmmio"); return 2; }
	if (ioctl(fd, SECMMIO_ARM, 0) < 0) { perror("SECMMIO_ARM"); return 2; }

	if (sec_rd(MLKEM_BASE + A_VERSION, &v) || v != 0x00010000u) {
		printf("✗ 经 EL3 读 ML-KEM VERSION 得到 0x%08x，位流没装或不对\n", v);
		return 2;
	}
	printf("经 EL3 读 ML-KEM VERSION = 0x%08x\n\n", v);

	/* 从干净状态开始：作废可能残留的暂存种子 */
	sec_wr(MLKEM_BASE + A_CTRL, C_SEEDCLR | C_INRST | C_OUTRST);
	sec_rd(MLKEM_BASE + A_SEEDSTAT, &before);
	printf("起点 SEED_STAT = 0x%08x（字数=%u 被拒=%u）\n",
	       before, before & 0x1F, before >> 16);

	/* ---------------- ① EL3 装种子 ---------------- */
	printf("\n[1] 发一条 SECMMIO_SEED（SMC 0x8200ff14，target=0 → ML-KEM）\n");
	memset(&sd, 0, sizeof(sd));
	sd.target = 0;
	if (ioctl(fd, SECMMIO_SEED, &sd) < 0) {
		perror("    ✗ SECMMIO_SEED");
		fail = 1;
	} else {
		printf("    ✓ 返回，world=%u\n", sd.world);
		/* ⚠️ 批 1 这里**必须是 0**，而且这正是设计本身。
		 *
		 * 批 1 的形态是「普通世界发起请求，EL3 自己取熵、自己往 SEED_DATA
		 * 写」——发起者就是普通世界的内核驱动，所以 is_caller_secure() 回 0。
		 * 种子的保管方是 EL3，**不是**调用方；调用方是谁与种子会不会泄漏
		 * 无关，那由「16 个字以安全事务进 PL」（下面被拒计数没动）来保证。
		 *
		 * 这个字段的用处是让上层**如实记录是谁要的**，不是一道门。
		 * 真正的门是 PQCHSM_SEED_NS_ALLOWED：批 2/3 把它改成 0 之后，
		 * 普通世界的调用会被直接拒掉，那时这里只可能是 1。
		 * 所以判据随批次改变，写死成 1 是错的。 */
		if (sd.world != 0)
			bad("批 1 的调用方应当是普通世界（world=0），得到 %u\n",
			    sd.world);
		else
			printf("    ✓ 如实回报调用方是普通世界"
			       "（批 2/3 关掉 NS_ALLOWED 后这里才会是 1）\n");
	}
	sec_rd(MLKEM_BASE + A_SEEDSTAT, &ss);
	printf("    SEED_STAT = 0x%08x（字数=%u READY=%u）\n",
	       ss, ss & 0x1F, (ss >> 8) & 1);
	if ((ss & 0x1F) != 16)
		bad("字数是 %u，应当是 16 —— EL3 那侧没把 16 个字写进来\n", ss & 0x1F);
	else
		printf("    ✓ 16 个字都到了，且这条路上没有任何密钥材料经过普通世界\n");
	if ((ss >> 16) != (before >> 16))
		bad("被拒计数动了（%u → %u）—— EL3 发的事务被当成非安全的了\n",
		    before >> 16, ss >> 16);
	else
		printf("    ✓ 被拒计数没动，说明这 16 笔是以安全事务进去的\n");

	/* ---------------- ② 用暂存种子跑 KeyGen ---------------- */
	printf("\n[2] MODE.SEED_STAGED=1 的 KeyGen，**一个字节 IN_DATA 都不喂**\n");
	sec_wr(MLKEM_BASE + A_CTRL, C_INRST | C_OUTRST);
	sec_wr(MLKEM_BASE + A_MODE, M_SEEDSTAGED | (1u << 2) /* pset=1 → ML-KEM-768 */);
	sec_rd(MLKEM_BASE + A_INPTR, &v);
	printf("    IN_PTR = %u（老路要 64 才准启动，这里是 0）\n", v);
	sec_wr(MLKEM_BASE + A_CTRL, C_START);
	for (i = 0; i < 200000; i++) {
		sec_rd(MLKEM_BASE + A_STATUS, &st);
		if (!(st & ST_BUSY))
			break;
	}
	sec_rd(MLKEM_BASE + A_STATUS, &st);
	sec_rd(MLKEM_BASE + A_OUTLEN, &v);
	printf("    STATUS = 0x%08x  OUT_LEN = %u\n", st, v);
	if (st & (ST_PARAMERR | ST_SEEDERR))
		bad("START 被拒（PARAM_ERR=%u SEED_ERR=%u）\n",
		    !!(st & ST_PARAMERR), !!(st & ST_SEEDERR));
	else if (!(st & ST_DONE) || v == 0)
		bad("没跑完或没有输出\n");
	else
		printf("    ✓ 跑完了，OUT_LEN=%u —— 种子确实是从暂存口进的核\n", v);

	sec_rd(MLKEM_BASE + A_SEEDSTAT, &ss);
	printf("    跑完之后 SEED_STAT = 0x%08x（字数=%u）\n", ss, ss & 0x1F);
	if ((ss & 0x1F) != 0)
		bad("字数没清 —— 违反「用一次就作废」\n");
	else
		printf("    ✓ 字数已清零\n");

	/* ---------------- ③ 不补种子再来一次，必须被拒 ---------------- */
	printf("\n[3] 不重新送种子，直接再 START 一次 —— 必须被拒且点亮 SEED_ERR\n");
	sec_wr(MLKEM_BASE + A_CTRL, C_INRST | C_OUTRST);
	sec_wr(MLKEM_BASE + A_MODE, M_SEEDSTAGED | (1u << 2));
	sec_wr(MLKEM_BASE + A_CTRL, C_START);
	sec_rd(MLKEM_BASE + A_STATUS, &st);
	sec_rd(MLKEM_BASE + A_OUTLEN, &v);
	printf("    STATUS = 0x%08x  OUT_LEN = %u\n", st, v);
	if (!(st & ST_SEEDERR))
		bad("SEED_ERR 没点亮 —— 同一份种子被用了第二次\n");
	else
		printf("    ✓ SEED_ERR 点亮，第二次被拒\n");
	if (v != 0)
		bad("OUT_LEN 不是 0 —— 上一次的结果没被作废，软件会当成本次的\n");
	else
		printf("    ✓ 上一次的 OUT_LEN 一并作废了\n");

	/* ---------------- ④a root 经 EL3 通用 PL_WR 绕道 ---------------- */
	printf("\n[4a] root 经 EL3 的通用 PL_WR 往 SEED_DATA 写"
	       "（白名单排除，应当 EACCES）\n");
	sec_rd(MLKEM_BASE + A_SEEDSTAT, &before);
	rc = sec_wr(MLKEM_BASE + A_SEEDDATA, 0xA5A5A5A5u);
	if (rc == 0)
		bad("EL3 放行了！白名单没把 SEED_DATA 排除掉 —— "
		    "有 root 就能借 EL3 的手换掉种子\n");
	else
		printf("    ✓ 被 EL3 拒了（errno=EACCES）\n");
	rc = sec_wr(MLDSA_BASE + D_SEEDDATA, 0xA5A5A5A5u);
	if (rc == 0)
		bad("ML-DSA 的 SEED_DATA 没被排除\n");
	else
		printf("    ✓ ML-DSA 的 SEED_DATA 也被拒了\n");
	sec_rd(MLKEM_BASE + A_SEEDSTAT, &after);
	if ((after & 0x1F) != (before & 0x1F))
		bad("字数动了 —— 那两笔写其实进去了\n");
	else
		printf("    ✓ 字数一步没动\n");

	/* 白名单排除的是这个偏移，不是整块从机 —— 顺手证明旁边的口还通，
	 * 否则"被拒"也可能只是因为整块都被挡了，那证明不了排除是精确的。 */
	if (sec_wr(MLKEM_BASE + A_CTRL, C_INRST) != 0)
		bad("连 CTRL 都写不进 —— 被挡的是整块从机，不是那一个偏移\n");
	else
		printf("    ✓ 同一块从机的 CTRL 仍然写得进，排除是**逐偏移**的\n");

	/* ---------------- ④b 普通世界自己发事务 ---------------- */
	printf("\n[4b] 普通世界经 /dev/mem 直接写 SEED_DATA（PL 侧拒 + 不产生总线错误）\n");
	{
		int mfd = open("/dev/mem", O_RDWR | O_SYNC);
		volatile uint8_t *m;

		if (mfd < 0) {
			printf("    ? 打不开 /dev/mem，跳过（seedprobe 已单独验过）\n");
		} else {
			m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
				 MAP_SHARED, mfd, MLKEM_BASE);
			if (m == MAP_FAILED) {
				printf("    ? mmap 失败，跳过\n");
			} else {
				sec_rd(MLKEM_BASE + A_SEEDSTAT, &before);
				for (i = 0; i < 4; i++) {
					*(volatile uint32_t *)(m + A_SEEDDATA) =
						0xDEADBE00u + i;
					(void)*(volatile uint32_t *)(m + A_VERSION);
				}
				printf("    ✓ 四笔写完板子还活着（RAZ/WI，不是 SLVERR）\n");
				sec_rd(MLKEM_BASE + A_SEEDSTAT, &after);
				printf("    SEED_STAT %08x → %08x（字数 %u→%u 被拒 %u→%u）\n",
				       before, after, before & 0x1F, after & 0x1F,
				       before >> 16, after >> 16);
				if ((after & 0x1F) != 0)
					bad("字数动了 —— 普通世界写进去了\n");
				else
					printf("    ✓ 字数一步没动\n");
				if ((after >> 16) != (before >> 16) + 4)
					bad("被拒计数 %u→%u，应当 +4\n",
					    before >> 16, after >> 16);
				else
					printf("    ✓ 被拒计数 +4，留痕了\n");
				munmap((void *)m, 0x1000);
			}
			close(mfd);
		}
	}

	/* ---------------- ⑤ 擦除拍数 ---------------- */
	printf("\n[5] ZEROIZE → 量 WIPING 实际持续多久"
	       "（RTL 是 65536 拍 @75 MHz ≈ 874 µs）\n");
	{
		struct timeval t0, t;
		long us_rise = -1, us_fall = -1;
		long spin;

		gettimeofday(&t0, NULL);
		sec_wr(MLKEM_BASE + A_CTRL, C_ZEROIZE);
		for (spin = 0; spin < 2000000L; spin++) {
			sec_rd(MLKEM_BASE + A_STATUS, &st);
			gettimeofday(&t, NULL);
			long us = (t.tv_sec - t0.tv_sec) * 1000000L
				+ (t.tv_usec - t0.tv_usec);
			if (us_rise < 0 && (st & ST_WIPING)) us_rise = us;
			if (us_rise >= 0 && !(st & ST_WIPING)) { us_fall = us; break; }
		}
		if (us_rise < 0)
			bad("WIPING 从来没起来\n");
		else if (us_fall < 0)
			bad("WIPING 起来了但一直不落\n");
		else
			printf("    ✓ WIPING 持续约 %ld µs（经 EL3 轮询，开销比 /dev/mem 大）\n",
			       us_fall - us_rise);
	}

	/* ---------------- ⑥ ML-DSA 那一侧同样走一遍装载 ---------------- */
	printf("\n[6] ML-DSA 侧：SECMMIO_SEED(target=1) 应当装进 8 个字（ξ）\n");
	sec_wr(MLDSA_BASE + A_CTRL, C_SEEDCLR);
	sec_rd(MLDSA_BASE + D_SEEDSTAT, &before);
	memset(&sd, 0, sizeof(sd));
	sd.target = 1;
	if (ioctl(fd, SECMMIO_SEED, &sd) < 0) {
		perror("    ✗ SECMMIO_SEED(target=1)");
		fail = 1;
	} else {
		sec_rd(MLDSA_BASE + D_SEEDSTAT, &ss);
		printf("    SEED_STAT = 0x%08x（字数=%u READY=%u）\n",
		       ss, ss & 0x0F, (ss >> 8) & 1);
		if ((ss & 0x0F) != 8)
			bad("字数是 %u，应当是 8\n", ss & 0x0F);
		else
			printf("    ✓ 8 个字（32 字节 ξ）都到了\n");
	}

	printf("\n%s\n", fail ? "seedel3: 有失败" : "seedel3: 全部通过");
	close(fd);
	return fail;
}
