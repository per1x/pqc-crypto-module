/* accel_mmap.c —— 经 /dev/mem + mmap 驱动真实 PL 的 transport
 *
 * 【状态】这条 transport 本身尚未对着真实可编程逻辑驱动过。板上的硬件密码引擎目前
 * 由 board/ 下的独立程序驱动（它们 mmap /dev/mem 到 0x8000_0000 直接操作各核），
 * ML-KEM 的 ACVP 结果与边界证明都来自那里。把这条 transport 接上去是另一件事。
 *
 * 启用它需要两个由器件地址分配决定的数值：
 *
 *   PQCHSM_ACCEL_MMAP_BASE   寄存器组的物理基址
 *   PQCHSM_ACCEL_MMAP_BUF    数据缓冲窗口的物理基址
 *
 * 这两个地址由具体器件的地址分配决定，因此这里不写死任何取值。
 * 本项目的目标器件 XCZU3EG 上，PS 的 M_AXI_HPM0_LPD 窗口是 0x8000_0000-0x9FFF_FFFF，
 * 各密码从机按 64 KB 分槽落在其中（见 docs/密码机原型-说明文档.md 的地址映射）。未定义时
 * accel_transport_mmap() 返回 NULL，如实反映"这条路没有可用的目标"。
 * 定义方式是构建时传入，例如
 *
 *   cmake -S . -B build -DCMAKE_C_FLAGS="-DPQCHSM_ACCEL_MMAP_BASE=0x80030000 \
 *                                        -DPQCHSM_ACCEL_MMAP_BUF=0x80030010"
 *
 * 【与其它 transport 的关系】
 * 命令时序与 accel_axi.c 完全相同，遵循同一份 docs/register-map.md 契约：
 * 写 MODE / IN_LEN → 送数据 → 写 CTRL.START → 轮询 STATUS.DONE → 取结果。
 * 区别只在事务怎么发出去：仿真里是驱动 Verilator 模型的端口，这里是对
 * mmap 出来的地址做 volatile 读写。
 *
 * 【数据面的取舍】
 * 这里把数据缓冲当成一段可直接寻址的窗口（PL 侧是一块挂在 AXI 上的 BRAM）。
 * 真实系统若改用 AXI-DMA 搬运，替换的是本文件的 write_data / read_data，
 * 寄存器部分不变。
 *
 * 【必须的编译屏障】
 * 所有寄存器访问都经 volatile 指针，防止编译器合并或重排两次寄存器读 ——
 * "写 START 之后轮询 STATUS"这类序列一旦被优化掉就会静默失效。
 */
#include "pqchsm/accel.h"

#if defined(__linux__) && defined(PQCHSM_ACCEL_MMAP_BASE) && defined(PQCHSM_ACCEL_MMAP_BUF)

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MMAP_REG_SPAN 0x1000u                 /* 寄存器组一页 */
#define MMAP_BUF_SPAN ACCEL_BUF_MAX

static volatile uint32_t *g_regs;
static volatile uint8_t  *g_buf;
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
	void *r = mmap(NULL, MMAP_REG_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED,
	               g_fd, (off_t)PQCHSM_ACCEL_MMAP_BASE);
	void *b = mmap(NULL, MMAP_BUF_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED,
	               g_fd, (off_t)PQCHSM_ACCEL_MMAP_BUF);
	if (r == MAP_FAILED || b == MAP_FAILED) {
		if (r != MAP_FAILED) {
			munmap(r, MMAP_REG_SPAN);
		}
		if (b != MAP_FAILED) {
			munmap(b, MMAP_BUF_SPAN);
		}
		close(g_fd);
		g_fd = -1;
		return -1;
	}
	g_regs = (volatile uint32_t *)r;
	g_buf  = (volatile uint8_t *)b;
	return 0;
}

static int mm_reset(void)
{
	if (mm_map_once() != 0) {
		return -1;
	}
	g_regs[ACCEL_REG_CTRL / 4] = ACCEL_CTRL_SOFT_RESET;
	return 0;
}

static void mm_write_reg(uint32_t off, uint32_t val)
{
	if (mm_map_once() == 0 && off < MMAP_REG_SPAN) {
		g_regs[off / 4] = val;
	}
}

static uint32_t mm_read_reg(uint32_t off)
{
	if (mm_map_once() != 0 || off >= MMAP_REG_SPAN) {
		return 0;
	}
	return g_regs[off / 4];
}

static void mm_write_data(uint32_t off, const uint8_t *src, size_t n)
{
	if (mm_map_once() != 0 || off >= MMAP_BUF_SPAN || n > MMAP_BUF_SPAN - off) {
		return;
	}
	for (size_t i = 0; i < n; i++) {
		g_buf[off + i] = src[i];
	}
}

static void mm_read_data(uint32_t off, uint8_t *dst, size_t n)
{
	if (mm_map_once() != 0 || off >= MMAP_BUF_SPAN || n > MMAP_BUF_SPAN - off) {
		memset(dst, 0, n);
		return;
	}
	for (size_t i = 0; i < n; i++) {
		dst[i] = g_buf[off + i];
	}
}

static const accel_transport_t g_mmap = {
	.name        = "mmap(/dev/mem)",
	.is_hardware = 1,
	.reset       = mm_reset,
	.write_reg   = mm_write_reg,
	.read_reg    = mm_read_reg,
	.write_data  = mm_write_data,
	.read_data   = mm_read_data,
};

const accel_transport_t *accel_transport_mmap(void)
{
	return &g_mmap;
}

#else

const accel_transport_t *accel_transport_mmap(void)
{
	/* 没有目标地址（或不在 Linux 上）时如实返回 NULL */
	return NULL;
}

#endif
