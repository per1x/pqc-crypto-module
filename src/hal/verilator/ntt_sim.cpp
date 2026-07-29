// ntt_sim.cpp —— 把 Verilator 仿真出来的 ntt_core 包成 C 接口
//
// 这是 Level B 的落点：**同一个 RTL 模块，既被 cocotb 对拍，也被 C 代码
// 当成"加速器"驱动**。上面接 src/hal/accel_verilator.c 实现 accel_transport_t，
// 再上面就是 pqc_accel.c 的寄存器语义 —— 与将来真 PL 走的是同一条路径。
//
// 时钟推进在这里是"想推多少推多少"（仿真时间与真实时间无关）；
// 换成真 PL 之后这段变成"写寄存器 + 等中断"，上层看不出区别。

#include "Vntt_core.h"
#include "verilated.h"

#include <cstdint>
#include <cstring>

/* Verilator 的运行时要求提供仿真时间；我们用 VerilatedContext 自己推进时间，
 * 这个回调只是让链接器满意。 */
double sc_time_stamp()
{
	return 0;
}

namespace {

VerilatedContext *g_ctx = nullptr;
Vntt_core *g_dut = nullptr;
uint64_t g_time = 0;
uint64_t g_last_cycles = 0;

void tick()
{
	g_dut->clk = 0;
	g_dut->eval();
	g_ctx->timeInc(1);
	g_dut->clk = 1;
	g_dut->eval();
	g_ctx->timeInc(1);
	g_time++;
}

void ensure_init()
{
	if (g_dut) {
		return;
	}
	g_ctx = new VerilatedContext;
	g_ctx->traceEverOn(false);
	g_dut = new Vntt_core{g_ctx};
	g_dut->rst_n = 0;
	g_dut->start = 0;
	g_dut->wr_en = 0;
	g_dut->inverse = 0;
	for (int i = 0; i < 5; i++) {
		tick();
	}
	g_dut->rst_n = 1;
	tick();
}

}  // namespace

extern "C" {

/* 复位仿真模型 */
int ntt_sim_reset(void)
{
	if (g_dut) {
		delete g_dut;
		g_dut = nullptr;
	}
	if (g_ctx) {
		delete g_ctx;
		g_ctx = nullptr;
	}
	ensure_init();
	return 0;
}

/* 跑一次 NTT。in/out 各 256 个 int16。成功返回 0。 */
int ntt_sim_run(const int16_t *in, int16_t *out, int inverse)
{
	if (!in || !out) {
		return -1;
	}
	ensure_init();

	// 写系数
	for (int i = 0; i < 256; i++) {
		g_dut->wr_en = 1;
		g_dut->wr_addr = static_cast<uint8_t>(i);
		g_dut->wr_data = static_cast<uint16_t>(in[i]);
		tick();
	}
	g_dut->wr_en = 0;
	tick();

	// 启动并等 done —— 与真 PL 的"写 CTRL.START、轮询 STATUS.DONE"同形
	g_dut->inverse = inverse ? 1 : 0;
	g_dut->start = 1;
	tick();
	g_dut->start = 0;

	uint64_t cycles = 0;
	while (!g_dut->done) {
		tick();
		if (++cycles > 100000) {
			return -1;   // 核挂了
		}
	}
	g_last_cycles = cycles;

	// 读回
	for (int i = 0; i < 256; i++) {
		g_dut->rd_addr = static_cast<uint8_t>(i);
		g_dut->eval();
		out[i] = static_cast<int16_t>(g_dut->rd_data);
	}
	return 0;
}

/* 上一次变换用了多少 cycle —— 无板阶段的性能数据就是从这里来的 */
uint64_t ntt_sim_last_cycles(void)
{
	return g_last_cycles;
}

}  // extern "C"
