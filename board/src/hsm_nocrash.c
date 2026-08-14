// hsm_nocrash —— 证明：任何用户态程序都搞不崩这块板
//
//   aarch64-linux-gnu-gcc -O2 -static -o hsm_nocrash hsm_nocrash.c
//
// ============================================================================
// 【这个程序存在的理由】
// ============================================================================
// 密码机的其他性质（算得对、密钥出不来、边界关得住）都已经有对应的证据程序。
// 缺的是这一条：**一个乱来的用户态程序，最坏能把这块板怎么样？**
//
// 在 RAZ/WI 之前，答案是"能让它当场断电"：
//   · 被拒的写在 AXI 上回 DECERR；
//   · aarch64 的写是 **posted** 的 —— 指令早退休了，错误以 **SError** 回来；
//   · SError 不属于任何一条指令，内核接不住，只能 panic。
// 而触发它连恶意都不需要：内核自己的 xgpio_of_probe 探测一个位流里不存在的
// 外设，就打穿过一次（调用栈见 boot/persist/build_j1_boot.sh）。
//
// 改成 RAZ/WI 之后，被拒和没命中的访问一律"读回 0、写丢弃、响应 OKAY"，
// 总线上不产生任何错误。**这个程序就是去把那个结论撞一遍。**
//
// ============================================================================
// 【它故意做的都是以前会出事的事】
// ============================================================================
//   ① 往四个 SECURE_ONLY=1 的核里写 —— 防火墙拒；
//   ② 往镜像地址、越窗地址写 —— 译码器判"没有这个地址"；
//   ③ 往 aperture 以外、不存在的槽写 —— 同上；
//   ④ 未对齐地址读写；
//   ⑤ 上面每一类都重复几千次，而不是一次 —— 一次不出事可能是运气，
//      posted 写的错误本来就是延迟回来的，量小的时候容易蒙混过去。
//
// 通过的判据很朴素：**这个程序自己跑完并退出，而且板子还在。**
// 前者由它自己的返回值说明，后者由"你还能读到这份报告"说明。
//
// ============================================================================
// 【它不是"安全性测试"，别把两件事混了】
// ============================================================================
// 这里一笔写都没生效（那由 hsm_secneg 和仿真证），本程序只证"没生效"这件事
// 是安静地发生的。安全性由防火墙保证，可用性由 RAZ/WI 保证 —— 两条独立。
//
// 最后从**安全世界**读一遍违规计数：RAZ/WI 之后总线上不留痕，那些计数器是
// 唯一的证据链，而且普通世界读不到它们（读到的是 0）。这一步同时证明了
// "留痕了"和"痕迹只有安全世界看得到"。
#define _GNU_SOURCE
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "kmod/secmmio_uapi.h"

#define PL_BASE   0x80000000UL
/* 映射得比 6 个槽宽，好让"aperture 以外"那一类也能真的发出去。
 * 这段地址上什么从机都没有 —— 以前碰它是找死，现在它是用例。 */
#define PL_SPAN   0x00200000UL

#define VERSION_TRUE 0x00010000U
#define ROUNDS       2000

static volatile uint8_t *pl;
static sigjmp_buf busjmp;
static volatile int bus_armed, bus_hit;
static int n_pass, n_fail, n_sigbus;
static FILE *rep;
static int sec_fd = -1;

static void on_bus(int sig)
{
	(void)sig;
	if (bus_armed) { bus_hit = 1; siglongjmp(busjmp, 1); }
	_exit(135);
}

static void ok(const char *f, ...)
{
	va_list a; va_start(a, f);
	fprintf(rep, "  PASS  "); vfprintf(rep, f, a); fprintf(rep, "\n");
	fflush(rep); va_end(a); n_pass++;
}
static void bad(const char *f, ...)
{
	va_list a; va_start(a, f);
	fprintf(rep, "  FAIL  "); vfprintf(rep, f, a); fprintf(rep, "\n");
	fflush(rep); va_end(a); n_fail++;
}

/* 读一次，接住 SIGBUS。RAZ/WI 生效时永远接不到。 */
static uint32_t rd_safe(unsigned long off, int *okp)
{
	uint32_t v = 0;

	bus_hit = 0;
	bus_armed = 1;
	if (sigsetjmp(busjmp, 1) == 0)
		v = *(volatile uint32_t *)(pl + off);
	bus_armed = 0;
	if (okp) *okp = !bus_hit;
	if (bus_hit) n_sigbus++;
	return bus_hit ? 0 : v;
}

/* 写一次。
 *
 * ⚠️ 这个函数就是整个程序的要害。SIGBUS 的处理在这里**基本没用** ——
 *    posted 写的错误是 SError，走的是内核的 do_serror，压根到不了用户态的
 *    信号机制。所以"写完还能往下跑"这件事本身才是结论；接 SIGBUS 只是
 *    为了万一它以同步中止的形式回来时别漏掉。 */
