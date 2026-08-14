/* accel_axi.c —— 经 AXI 总线驱动仿真出来的加速器
 *
 * 这是三个 transport 里离真板最近的一个：它不直接握手算法核的私有端口，
 * 而是发真正的 AXI4-Lite 读写事务与 AXI4-Stream 数据包，走的是
 * docs/REGISTERS.md 定义的那份契约。换到真板上时，替换的只是
 * "AXI 事务怎么发出去"这一层（仿真 → /dev/mem + mmap 或内核驱动），
 * 本文件的命令时序原样保留。
 *
 * 【与 accel_transport_t 的阻抗匹配】
 * 上层的 write_data / read_data 是"按偏移读写一块缓冲"的语义，而 AXI4-Stream
 * 是成块搬运、结果取一次即失效。两者之间用一对影子缓冲衔接：
 *
 *   write_data  写进输入影子缓冲；CTRL.START 落下时，才把 IN_LEN 个字节
 *               作为一个数据包送进去
 *   read_data   首次调用时把输出流整包取回输出影子缓冲，之后按偏移读它
 *
 * 这与真实系统里 DMA 驱动的做法一致：描述符备好、写寄存器踢一脚、
 * 完成后把结果搬回主存，上层再按偏移取用。
 *
 * 编译时若没有 PQCHSM_HAVE_VERILATOR，accel_transport_axi() 返回 NULL ——
 * 如实反映这条路没编进来，而不是悄悄退回软件。
 */
#include "pqchsm/accel.h"

#ifdef PQCHSM_HAVE_VERILATOR

#include "pqchsm/util.h"

#include <string.h>

/* 由 src/hal/verilator/axi_sim.cpp 提供 */
int      axi_sim_reset(void);
void     axi_sim_write_reg(uint32_t off, uint32_t val);
uint32_t axi_sim_read_reg(uint32_t off);
void     axi_sim_stream_in(const uint8_t *src, size_t n);
size_t   axi_sim_stream_out(uint8_t *dst, size_t max);
uint64_t axi_sim_cycles(void);

static uint8_t  g_in[ACCEL_BUF_MAX];
static uint8_t  g_out[ACCEL_BUF_MAX];
static size_t   g_out_len;
static int      g_out_valid;
static uint64_t g_cmd_start;      /* START 落下时的周期计数 */
static uint64_t g_cmd_cycles;     /* 上一条命令从 START 到 DONE 的周期数 */

static int ax_reset(void)
{
	pqc_secure_zero(g_in, sizeof(g_in));
	pqc_secure_zero(g_out, sizeof(g_out));
	g_out_len = 0;
	g_out_valid = 0;
	g_cmd_start = 0;
	g_cmd_cycles = 0;
	return axi_sim_reset();
}

static void ax_write_data(uint32_t off, const uint8_t *src, size_t n)
{
	if (off < ACCEL_BUF_MAX && n <= ACCEL_BUF_MAX - off) {
		memcpy(g_in + off, src, n);
	}
}

static void ax_read_data(uint32_t off, uint8_t *dst, size_t n)
{
	/* 结果只能从流里取一次，首次调用时整包取回影子缓冲 */
	if (!g_out_valid) {
		uint32_t len = axi_sim_read_reg(ACCEL_REG_OUT_LEN);
		if (len > ACCEL_BUF_MAX) {
			len = ACCEL_BUF_MAX;
		}
		g_out_len = axi_sim_stream_out(g_out, len);
		g_out_valid = 1;
	}
	if (off < g_out_len && n <= g_out_len - off) {
		memcpy(dst, g_out + off, n);
	} else {
		memset(dst, 0, n);
	}
}

static void ax_write_reg(uint32_t off, uint32_t val)
{
	if (off != ACCEL_REG_CTRL) {
		axi_sim_write_reg(off, val);
		return;
	}

	if (val & ACCEL_CTRL_SOFT_RESET) {
		axi_sim_write_reg(ACCEL_REG_CTRL, ACCEL_CTRL_SOFT_RESET);
		g_out_valid = 0;
		g_out_len = 0;
		return;
	}
	if (!(val & ACCEL_CTRL_START)) {
		return;
	}

	/* 先把输入作为一个数据包送进去，再写 START —— 与 DMA 驱动的顺序一致 */
	uint32_t in_len = axi_sim_read_reg(ACCEL_REG_IN_LEN);
	if (in_len > ACCEL_BUF_MAX) {
		in_len = ACCEL_BUF_MAX;
	}
	if (in_len) {
		axi_sim_stream_in(g_in, in_len);
	}
	g_out_valid = 0;
	g_out_len = 0;

	axi_sim_write_reg(ACCEL_REG_CTRL, ACCEL_CTRL_START);
	g_cmd_start = axi_sim_cycles();
}

static uint32_t ax_read_reg(uint32_t off)
{
	uint32_t val = axi_sim_read_reg(off);
	if (off == ACCEL_REG_STATUS && (val & ACCEL_ST_DONE)) {
		/* 命令耗时按"START 之后到首次看到 DONE"计。这是软件视角的时延，
		 * 包含轮询本身占用的总线周期，比核内部的运算周期数更接近真实开销。 */
		g_cmd_cycles = axi_sim_cycles() - g_cmd_start;
	}
	return val;
}

static const accel_transport_t g_axi = {
	.name        = "axi(RTL pqc_accel_axi; AXI4-Lite + AXI4-Stream)",
	.is_hardware = 0,          /* 仿真不是硬件 */
	.reset       = ax_reset,
	.write_reg   = ax_write_reg,
	.read_reg    = ax_read_reg,
	.write_data  = ax_write_data,
	.read_data   = ax_read_data,
};

const accel_transport_t *accel_transport_axi(void)
{
	return &g_axi;
}

uint64_t accel_axi_last_cycles(void)
{
	return g_cmd_cycles;
}

#else

const accel_transport_t *accel_transport_axi(void)
{
	return NULL;
}

uint64_t accel_axi_last_cycles(void)
{
	return 0;
}

#endif
