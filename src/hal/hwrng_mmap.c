/* hwrng_mmap.c —— 经 /dev/mem + mmap 访问 PL 里的 trng_axi
 *
 * 【状态】这条 transport 未在硬件上跑过 —— 但 PL 里的 trng_axi 跑过：
 * 板上取过 512 KiB 原始熵做 SP 800-90B 评估（见 docs/SECURITY.md），
 * 用的是 board/src/trngraw.c 那条直接 mmap 的路。这里差的是把两端接上。
 *
 * 启用它需要一个由地址分配决定的数值：
 *
 *   PQCHSM_HWRNG_MMAP_BASE   TRNG 寄存器组的物理基址
 *
 * 不写死任何取值：未定义时 hwrng_transport_mmap() 返回 NULL，如实反映
 * "这条路没有可用的目标"。构建时传入，例如
 *
 *   cmake -S . -B build -DCMAKE_C_FLAGS=-DPQCHSM_HWRNG_MMAP_BASE=0x43C20000
 *
 * 【安全世界才是它最终该待的地方】
 * 走 /dev/mem 意味着这条路跑在普通世界的 Linux 里。trng_axi 默认
 * SECURE_ONLY=1，AxPROT[1]=1 的访问一律 DECERR —— 也就是说**这个 transport
 * 在默认配置下打不通**，读回来的是总线错误。这不是 bug，是设计如此。
 *
 * 它有两个正当用途：
 *   · 综合时把 SECURE_ONLY 设成 0 的调试位流上，用来验通路和做熵采集；
 *   · 作为 OP-TEE TA 侧驱动的原型 —— TA 里换成安全世界的映射即可，
 *     寄存器时序完全一样。
 * 生产配置下，取熵的路径必须是安全世界，普通世界连地址都不该看见。
 *
 * 【必须的编译屏障】
 * 所有寄存器访问都经 volatile 指针。RDATA 是**读时弹出**的，编译器若把两次
 * 读合并成一次，或者把"读 STATUS → 读 RDATA"的顺序调换，语义就全毁了。
 */
#include "pqchsm/hwrng.h"

#if defined(__linux__) && defined(PQCHSM_HWRNG_MMAP_BASE)

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#define HWRNG_REG_SPAN 0x1000u   /* 寄存器组一页 */

static volatile uint32_t *g_regs;
static int                g_fd = -1;

static int mm_map_once(void)
{
	if (g_regs) {
		return 0;
	}
	g_fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (g_fd < 0) {
		return -1;
	}
	void *r = mmap(NULL, HWRNG_REG_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED,
	               g_fd, (off_t)PQCHSM_HWRNG_MMAP_BASE);
	if (r == MAP_FAILED) {
		close(g_fd);
		g_fd = -1;
		return -1;
	}
	g_regs = (volatile uint32_t *)r;
	return 0;
}

static void mm_write_reg(uint32_t off, uint32_t val)
{
	if (mm_map_once() == 0 && off < HWRNG_REG_SPAN) {
		g_regs[off / 4] = val;
	}
}

static uint32_t mm_read_reg(uint32_t off)
{
	if (mm_map_once() != 0 || off >= HWRNG_REG_SPAN) {
		/* 映射不上时返回 0：STATUS 读成 0 意味着 READY=0，
		 * 驱动会等到超时后报错 —— 正是想要的结果。绝不能让它
		 * 看起来像"设备就绪且有数据"。 */
		return 0;
	}
	return g_regs[off / 4];
}

static const hwrng_transport_t g_mmap = {
	.name        = "mmap(/dev/mem)",
	.is_hardware = 1,
	.write_reg   = mm_write_reg,
	.read_reg    = mm_read_reg,
};

const hwrng_transport_t *hwrng_transport_mmap(void)
{
	return &g_mmap;
}

#else

const hwrng_transport_t *hwrng_transport_mmap(void)
{
	return NULL;
}

#endif
