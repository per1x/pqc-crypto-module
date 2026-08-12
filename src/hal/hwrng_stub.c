/* hwrng_stub.c —— TRNG 的软件模型 transport
 *
 * 逐位复现 hardware/rtl/trng/trng_axi.v 的**寄存器语义**：上电暖机、
 * READY/DATA_VALID 的时序关系、RDATA 读时弹出、空读返回 0 并锁存 UNDERRUN、
 * ALARM 锁存、ZEROIZE 清池并重跑启动检测、PARAM0/1/2 回读。
 * FIFO 内容由 OpenSSL 填 —— 这里模拟的是**接口**，不是熵。
 *
 * 【模型的边界：语义复现，时间不复现】
 * 真器件暖机要 STARTUP_SAMPLES×DECIM = 8192 个时钟，每个 rate 块要 1088 个
 * 样本。软件模型里没有时钟，所以拿"软件读了几次 STATUS"当时间基准：每读一次
 * STATUS 推进 64 个样本，于是暖机需要软件轮询 16 次。
 *
 * 这个压缩是有意的，也必须说清楚它证明了什么、没证明什么：
 *   · 证明了：驱动确实在轮询、确实等到 READY 才读、暖机期间取不到数；
 *   · 没证明：任何与真实时延、吞吐、熵率有关的东西。
 * 上层不该从这个模型得出任何性能或熵的结论。
 *
 * 【没有模拟 AxPROT】
 * 门控发生在 AXI 总线上，软件模型没有总线，也就没有 AxPROT 可判。
 * 那一层由 hardware/tb/cocotb/test_trng_axi.py 在 RTL 侧验证 —— 那才是它
 * 真正生效的地方。在这里造一个假的门控只会给人"软件也测了"的错觉。
 */
#include "pqchsm/hwrng.h"
#include "hwrng_stub.h"

#include <openssl/rand.h>
#include <stdlib.h>
#include <string.h>

#include "pqchsm/util.h"

#define STUB_FIFO_DEPTH   16
/* 每吸收一个 rate 块挤出 OUT_LANES×64 = 256 bit = 8 个 32 位字 */
#define STUB_WORDS_PER_BLK 8
/* 每读一次 STATUS 推进多少个启动样本，见文件头 */
#define STUB_SAMPLES_PER_POLL 64

static struct {
	int      enable;
	uint32_t startup_count;
	int      startup_done;
	uint32_t words_out;
	uint32_t blocks;
	int      underrun;
	int      alarm;
	int      starve;
	unsigned alarm_after;   /* 非 0：再弹这么多个字之后锁存 ALARM */

	uint32_t fifo[STUB_FIFO_DEPTH];
	int      fifo_len;
} g;

static int g_inited;

static void stub_power_on(void)
{
	pqc_secure_zero(&g, sizeof(g));
	g.enable  = 1;   /* 与 RTL 一致：reg_enable 复位为 1，上电即暖机 */
	g_inited  = 1;
}

/* ZEROIZE / tamper：擦 FIFO（真擦，不是只挪指针）、清计数、重跑启动检测 */
static void stub_zeroize(void)
{
	pqc_secure_zero(g.fifo, sizeof(g.fifo));
	g.fifo_len      = 0;
	g.words_out     = 0;
	g.blocks        = 0;
	g.startup_count = 0;
	g.startup_done  = 0;
	g.underrun      = 0;
}

/* 自由运行的器件在背景里持续产出。软件模型没有背景，所以把"时间流逝"
 * 挂在读 STATUS 上 —— 驱动本来就必须先读 STATUS 才能读 RDATA。 */
static void stub_tick(void)
{
	if (!g.enable || g.alarm) {
		return;
	}
	if (!g.startup_done) {
		g.startup_count += STUB_SAMPLES_PER_POLL;
		if (g.startup_count >= HWRNG_EXPECT_STARTUP_SAMPLES) {
			g.startup_count = HWRNG_EXPECT_STARTUP_SAMPLES;
			g.startup_done  = 1;
		}
		return;   /* 暖机期间一个字也不产出 */
	}
	if (g.starve || g.fifo_len > STUB_FIFO_DEPTH - STUB_WORDS_PER_BLK) {
		return;
	}
	if (RAND_bytes((uint8_t *)&g.fifo[g.fifo_len],
	               STUB_WORDS_PER_BLK * (int)sizeof(uint32_t)) != 1) {
		/* 拿不到随机数是不可恢复的：返回可预测数据比崩溃危险得多 */
		abort();
	}
	g.fifo_len += STUB_WORDS_PER_BLK;
	g.blocks++;
}

static uint32_t stub_status(void)
{
	uint32_t s = 0;
	if (g.startup_done && !g.alarm) {
		s |= HWRNG_ST_READY;
	}
	if (g.fifo_len > 0) {
		s |= HWRNG_ST_DATA_VALID;
	}
	if (g.alarm) {
		s |= HWRNG_ST_ALARM | HWRNG_ST_RCT_ALARM;
	}
	if (g.startup_done) {
		s |= HWRNG_ST_STARTUP_DONE;
	}
	if (g.enable) {
		s |= HWRNG_ST_ENABLED;
	}
	if (g.underrun) {
		s |= HWRNG_ST_UNDERRUN;
	}
	return s;
}

