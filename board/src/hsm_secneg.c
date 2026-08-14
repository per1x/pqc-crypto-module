// hsm_secneg —— 反证：同一批核地址，从普通世界直接读，必须全部被总线拒
//
//   aarch64-linux-gnu-gcc -O2 -static -o hsm_secneg hsm_secneg.c
//
// ============================================================================
// 【它证明的那一半】
// ============================================================================
// 正向那一半由 hsm_kem3 / hsm_hwtest（默认走 /dev/secmmio）跑：SECURE_ONLY=1
// 的全功能核，由安全世界端到端跑完整套 KAT。
//
// 但"安全世界能用"单独不构成边界 —— 还得证明**普通世界不能用**。否则
// "跑通了"也可能只是因为门根本没关。这个程序就是那一半：
//
//   · 对四个功能核（trng / key_vault / sym / mlkem）各读一个 VERSION，
//     **每一次都必须读回 0**（各核真实的 VERSION 都是 0x0001_0000）；
//   · 对照：风扇观测口（槽 5，SECURE_ONLY=0）必须读到**真值** ——
//     否则"全都是 0"可能只是 PL 没配置、或 /dev/mem 这条路本身断了，
//     那样的话上面四条什么都证明不了。
//
// ============================================================================
// 【判据从 "DECERR" 换成了 "读回 0" —— 而且这一条更强】
// ============================================================================
// 防火墙改成 RAZ/WI 之后（理由见 hardware/rtl/bus/axi4lite_firewall.v 文件头：
// DECERR 的 posted 写会以 SError 打穿内核），被拒的读回 OKAY + 数据 0，
// 不再产生总线错误，所以 SIGBUS 那条判据失效了。
//
// 换上来的判据不是退而求其次：
//   · DECERR 只证明"这个地址给不了你东西" —— 地址根本不存在时也是 DECERR，
//     所以它区分不了"门关着"和"后面压根没东西"；
//   · **"读回 0" 同时证明两件事**：事务确实到达了那个从机（路是通的、
//     核确实在那儿），以及它确实被拒了（拿到的不是 0x0001_0000）。
//     一个空地址给不出这个结果。
//
// SIGBUS 处理仍然留着：现在它是**兜底断言**——如果哪一次真的弹了 SIGBUS，
// 说明 RAZ/WI 没生效，那本身就是要报出来的事实。
#define _GNU_SOURCE
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* 五个核的 VERSION 都是这个值。"读到 0" 之所以能当判据，
 * 全靠它非零 —— 哪天有人把某个核的 VERSION 改成 0，这个反证就失效了。 */
#define VERSION_TRUE 0x00010000U

#define PL_BASE   0x80000000UL
#define PL_SPAN   0x00060000UL

static volatile uint8_t *pl;
static sigjmp_buf busjmp;
static volatile int bus_armed, bus_hit;
static int n_pass, n_fail;
static FILE *rep;

static void on_bus(int sig)
{
	(void)sig;
	if (bus_armed) { bus_hit = 1; siglongjmp(busjmp, 1); }
	_exit(135);
}

static uint32_t rd_safe(unsigned off, int *okp)
{
	uint32_t v = 0;

	bus_hit = 0;
	bus_armed = 1;
	if (sigsetjmp(busjmp, 1) == 0)
		v = *(volatile uint32_t *)(pl + off);
	bus_armed = 0;
	if (okp) *okp = !bus_hit;
	return bus_hit ? 0 : v;
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

int main(void)
{
	struct { unsigned off; const char *name; } sec[] = {
		{ 0x00020, "trng_axi VERSION      (槽 0，SECURE_ONLY=1)" },
		{ 0x10000, "key_vault_axi VERSION (槽 1，SECURE_ONLY=1)" },
		{ 0x20000, "sym_axi VERSION       (槽 2，SECURE_ONLY=1)" },
		{ 0x30000, "mlkem_axi VERSION     (槽 3，SECURE_ONLY=1)" },
		{ 0x40000, "金丝雀 VERSION        (槽 4，SECURE_ONLY=1)" },
	};
	size_t i;
	int fd, good;
	uint32_t v;
	struct sigaction sa;

	rep = fopen("/media/sd-mmcblk1p2/hsm/RESULT_secneg.txt", "w");
	if (!rep) rep = stdout;

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_bus;
	sigaction(SIGBUS, &sa, NULL);

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("open /dev/mem"); return 1; }
	pl = mmap(NULL, PL_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PL_BASE);
	if (pl == MAP_FAILED) { perror("mmap"); return 1; }

	fprintf(rep, "==== 反证：普通世界直接读 SECURE_ONLY=1 的核 ====\n");
	fprintf(rep, "判据：被拒的读回 0（RAZ/WI），而各核真实 VERSION = 0x%08x。\n",
		VERSION_TRUE);
	fprintf(rep, "本程序仍然一笔写都不发 —— 边界证明用不着写，"
		     "写侧的\"不崩板\"由 hsm_nocrash 单独证。\n\n");

	fprintf(rep, "[对照：先证明这条路本身是通的]\n");
	v = rd_safe(0x50000, &good);      /* 风扇观测口，SECURE_ONLY=0 */
	if (!good)
		bad("读风扇观测口弹了 SIGBUS —— 总线还在报错，RAZ/WI 没生效");
	else if (v == VERSION_TRUE)
		ok("风扇观测口 (槽 5，SECURE_ONLY=0) 读到真值 0x%08x —— "
		   "/dev/mem 这条路是通的，PL 也在，而且这个值不是 0", v);
	else
		bad("风扇观测口读到 0x%08x，既不是真值也不是被拒 —— "
		    "PL 没配置或位流不对，下面的结果什么都证明不了", v);

	fprintf(rep, "\n[四个功能核 + 金丝雀：每一次都必须读回 0]\n");
	for (i = 0; i < sizeof(sec) / sizeof(sec[0]); i++) {
		v = rd_safe(sec[i].off, &good);
		if (!good)
			bad("%s 弹了 SIGBUS —— 门是关着的，但总线仍在报错，"
			    "RAZ/WI 没生效（这会让任何一次误写都打穿内核）",
			    sec[i].name);
		else if (v == 0)
			ok("%s 读回 0（真值应为 0x%08x）—— 事务到得了，值拿不到",
			   sec[i].name, VERSION_TRUE);
		else
			bad("%s **读到了 0x%08x** —— 门没关上", sec[i].name, v);
	}

	fprintf(rep, "\n================================\n");
	fprintf(rep, "通过 %d，失败 %d\n", n_pass, n_fail);
	fprintf(rep, "\n与正向那一半合起来才是完整命题：\n"
		     "  正向 hsm_kem3 / hsm_hwtest（经 /dev/secmmio 由 EL3 发）跑通全部 KAT；\n"
		     "  反向 本程序：同一批地址从普通世界读，全部读回 0。\n");
	fflush(rep);
	return n_fail ? 1 : 0;
}
