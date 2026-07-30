/* accel_zynq.c —— Zynq-7000 上的真实 MMIO 后端
 *
 * 命令时序与 docs/register-map.md 完全一致，也与仿真用的 accel_axi.c 一致：
 * 配 MODE/IN_LEN → 把输入搬进 PL → 写 CTRL.START → 轮询 STATUS.DONE → 把结果搬回。
 * 区别只在事务怎么发出去：这里是对 mmap 出来的地址做 volatile 读写，
 * 数据由 AXI-DMA 在 DDR 与 PL 之间搬。
 *
 * 【为什么分成"映射"与"时序"两层】
 * 映射方式有三种（UIO、/dev/mem、测试注入），而寄存器时序只有一套。把映射抽成
 * 一个函数指针之后，时序这一层就能在**没有板子**的情况下对着硬件模型验证 ——
 * 见 board/xc7z020/tests/test_accel_zynq.c。板上真正剩下的未知只有"地址对不对、
 * 时钟通不通"，那两件事无论如何都要上板才知道。
 *
 * 【Cache 一致性】PS7 的 HP 口与 Cortex-A9 的 L1/L2 **不保证一致**。DMA 缓冲
 * 必须是非缓存映射，否则 CPU 写进去的输入还在 cache 里、DMA 读到的是旧数据，
 * 而且这种错误是间歇性的。这里用 O_SYNC 打开 /dev/mem —— 在 ARM Linux 上
 * 它决定了映射的内存属性为 device/uncached。PYNQ 那条路径用 pynq.allocate，
 * 拿到的是 dma-coherent 缓冲，同样不走 cache。
 *
 * 【AXI-DMA 用 simple mode】不开 scatter-gather：一次命令就是一段连续缓冲，
 * 描述符链带来的复杂度换不到任何东西。S2MM 用缓冲上限武装，传输由 TLAST 结束，
 * 完成后从 S2MM_LENGTH 读回实际字节数 —— 因此驱动不需要知道每个操作码的
 * 输出长度，操作码增减时这里不用改。
 */
#include "pqc_accel_zynq.h"

#include "pqchsm/util.h"

#include <stdio.h>
#include <string.h>

#if defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

/* 轮询上限。100 MHz 下最长的命令（NTT）约 1300 周期，即 13 µs；
 * 这里给出四个数量级的余量，超过就认定是硬件没响应而不是还在算。 */
#define POLL_LIMIT 2000000

/* ---- 状态 ---- */

static pqc_zynq_config_t g_cfg;
static volatile uint32_t *g_accel;      /* 加速器寄存器组 */
static volatile uint32_t *g_dma;        /* AXI-DMA 寄存器组 */
static volatile uint8_t  *g_buf;        /* DMA 缓冲 */
static uint32_t           g_buf_half;   /* 输入占前半，输出占后半 */
static int                g_open;
static uint64_t           g_poll_count;
static uint32_t           g_in_len;     /* 软件侧攒的输入长度 */
static uint32_t           g_out_len;    /* DMA 实际收回的字节数 */
static int                g_dma_failed; /* 本次命令的搬运没有完成 */

void pqc_zynq_default_config(pqc_zynq_config_t *cfg)
{
	if (!cfg) {
		return;
	}
	memset(cfg, 0, sizeof(*cfg));
	cfg->mode       = PQC_ZYNQ_MAP_AUTO;
	cfg->accel_base = PQC_ZYNQ_ACCEL_BASE;
	cfg->dma_base   = PQC_ZYNQ_DMA_BASE;
	cfg->buf_phys   = PQC_ZYNQ_DMA_BUF_PHYS;
	cfg->buf_span   = PQC_ZYNQ_DMA_BUF_SPAN;
}

/* ---- 寄存器访问 ---- */
/* 一律经 volatile：否则编译器会把"写 START 再轮询 STATUS"里的重复读合并掉，
 * 循环变成读一次然后无限比较同一个值。 */

static void wr(volatile uint32_t *base, uint32_t off, uint32_t val)
{
	base[off / 4] = val;
}

static uint32_t rd(volatile uint32_t *base, uint32_t off)
{
	return base[off / 4];
}

/* ---- 映射 ---- */

#if defined(__linux__)

static int g_mem_fd = -1;

