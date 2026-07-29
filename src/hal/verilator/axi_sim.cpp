// axi_sim.cpp —— 把 Verilator 仿真出来的 pqc_accel_axi 包成 C 接口
//
// 与 ntt_sim.cpp / keccak_sim.cpp 的区别在于驱动的层次：那两个直接握手算法核的
// 私有端口，这里驱动的是**真实总线** —— 控制面发 AXI4-Lite 读写事务，
// 数据面发 AXI4-Stream 数据包。因此这条路径验证的不只是算法，还包括
// docs/register-map.md 里那份寄存器映射契约在软件侧同样成立。
//
// 总线功能模型手写，不引入任何第三方 AXI 库。
// 采样时机遵循同步设计的惯例：在时钟沿之前读对端的 ready/valid，
// 沿之后再撤销自己的 valid —— 沿上是否成交由沿前的取值决定。

#include "Vpqc_accel_axi.h"
#include "verilated.h"

#include <cstddef>
#include <cstdint>

namespace {

VerilatedContext *g_ctx = nullptr;
Vpqc_accel_axi *g_dut = nullptr;
uint64_t g_cycles = 0;

constexpr int kSpinLimit = 2000000;

void edge()
{
	g_dut->clk = 1;
	g_dut->eval();
	g_ctx->timeInc(1);
	g_dut->clk = 0;
	g_dut->eval();
	g_ctx->timeInc(1);
	g_cycles++;
}

void idle_inputs()
{
	g_dut->s_axi_awaddr = 0;
	g_dut->s_axi_awvalid = 0;
	g_dut->s_axi_wdata = 0;
	g_dut->s_axi_wstrb = 0;
	g_dut->s_axi_wvalid = 0;
	g_dut->s_axi_bready = 0;
	g_dut->s_axi_araddr = 0;
	g_dut->s_axi_arvalid = 0;
	g_dut->s_axi_rready = 0;
	g_dut->s_axis_tdata = 0;
	g_dut->s_axis_tvalid = 0;
	g_dut->s_axis_tlast = 0;
	g_dut->m_axis_tready = 0;
}

void ensure_init()
{
	if (g_dut) {
		return;
	}
	g_ctx = new VerilatedContext;
	g_ctx->traceEverOn(false);
	g_dut = new Vpqc_accel_axi{g_ctx};
	g_dut->clk = 0;
	g_dut->rst_n = 0;
	idle_inputs();
	for (int i = 0; i < 5; i++) {
		edge();
	}
	g_dut->rst_n = 1;
	edge();
}

}  // namespace

extern "C" {

int axi_sim_reset(void)
{
	delete g_dut;
	g_dut = nullptr;
	delete g_ctx;
	g_ctx = nullptr;
	g_cycles = 0;
	ensure_init();
	return 0;
}

/* AXI4-Lite 写事务：AW 与 W 同时发起，等 B 响应 */
void axi_sim_write_reg(uint32_t off, uint32_t val)
{
	ensure_init();
	g_dut->s_axi_awaddr = static_cast<uint8_t>(off);
	g_dut->s_axi_awvalid = 1;
	g_dut->s_axi_wdata = val;
	g_dut->s_axi_wstrb = 0xF;
	g_dut->s_axi_wvalid = 1;
	g_dut->s_axi_bready = 1;

	bool aw_done = false, w_done = false;
	for (int spin = 0; spin < kSpinLimit; spin++) {
		g_dut->eval();
		bool aw = !aw_done && g_dut->s_axi_awready;
		bool w = !w_done && g_dut->s_axi_wready;
		bool b = g_dut->s_axi_bvalid;
		edge();
		if (aw) {
			aw_done = true;
			g_dut->s_axi_awvalid = 0;
		}
		if (w) {
			w_done = true;
			g_dut->s_axi_wvalid = 0;
		}
		if (b) {
			break;
		}
	}
	g_dut->s_axi_bready = 0;
	g_dut->s_axi_awvalid = 0;
	g_dut->s_axi_wvalid = 0;
}

/* AXI4-Lite 读事务 */
uint32_t axi_sim_read_reg(uint32_t off)
{
	ensure_init();
	g_dut->s_axi_araddr = static_cast<uint8_t>(off);
	g_dut->s_axi_arvalid = 1;
	g_dut->s_axi_rready = 1;

	uint32_t out = 0;
	bool ar_done = false;
	for (int spin = 0; spin < kSpinLimit; spin++) {
		g_dut->eval();
		bool ar = !ar_done && g_dut->s_axi_arready;
		bool rv = g_dut->s_axi_rvalid;
		uint32_t rdata = g_dut->s_axi_rdata;
		edge();
		if (ar) {
			ar_done = true;
			g_dut->s_axi_arvalid = 0;
		}
		if (rv && ar_done) {
			out = rdata;
			break;
		}
	}
	g_dut->s_axi_rready = 0;
	g_dut->s_axi_arvalid = 0;
	return out;
}

/* AXI4-Stream 送一个包，末拍带 TLAST。不足 4 字节的尾部补零。 */
void axi_sim_stream_in(const uint8_t *src, size_t n)
{
	ensure_init();
	size_t words = (n + 3) / 4;
	for (size_t i = 0; i < words; i++) {
		uint32_t w = 0;
		for (size_t b = 0; b < 4; b++) {
			size_t idx = i * 4 + b;
			if (idx < n) {
				w |= static_cast<uint32_t>(src[idx]) << (8 * b);
			}
		}
		g_dut->s_axis_tdata = w;
		g_dut->s_axis_tvalid = 1;
		g_dut->s_axis_tlast = (i + 1 == words) ? 1 : 0;
		for (int spin = 0; spin < kSpinLimit; spin++) {
			g_dut->eval();
			bool ready = g_dut->s_axis_tready;
			edge();
			if (ready) {
				break;
			}
		}
	}
	g_dut->s_axis_tvalid = 0;
	g_dut->s_axis_tlast = 0;
}

/* AXI4-Stream 收一个包，直到 TLAST 或者写满 max 字节。返回收到的字节数。 */
size_t axi_sim_stream_out(uint8_t *dst, size_t max)
{
	ensure_init();
	size_t n = 0;
	g_dut->m_axis_tready = 1;
	for (int spin = 0; spin < kSpinLimit && n < max; spin++) {
		g_dut->eval();
		bool valid = g_dut->m_axis_tvalid;
		uint32_t data = g_dut->m_axis_tdata;
		bool last = g_dut->m_axis_tlast;
		edge();
		if (valid) {
			for (int b = 0; b < 4 && n < max; b++) {
				dst[n++] = static_cast<uint8_t>(data >> (8 * b));
			}
			if (last) {
				break;
			}
		}
	}
	g_dut->m_axis_tready = 0;
	return n;
}

uint64_t axi_sim_cycles(void)
{
	return g_cycles;
}

}  // extern "C"
