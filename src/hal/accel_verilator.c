/* accel_verilator.c —— 驱动 Verilator 仿真出来的真 RTL
 *
 * 编译时若没有 PQCHSM_HAVE_VERILATOR（即本机没装 verilator），
 * 这里返回 NULL —— **如实反映"这条路没编进来"**，而不是悄悄退回软件。
 *
 * 已实现的模式：NTT_FWD / NTT_INV（hardware/rtl/mlkem/ntt_core.v）、
 *               KECCAK_F1600（hardware/rtl/keccak/keccak_f1600.v）。
 * 其余模式置 STATUS.ERR + ERRCODE=3（"该模式未实现"），
 * 上层会收到 PQC_ERR_UNSUPPORTED —— 完整的 ML-KEM/ML-DSA 核属于
 * 路线图 Phase 1–4，不是这一层能变出来的。
 */
#include "pqchsm/accel.h"

#ifdef PQCHSM_HAVE_VERILATOR

#include "pqchsm/util.h"
#include <string.h>

/* 由 src/hal/verilator/ntt_sim.cpp 提供（Verilator 生成的 C++ 模型的 C 包装） */
int      ntt_sim_reset(void);
int      ntt_sim_run(const int16_t *in, int16_t *out, int inverse);
uint64_t ntt_sim_last_cycles(void);

/* 由 src/hal/verilator/keccak_sim.cpp 提供 */
int      keccak_sim_reset(void);
int      keccak_sim_run(const uint64_t *state_in, uint64_t *state_out);
uint64_t keccak_sim_last_cycles(void);

static uint32_t g_regs[16];
static uint8_t  g_buf[ACCEL_BUF_MAX];

static int vr_reset(void)
{
	memset(g_regs, 0, sizeof(g_regs));
	pqc_secure_zero(g_buf, sizeof(g_buf));
	int a = ntt_sim_reset();
	int b = keccak_sim_reset();
	return a ? a : b;
}

static uint32_t vr_read_reg(uint32_t off)
{
	return (off / 4 < 16) ? g_regs[off / 4] : 0;
}

static void vr_write_data(uint32_t off, const uint8_t *src, size_t n)
{
	if (off < ACCEL_BUF_MAX && n <= ACCEL_BUF_MAX - off) {
		memcpy(g_buf + off, src, n);
	}
}

static void vr_read_data(uint32_t off, uint8_t *dst, size_t n)
{
	if (off < ACCEL_BUF_MAX && n <= ACCEL_BUF_MAX - off) {
		memcpy(dst, g_buf + off, n);
	}
}

static void vr_write_reg(uint32_t off, uint32_t val)
{
	if (off / 4 >= 16) {
		return;
	}
	g_regs[off / 4] = val;
	if (off != ACCEL_REG_CTRL) {
		return;
	}
	if (val & ACCEL_CTRL_SOFT_RESET) {
		vr_reset();
		return;
	}
	if (!(val & ACCEL_CTRL_START)) {
		return;
	}

	uint32_t mode = g_regs[ACCEL_REG_MODE / 4];
	uint32_t in_len = g_regs[ACCEL_REG_IN_LEN / 4];
	g_regs[ACCEL_REG_STATUS / 4] = ACCEL_ST_BUSY;

	if ((mode == ACCEL_MODE_NTT_FWD || mode == ACCEL_MODE_NTT_INV) && in_len == 512) {
		int16_t in[256], out[256];
		memcpy(in, g_buf, 512);
		if (ntt_sim_run(in, out, mode == ACCEL_MODE_NTT_INV) != 0) {
			g_regs[ACCEL_REG_ERRCODE / 4] = 2;
			g_regs[ACCEL_REG_STATUS / 4] = ACCEL_ST_DONE | ACCEL_ST_ERR;
			return;
		}
		memcpy(g_buf, out, 512);
		g_regs[ACCEL_REG_OUT_LEN / 4] = 512;
		g_regs[ACCEL_REG_STATUS / 4] = ACCEL_ST_DONE;
		return;
	}

	if (mode == ACCEL_MODE_KECCAK_F1600 && in_len == 200) {
		uint64_t in[25], out[25];
		/* 缓冲里是 200 字节小端 lane —— 显式转换，不靠 memcpy 的字节序运气 */
		for (int i = 0; i < 25; i++) {
			uint64_t v = 0;
			for (int b = 0; b < 8; b++) {
				v |= (uint64_t)g_buf[i * 8 + b] << (8 * b);
			}
			in[i] = v;
		}
		if (keccak_sim_run(in, out) != 0) {
			g_regs[ACCEL_REG_ERRCODE / 4] = 2;
			g_regs[ACCEL_REG_STATUS / 4] = ACCEL_ST_DONE | ACCEL_ST_ERR;
			return;
		}
		for (int i = 0; i < 25; i++) {
			for (int b = 0; b < 8; b++) {
				g_buf[i * 8 + b] = (uint8_t)(out[i] >> (8 * b));
			}
		}
		g_regs[ACCEL_REG_OUT_LEN / 4] = 200;
		g_regs[ACCEL_REG_STATUS / 4] = ACCEL_ST_DONE;
		return;
	}

	/* 其余模式：RTL 侧还没有对应的核。**明确报"未实现"而不是偷偷回落软件** ——
	 * 否则"跑通了"就变成了假象。完整的 ML-KEM/ML-DSA 核是 Phase 1–4 的工作。 */
	g_regs[ACCEL_REG_ERRCODE / 4] = 3;
	g_regs[ACCEL_REG_STATUS / 4] = ACCEL_ST_DONE | ACCEL_ST_ERR;
}

static const accel_transport_t g_verilator = {
	.name        = "verilator(RTL ntt_core + keccak_f1600; 仅 NTT/Keccak 模式)",
	.is_hardware = 0,          /* 仿真不是硬件 */
	.reset       = vr_reset,
	.write_reg   = vr_write_reg,
	.read_reg    = vr_read_reg,
	.write_data  = vr_write_data,
	.read_data   = vr_read_data,
};

const accel_transport_t *accel_transport_verilator(void)
{
	return &g_verilator;
}

uint64_t accel_verilator_last_cycles(void)
{
	return ntt_sim_last_cycles();
}

uint64_t accel_verilator_keccak_cycles(void)
{
	return keccak_sim_last_cycles();
}

#else

const accel_transport_t *accel_transport_verilator(void)
{
	return NULL;
}

uint64_t accel_verilator_last_cycles(void)
{
	return 0;
}

uint64_t accel_verilator_keccak_cycles(void)
{
	return 0;
}

#endif