/* UIO：设备树把 PL 的地址段绑给 generic-uio 之后，内核在 /sys/class/uio/uioN/maps
 * 下给出每段的物理地址。按物理地址反查设备号，比按名字匹配稳 —— 名字取决于
 * 设备树里怎么写，地址是硬件事实。 */
static int uio_find_by_phys(uint32_t phys, char *dev, size_t dev_len)
{
	DIR *d = opendir("/sys/class/uio");
	if (!d) {
		return -1;
	}
	struct dirent *e;
	int found = -1;
	while ((e = readdir(d)) != NULL) {
		if (strncmp(e->d_name, "uio", 3) != 0) {
			continue;
		}
		/* d_name 在 Linux 上最长 255 字节，加上固定前缀后可能超过一个 256 字节的
		 * 缓冲。缓冲按最坏情况开够，编译器就不必再警告可能截断 —— 截断在这里
		 * 不是良性的：路径被截掉尾部会打开到另一个文件上。 */
		char path[320];
		int n = snprintf(path, sizeof(path),
		                 "/sys/class/uio/%s/maps/map0/addr", e->d_name);
		if (n < 0 || (size_t)n >= sizeof(path)) {
			continue;
		}
		FILE *f = fopen(path, "r");
		if (!f) {
			continue;
		}
		/* 用 unsigned long long 而不是 unsigned long 去接：long 的宽度随 ABI 变
		 * （目标板 armv7l 上 4 字节，开发机 aarch64 上 8 字节），同一段 sysfs
		 * 文本在两边的溢出行为会不一样。固定成 64 位再回落到 uint32_t 比较，
		 * 开 LPAE 的内核给出超过 4 GiB 的地址时也只是匹配不上，不会读出错值。 */
		unsigned long long addr = 0;
		int ok = (fscanf(f, "%llx", &addr) == 1);
		fclose(f);
		if (ok && addr == (unsigned long long)phys) {
			snprintf(dev, dev_len, "/dev/%s", e->d_name);
			found = 0;
			break;
		}
	}
	closedir(d);
	return found;
}

static void *map_uio(uint32_t phys, uint32_t span)
{
	char dev[64];
	if (uio_find_by_phys(phys, dev, sizeof(dev)) != 0) {
		return NULL;
	}
	int fd = open(dev, O_RDWR | O_SYNC);
	if (fd < 0) {
		return NULL;
	}
	void *p = mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	return (p == MAP_FAILED) ? NULL : p;
}

/* /dev/mem：O_SYNC 决定映射为非缓存，这一点对 DMA 缓冲是必须的 */
static void *map_devmem(uint32_t phys, uint32_t span)
{
	if (g_mem_fd < 0) {
		g_mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
		if (g_mem_fd < 0) {
			return NULL;
		}
	}
	void *p = mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_SHARED,
	               g_mem_fd, (off_t)phys);
	return (p == MAP_FAILED) ? NULL : p;
}

#endif /* __linux__ */

static void *map_region(uint32_t phys, uint32_t span)
{
	if (g_cfg.mode == PQC_ZYNQ_MAP_CUSTOM) {
		return g_cfg.mapper ? g_cfg.mapper(phys, span, g_cfg.user) : NULL;
	}
#if defined(__linux__)
	void *p = NULL;
	if (g_cfg.mode == PQC_ZYNQ_MAP_UIO || g_cfg.mode == PQC_ZYNQ_MAP_AUTO) {
		p = map_uio(phys, span);
	}
	if (!p && (g_cfg.mode == PQC_ZYNQ_MAP_DEVMEM || g_cfg.mode == PQC_ZYNQ_MAP_AUTO)) {
		p = map_devmem(phys, span);
	}
	return p;
#else
	/* 非 Linux 平台上只有注入映射可用。如实返回 NULL，不假装打开成功。 */
	(void)phys;
	(void)span;
	return NULL;
#endif
}

static void unmap_region(void *addr, uint32_t span)
{
	if (!addr) {
		return;
	}
	if (g_cfg.mode == PQC_ZYNQ_MAP_CUSTOM) {
		if (g_cfg.unmapper) {
			g_cfg.unmapper(addr, span, g_cfg.user);
		}
		return;
	}
#if defined(__linux__)
	munmap(addr, span);
#endif
}

/* ---- DMA ---- */

