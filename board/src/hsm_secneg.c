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
//     **每一次都必须 DECERR**；
//   · 对照：风扇观测口（槽 5，SECURE_ONLY=0）必须**读得到** ——
//     否则"全都读不到"可能只是 PL 没配置、或 /dev/mem 这条路本身断了，
//     那样的话上面四条什么都证明不了。
//
// ============================================================================
// 【为什么一笔写都不发】
// ============================================================================
// 读的 DECERR 是**同步**外部中止，精确落在那条读指令上 → SIGBUS → siglongjmp
// 回得来。写是 **posted** 的：指令早退休了，错误以 **SError** 回来，
// 不属于任何一条指令，内核只能 panic。而那时密码 bitstream 正载着，
// eth0 的 MAC 在厂家 PL 里根本不存在 —— 网络、harness 恢复、sysrq 看门狗
// 一起没了。这条代价是一次断电，不再交第二次。
//
// **所以这个程序只读。** 写侧的拒绝语义由仿真判。
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
	fprintf(rep, "本程序**一笔写都不发**（写的 DECERR 是 SError，接不住）。\n\n");

	fprintf(rep, "[对照：先证明这条路本身是通的]\n");
	v = rd_safe(0x50000, &good);      /* 风扇观测口，SECURE_ONLY=0 */
	if (good)
		ok("风扇观测口 (槽 5，SECURE_ONLY=0) 读到 0x%08x —— "
		   "/dev/mem 这条路是通的，PL 也在", v);
	else
		bad("连 SECURE_ONLY=0 的风扇口都读不到 —— "
		    "PL 没配置或路径本身断了，下面的结果什么都证明不了");

	fprintf(rep, "\n[四个功能核 + 金丝雀：每一次都必须被拒]\n");
	for (i = 0; i < sizeof(sec) / sizeof(sec[0]); i++) {
		v = rd_safe(sec[i].off, &good);
		if (!good)
			ok("%s 被总线拒（DECERR/SIGBUS）", sec[i].name);
		else
			bad("%s **读到了 0x%08x** —— 门没关上", sec[i].name, v);
	}

	fprintf(rep, "\n================================\n");
	fprintf(rep, "通过 %d，失败 %d\n", n_pass, n_fail);
	fprintf(rep, "\n与正向那一半合起来才是完整命题：\n"
		     "  正向 hsm_kem3 / hsm_hwtest（经 /dev/secmmio 由 EL3 发）跑通全部 KAT；\n"
		     "  反向 本程序：同一批地址从普通世界读，全部被拒。\n");
	fflush(rep);
	return n_fail ? 1 : 0;
}