static void wr_hammer(unsigned long off, uint32_t v)
{
	bus_hit = 0;
	bus_armed = 1;
	if (sigsetjmp(busjmp, 1) == 0)
		*(volatile uint32_t *)(pl + off) = v;
	bus_armed = 0;
	if (bus_hit) n_sigbus++;
}

/* 一道栅栏：把 posted 写逼到总线上去，别让它攒在写缓冲里。
 * 少了这一步，"写了一万次没事"可能只是那些写还没真正发出去。 */
static void drain(void)
{
	int g;

	__asm__ volatile ("dsb sy" ::: "memory");
	(void)rd_safe(0x50000, &g);      /* 风扇口，一定读得到，兼作往返栅栏 */
	__asm__ volatile ("dsb sy" ::: "memory");
}

struct site {
	unsigned long off;
	const char *what;
	/* 期望在 CPU 层就产生对齐异常（SIGBUS），事务根本不上总线。
	 * 见下面 hammer() 里的说明 —— 这与 RAZ/WI 是两码事，必须分开判。 */
	int expect_align_fault;
};

/* SIGBUS 必须**按地点**记，不能只记一个总数。
 *
 * 第一版就是记总数，结果报了"4000 次 SIGBUS"却指不出是谁 —— 而
 * 4000 = 2000×2 恰好是一个地点的读加写，也就是说那个数字本身已经在提示
 * 答案了，只是报告的粒度不够，看不出来。粒度不够的报告比没有报告更坏：
 * 它把一个具体的、可解释的现象包装成了一个笼统的失败。 */
static void hammer(const char *title, const struct site *s, size_t n)
{
	size_t i;
	int j, before;

	fprintf(rep, "\n[%s]\n", title);
	for (i = 0; i < n; i++) {
		before = n_sigbus;
		for (j = 0; j < ROUNDS; j++) {
			wr_hammer(s[i].off, 0xDEADBEEFU ^ (uint32_t)j);
			(void)rd_safe(s[i].off, NULL);
		}
		drain();
		int got = n_sigbus - before;

		if (s[i].expect_align_fault) {
			/* aarch64 上 /dev/mem 映出来的是 **Device 内存**，
			 * 它不支持非对齐访问：CPU 在发出总线事务**之前**就抛
			 * 对齐异常，内核转成 SIGBUS。
			 *
			 * 所以这里的 SIGBUS **不是**总线在报错，RAZ/WI 也没
			 * 失效 —— 事务压根没走到 fabric。而且这条路径同样安全：
			 * 对齐异常是同步的、精确的，程序接得住，不会变成 SError。
			 *
			 * 反过来说，如果这里**没有** SIGBUS，才该警惕：那意味着
			 * 非对齐访问真的发出去了，得看译码器怎么处理它。 */
			if (got == 2 * ROUNDS)
				ok("%s：读写各 %d 次，%d 次 SIGBUS —— "
				   "这是 CPU 的对齐异常（Device 内存不支持非对齐），"
				   "事务没上总线，与 RAZ/WI 无关；同步可捕获，崩不了板",
				   s[i].what, ROUNDS, got);
			else
				bad("%s：期望 %d 次对齐异常，实得 %d 次",
				    s[i].what, 2 * ROUNDS, got);
		} else if (got == 0) {
			ok("%s：读写各 %d 次，全程无总线错误，进程还活着",
			   s[i].what, ROUNDS);
		} else {
			bad("%s：出现 %d 次 SIGBUS —— 总线仍在报错，"
			    "RAZ/WI 在这个地址上没生效", s[i].what, got);
		}
	}
}

/* ---- 安全世界侧：读违规计数 ---- */
static int sec_open(void)
{
	char st[64] = {0};
	int f;

	f = open("/sys/class/fpga_manager/fpga0/state", O_RDONLY);
	if (f < 0) return -1;
	if (read(f, st, sizeof st - 1) <= 0) { close(f); return -1; }
	close(f);
	if (!strstr(st, "operating")) return -1;

	sec_fd = open("/dev/secmmio", O_RDWR);
	if (sec_fd < 0) return -1;
	if (ioctl(sec_fd, SECMMIO_ARM) < 0) { close(sec_fd); sec_fd = -1; return -1; }
	return 0;
}

static int sec_rd(unsigned long off, uint32_t *out)
{
	struct secmmio_op op = { .addr = (uint32_t)(PL_BASE + off), .val = 0 };

	if (sec_fd < 0) return -1;
	if (ioctl(sec_fd, SECMMIO_RD, &op) < 0) return -1;
	*out = op.val;
	return 0;
}