static int dma_reset(void)
{
	wr(g_dma, DMA_MM2S_DMACR, DMA_CR_RESET);
	wr(g_dma, DMA_S2MM_DMACR, DMA_CR_RESET);
	for (int i = 0; i < POLL_LIMIT; i++) {
		if (!(rd(g_dma, DMA_MM2S_DMACR) & DMA_CR_RESET)
		    && !(rd(g_dma, DMA_S2MM_DMACR) & DMA_CR_RESET)) {
			return 0;
		}
	}
	return -1;
}

/* 等一次传输真正结束。
 *
 * 判据取 IOC（传输完成）而不是 IDLE：IDLE 在 DMA 处于停止状态时也是 1，
 * 刚写完 LENGTH、引擎还没起来的那几拍里去查 IDLE 会立刻通过，
 * 于是驱动以为搬完了、实际一个字节都没动。IOC 只在真的完成一次传输后置位，
 * 而且要由软件写 1 清除，不存在这个歧义。 */
static int dma_wait_ioc(uint32_t sr_off)
{
	for (int i = 0; i < POLL_LIMIT; i++) {
		uint32_t sr = rd(g_dma, sr_off);
		if (sr & DMA_SR_ERR_MASK) {
			return -1;
		}
		if (sr & DMA_SR_IOC) {
			wr(g_dma, sr_off, DMA_SR_IOC);   /* 写 1 清除，为下一次留干净状态 */
			return 0;
		}
	}
	return -1;
}

/* ---- transport ---- */

static int zy_reset(void)
{
	if (!g_open) {
		return -1;
	}
	g_in_len = 0;
	g_out_len = 0;
	g_dma_failed = 0;
	wr(g_accel, ACCEL_REG_CTRL, ACCEL_CTRL_SOFT_RESET);
	return dma_reset();
}

static void zy_write_data(uint32_t off, const uint8_t *src, size_t n)
{
	if (!g_open || off >= g_buf_half || n > g_buf_half - off) {
		return;
	}
	for (size_t i = 0; i < n; i++) {
		g_buf[off + i] = src[i];
	}
	if (off + n > g_in_len) {
		g_in_len = (uint32_t)(off + n);
	}
}

static void zy_read_data(uint32_t off, uint8_t *dst, size_t n)
{
	if (!g_open || off >= g_out_len || n > g_out_len - off) {
		memset(dst, 0, n);
		return;
	}
	for (size_t i = 0; i < n; i++) {
		dst[i] = g_buf[g_buf_half + off + i];
	}
}

static void zy_write_reg(uint32_t off, uint32_t val)
{
	if (!g_open) {
		return;
	}
	if (off != ACCEL_REG_CTRL) {
		wr(g_accel, off, val);
		return;
	}

	if (val & ACCEL_CTRL_SOFT_RESET) {
		zy_reset();
		return;
	}
	if (!(val & ACCEL_CTRL_START)) {
		return;
	}

	uint32_t in_len = rd(g_accel, ACCEL_REG_IN_LEN);
	if (in_len > g_buf_half) {
		in_len = g_buf_half;
	}
	g_out_len = 0;
	g_dma_failed = 0;

	/* 先武装 S2MM（接收），再启动 MM2S（发送）：反过来的话，加速器可能在接收
	 * 通道就绪之前就把结果推出来，那些拍会因为 TREADY 为低而堵在 PL 里。 */
	wr(g_dma, DMA_S2MM_DMASR, DMA_SR_IOC);      /* 清掉上一次的完成标志 */
	wr(g_dma, DMA_S2MM_DMACR, DMA_CR_RS | DMA_CR_IOC_EN);
	wr(g_dma, DMA_S2MM_DA, g_cfg.buf_phys + g_buf_half);
	wr(g_dma, DMA_S2MM_LENGTH, g_buf_half);

	if (in_len) {
		wr(g_dma, DMA_MM2S_DMASR, DMA_SR_IOC);
		wr(g_dma, DMA_MM2S_DMACR, DMA_CR_RS | DMA_CR_IOC_EN);
		wr(g_dma, DMA_MM2S_SA, g_cfg.buf_phys);
		wr(g_dma, DMA_MM2S_LENGTH, in_len);
		/* 输入必须整包进到 PL 之后才能触发运算：加速器是"包收齐、写 START、
		 * 再开算"的语义，START 早于数据到齐会算到上一次的残留。 */
		if (dma_wait_ioc(DMA_MM2S_DMASR) != 0) {
			/* 输入没搬进去就不能触发运算，也不能让上层以为命令发出去了 */
			g_dma_failed = 1;
			return;
		}
	}

	wr(g_accel, ACCEL_REG_CTRL, ACCEL_CTRL_START);
}