static uint32_t stub_pop(void)
{
	if (g.fifo_len == 0 || g.alarm) {
		/* 空读返回 0 并锁存 UNDERRUN —— 与 RTL 完全一致。
		 * 返回 0 是危险的，UNDERRUN 就是用来事后抓住"没查状态就读"的。 */
		g.underrun = 1;
		return 0;
	}
	uint32_t v = g.fifo[0];
	memmove(&g.fifo[0], &g.fifo[1], (size_t)(g.fifo_len - 1) * sizeof(uint32_t));
	g.fifo_len--;
	g.fifo[g.fifo_len] = 0;   /* 弹出的槽位立刻清掉，别留残留 */
	g.words_out++;

	/* 定时告警：这个字已经交给软件了，告警在它之后才锁存。
	 * 与真器件一致 —— 告警不会把已经读走的字追回来，所以驱动必须靠
	 * 取完之后的复查来判定整批作废。 */
	if (g.alarm_after > 0 && --g.alarm_after == 0) {
		g.alarm = 1;
		pqc_secure_zero(g.fifo, sizeof(g.fifo));
		g.fifo_len = 0;
	}
	return v;
}

static void stub_write_reg(uint32_t off, uint32_t val)
{
	if (!g_inited) {
		stub_power_on();
	}
	if (off != HWRNG_REG_CTRL) {
		return;   /* 其余全是只读，写入无副作用 */
	}
	g.enable = (val & HWRNG_CTRL_ENABLE) ? 1 : 0;
	if (val & HWRNG_CTRL_ZEROIZE) {
		stub_zeroize();
	}
	if (val & HWRNG_CTRL_CLEAR_ALARM) {
		g.alarm    = 0;
		g.underrun = 0;
		/* 清告警不是"清了就能接着用"：启动健康检测要重跑 */
		g.startup_count = 0;
		g.startup_done  = 0;
		pqc_secure_zero(g.fifo, sizeof(g.fifo));
		g.fifo_len = 0;
	}
}

static uint32_t stub_read_reg(uint32_t off)
{
	if (!g_inited) {
		stub_power_on();
	}
	switch (off) {
	case HWRNG_REG_CTRL:
		return (uint32_t)g.enable;
	case HWRNG_REG_STATUS:
		stub_tick();
		return stub_status();
	case HWRNG_REG_RDATA:
		return stub_pop();
	case HWRNG_REG_HEALTH:
		return 0;
	case HWRNG_REG_APT_INDEX:
		return 0;
	case HWRNG_REG_STARTUP:
		return g.startup_count;
	case HWRNG_REG_BLOCKS:
		return g.blocks;
	case HWRNG_REG_WORDS:
		return g.words_out;
	case HWRNG_REG_VERSION:
		return HWRNG_VERSION_EXPECTED;
	case HWRNG_REG_PARAM0:
		return (HWRNG_EXPECT_DECIM << 24) | (HWRNG_EXPECT_NUM_RO << 16) |
		       (HWRNG_EXPECT_RATE_LANES << 8) | HWRNG_EXPECT_OUT_LANES;
	case HWRNG_REG_PARAM1:
		return (HWRNG_EXPECT_APT_CUTOFF << 16) | HWRNG_EXPECT_RCT_CUTOFF;
	case HWRNG_REG_PARAM2:
		return (HWRNG_EXPECT_STARTUP_SAMPLES << 16) | HWRNG_EXPECT_APT_WINDOW;
	default:
		return 0;
	}
}

static const hwrng_transport_t g_stub = {
	.name        = "stub(软件模型)",
	.is_hardware = 0,
	.write_reg   = stub_write_reg,
	.read_reg    = stub_read_reg,
};

const hwrng_transport_t *hwrng_transport_stub(void)
{
	return &g_stub;
}

/* ---- 故障注入（只给测试） ---- */

void hwrng_stub_force_alarm(int on)
{
	if (!g_inited) {
		stub_power_on();
	}
	g.alarm = on ? 1 : 0;
	if (on) {
		/* 告警随即清池：与 trng_top 一致 —— 噪声源可能已经坏了一段时间，
		 * 池子里可能混了低熵输入，留着比丢掉风险大。 */
		pqc_secure_zero(g.fifo, sizeof(g.fifo));
		g.fifo_len = 0;
	}
}

void hwrng_stub_starve(int on)
{
	if (!g_inited) {
		stub_power_on();
	}
	g.starve = on ? 1 : 0;
	if (on) {
		g.fifo_len = 0;
	}
}

void hwrng_stub_alarm_after(unsigned words)
{
	if (!g_inited) {
		stub_power_on();
	}
	g.alarm_after = words;
}

void hwrng_stub_reset(void)
{
	stub_power_on();
}