int main(void)
{
	/* ① 防火墙拒的：四个功能核 + 金丝雀。地址都是各核合法的 VERSION 偏移，
	 *    所以被拒的一定是 AxPROT 门控，不是译码 —— 两类分开测才说得清。 */
	static const struct site refused[] = {
		{ 0x00020, "trng_axi VERSION      (槽 0，防火墙拒)", 0 },
		{ 0x10000, "key_vault_axi VERSION (槽 1，防火墙拒)", 0 },
		{ 0x20000, "sym_axi VERSION       (槽 2，防火墙拒)", 0 },
		{ 0x30000, "mlkem_axi VERSION     (槽 3，防火墙拒)", 0 },
		{ 0x40000, "金丝雀 VERSION        (槽 4，防火墙拒)", 0 },
	};
	/* ② 译码器判"没有这个地址"的四类，与 axi4lite_xbar 的 hit_of() 一一对应 */
	static const struct site nohit[] = {
		{ 0x10110, "槽内偏移高位非零 0x8001_0110（镜像地址）", 0 },
		{ 0x60000, "槽号越界 0x8006_0000（只有 6 个槽）", 0 },
		{ 0x100004,"aperture 以外 0x8010_0004", 0 },
		{ 0x10005, "未对齐 0x8001_0005", 1 },
	};
	struct sigaction sa;
	uint32_t v, tv, mv, xv;
	int fd, good;

	rep = fopen("/media/sd-mmcblk1p2/hsm/RESULT_nocrash.txt", "w");
	if (!rep) rep = stdout;

	fprintf(rep, "==== 崩不了板：用户态最坏情况 ====\n");
	fprintf(rep, "每个地点读写各 %d 次。判据：程序自己跑完 + 板子还在。\n", ROUNDS);
	fprintf(rep, "改 RAZ/WI 之前，下面第 ① 组的第一笔写就足以让内核 panic。\n");

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_bus;
	sigaction(SIGBUS, &sa, NULL);

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("open /dev/mem"); return 1; }
	pl = mmap(NULL, PL_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PL_BASE);
	if (pl == MAP_FAILED) { perror("mmap"); return 1; }

	/* 对照先行：这条路本身得是通的，否则"什么都没炸"只是因为什么都没发生 */
	fprintf(rep, "\n[对照：先证明总线这条路是活的]\n");
	v = rd_safe(0x50000, &good);
	if (good && v == VERSION_TRUE)
		ok("风扇观测口读到真值 0x%08x —— 事务确实发得出去", v);
	else {
		bad("风扇观测口读到 0x%08x（good=%d）—— 位流不对或路不通，"
		    "下面的\"没炸\"什么都不能证明", v, good);
		goto out;
	}

	hammer("① 防火墙拒的地址：以前第一笔写就 panic",
	       refused, sizeof refused / sizeof refused[0]);
	hammer("② 译码器判\"没有这个地址\"的四类",
	       nohit, sizeof nohit / sizeof nohit[0]);

	/* 留痕：从安全世界读计数器。普通世界读同样的地址只会得到 0 —— 这正是
	 * 我们要的：证据存在，但制造证据的人看不到也擦不掉。 */
	fprintf(rep, "\n[留痕：违规计数只有安全世界读得到]\n");
	v = rd_safe(0x00038, &good);     /* 普通世界读 trng A_VIOL */
	if (good && v == 0)
		ok("普通世界读 trng A_VIOL 得到 0 —— 攻击者看不到自己的痕迹");
	else
		bad("普通世界读 trng A_VIOL 得到 0x%08x（good=%d）", v, good);

	if (sec_open() == 0 &&
	    sec_rd(0x00038, &tv) == 0 &&      /* trng  A_VIOL   {读,写} */
	    sec_rd(0x30024, &mv) == 0 &&      /* mlkem A_VIOL   {读,写} */
	    sec_rd(0x3002C, &xv) == 0) {      /* mlkem A_XBAR_VIOL 译码违规 */
		fprintf(rep, "        trng  防火墙违规  读 %u / 写 %u\n",
			tv >> 16, tv & 0xFFFF);
		fprintf(rep, "        mlkem 防火墙违规  读 %u / 写 %u\n",
			mv >> 16, mv & 0xFFFF);
		/* A_XBAR_VIOL 与各核 A_VIOL 同一布局：{读[31:16], 写[15:0]}。
		 * 第一版只打了低 16 位却把标签写成"译码违规"，看着像总数
		 * 其实只有写的那一半 —— 和前面 SIGBUS 只记总数是同一类错误。 */
		fprintf(rep, "        xbar  译码违规    读 %u / 写 %u（各自 16 位饱和）\n",
			xv >> 16, xv & 0xFFFF);
		if ((tv & 0xFFFF) && (xv & 0xFFFF))
			ok("安全世界读到了非零的违规计数 —— 痕迹留下了");
		else
			bad("安全世界读到的违规计数是 0 —— 打了这么多次却没留痕");
	} else {
		fprintf(rep, "        （/dev/secmmio 不可用，跳过留痕核对；"
			     "崩不了板那部分的结论不受影响）\n");
	}

out:
	fprintf(rep, "\n================================\n");
	fprintf(rep, "通过 %d，失败 %d\n", n_pass, n_fail);
	fprintf(rep, "\n你能读到这一行，本身就是结论的一半："
		     "板子跑完了上面所有操作还活着。\n");
	fflush(rep);
	return n_fail ? 1 : 0;
}