static uint32_t zy_read_reg(uint32_t off)
{
	if (!g_open) {
		return 0;
	}
	uint32_t val = rd(g_accel, off);

	if (off == ACCEL_REG_STATUS) {
		g_poll_count++;
		if ((val & ACCEL_ST_DONE) && !(val & ACCEL_ST_ERR)
		    && g_out_len == 0 && !g_dma_failed) {
			/* 结果由 DMA 搬回 DDR。实际字节数从 S2MM_LENGTH 读回，
			 * 因此这里不需要知道操作码对应多长的输出。 */
			if (dma_wait_ioc(DMA_S2MM_DMASR) == 0) {
				g_out_len = rd(g_dma, DMA_S2MM_LENGTH);
			} else {
				g_dma_failed = 1;
			}
		}
		/* 【必须往上报】加速器自己算完了，但结果没搬回来 —— 从加速器寄存器看
		 * 这次命令是成功的，缓冲里却是上一次的残留或全零。不把它变成 ERR，
		 * 上层会拿着一段无意义的数据继续走下去。 */
		if (g_dma_failed) {
			val |= ACCEL_ST_ERR;
		}
	}

	if (off == ACCEL_REG_OUT_LEN) {
		/* 以 DMA 实际收回的字节数为准，并与加速器自报的长度交叉核对：
		 * 两者不一致同样说明搬运出了问题，不能按自报长度去取数据。 */
		if (g_dma_failed || g_out_len != val) {
			g_dma_failed = 1;
			return 0;
		}
		return g_out_len;
	}
	return val;
}

static const accel_transport_t g_zynq = {
	.name        = "zynq(mmio: AXI4-Lite + AXI-DMA)",
	.is_hardware = 1,
	.reset       = zy_reset,
	.write_reg   = zy_write_reg,
	.read_reg    = zy_read_reg,
	.write_data  = zy_write_data,
	.read_data   = zy_read_data,
};

/* ---- 打开与关闭 ---- */

int pqc_zynq_open(const pqc_zynq_config_t *cfg)
{
	if (g_open) {
		return 0;
	}
	if (!cfg) {
		pqc_zynq_default_config(&g_cfg);
	} else {
		g_cfg = *cfg;
	}
	if (g_cfg.buf_span < 8 || (g_cfg.buf_span & 1u)) {
		return -1;
	}

	g_accel = map_region(g_cfg.accel_base, PQC_ZYNQ_ACCEL_SPAN);
	if (!g_accel) {
		return -1;
	}
	g_dma = map_region(g_cfg.dma_base, PQC_ZYNQ_DMA_SPAN);
	if (!g_dma) {
		unmap_region((void *)g_accel, PQC_ZYNQ_ACCEL_SPAN);
		g_accel = NULL;
		return -1;
	}
	g_buf = map_region(g_cfg.buf_phys, g_cfg.buf_span);
	if (!g_buf) {
		unmap_region((void *)g_dma, PQC_ZYNQ_DMA_SPAN);
		unmap_region((void *)g_accel, PQC_ZYNQ_ACCEL_SPAN);
		g_dma = NULL;
		g_accel = NULL;
		return -1;
	}

	g_buf_half   = g_cfg.buf_span / 2;
	g_poll_count = 0;
	g_in_len     = 0;
	g_out_len    = 0;
	g_open       = 1;

	if (dma_reset() != 0) {
		pqc_zynq_close();
		return -1;
	}
	return 0;
}

void pqc_zynq_close(void)
{
	if (g_buf) {
		unmap_region((void *)g_buf, g_cfg.buf_span);
		g_buf = NULL;
	}
	if (g_dma) {
		unmap_region((void *)g_dma, PQC_ZYNQ_DMA_SPAN);
		g_dma = NULL;
	}
	if (g_accel) {
		unmap_region((void *)g_accel, PQC_ZYNQ_ACCEL_SPAN);
		g_accel = NULL;
	}
#if defined(__linux__)
	if (g_mem_fd >= 0) {
		close(g_mem_fd);
		g_mem_fd = -1;
	}
#endif
	g_open = 0;
}

const accel_transport_t *accel_transport_zynq(void)
{
	return g_open ? &g_zynq : NULL;
}

uint64_t pqc_zynq_last_poll_count(void)
{
	return g_poll_count;
}
