// keccak_sim.cpp —— 把 Verilator 仿真出来的 keccak_f1600 包成 C 接口
//
// 与 ntt_sim.cpp 同形：同一个 RTL 模块既被 cocotb 对拍，也被 C 代码当"加速器"驱动。
// 上面接 accel_verilator.c 的 ACCEL_MODE_KECCAK_F1600，再上面 accel_shake()
// 在其之上搭海绵 —— 于是 SHA3/SHAKE 这条路径整条都能跑在仿真 RTL 上。

#include "Vkeccak_f1600.h"
#include "verilated.h"

#include <cstdint>

namespace {

VerilatedContext *g_ctx = nullptr;
Vkeccak_f1600 *g_dut = nullptr;
uint64_t g_last_cycles = 0;

void tick()
{
	g_dut->clk = 0;
	g_dut->eval();
	g_ctx->timeInc(1);
	g_dut->clk = 1;
	g_dut->eval();
	g_ctx->timeInc(1);
}

void ensure_init()
{
	if (g_dut) {
		return;
	}
	g_ctx = new VerilatedContext;
	g_ctx->traceEverOn(false);
	g_dut = new Vkeccak_f1600{g_ctx};
	g_dut->rst_n = 0;
	g_dut->start = 0;
	g_dut->wr_en = 0;
	for (int i = 0; i < 5; i++) {
		tick();
	}
	g_dut->rst_n = 1;
	tick();
}

}  // namespace

extern "C" {

int keccak_sim_reset(void)
{
	delete g_dut;
	g_dut = nullptr;
	delete g_ctx;
	g_ctx = nullptr;
	ensure_init();
	return 0;
}

/* 跑一次 Keccak-f[1600]。state 是 25 个 lane（小端 uint64）。成功返回 0。 */
int keccak_sim_run(const uint64_t *state_in, uint64_t *state_out)
{
	if (!state_in || !state_out) {
		return -1;
	}
	ensure_init();

	for (int i = 0; i < 25; i++) {
		g_dut->wr_en = 1;
		g_dut->wr_addr = static_cast<uint8_t>(i);
		g_dut->wr_data = state_in[i];
		tick();
	}
	g_dut->wr_en = 0;
	tick();

	// done 是电平语义 —— 先确认这次 start 把它清掉了，再等它重新拉高。
	// 否则上一次残留的 done=1 会让等待循环立刻退出，读到变换中途的状态。
	g_dut->start = 1;
	tick();
	g_dut->start = 0;
	if (g_dut->done) {
		return -1;
	}

	uint64_t cycles = 0;
	while (!g_dut->done) {
		tick();
		if (++cycles > 10000) {
			return -1;
		}
	}
	g_last_cycles = cycles;

	for (int i = 0; i < 25; i++) {
		g_dut->rd_addr = static_cast<uint8_t>(i);
		g_dut->eval();
		state_out[i] = g_dut->rd_data;
	}
	return 0;
}

uint64_t keccak_sim_last_cycles(void)
{
	return g_last_cycles;
}

}  // extern "C"
