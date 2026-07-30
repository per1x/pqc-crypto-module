/* Zynq MMIO 后端：寄存器与 DMA 时序对着硬件模型验证
 *
 * 【为什么这个测试有意义】真实硬件不在手边，但驱动里真正容易错的部分并不需要
 * 硬件才能验：DMA 的地址算错、接收通道没武装就启动发送、拿 IDLE 当完成判据、
 * 输出长度取错、错误路径返回成功。这些都是**时序与地址**的问题。
 *
 * 做法是把映射换成内存，并起一个线程扮演硬件：它按 AXI-DMA 与
 * docs/register-map.md 的语义响应寄存器写入 —— 复位位写 1 后自清、IOC 由软件
 * 写 1 清除、CTRL.START 自清、DONE 电平锁存、未实现的操作码置 ERR 与 ERRCODE=3。
 *
 * 模型还会**校验它收到的输入**是否与驱动被要求发送的一致：驱动把输入放错位置、
 * 长度算错，模型都会拒绝，测试随之失败。因此输入通路不是走过场。
 *
 * 上板之后仍然未知的只有两件事：地址表与真实地址映射是否一致、时钟与复位是否
 * 真的通。那两件事无论如何都要上板才知道，本测试不声称覆盖它们。
 */
#include "testlib.h"

#include "pqc_accel_zynq.h"
#include "pqchsm/accel.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#define BUF_SPAN 0x1000u          /* 4 KiB：输入输出各 2 KiB，够 NTT 的 512 字节 */
#define BUF_HALF (BUF_SPAN / 2)
#define TEST_BUF_PHYS 0x3E000000u

#define REG_VERSION 0x1Cu
#define VERSION_CONST 0x00010000u

/* ---- 假映射 ---- */
static uint32_t *g_accel_mem;
static uint32_t *g_dma_mem;
static uint8_t  *g_buf_mem;

static void *fake_map(uint32_t phys, uint32_t span, void *user)
{
	(void)user;
	if (phys == PQC_ZYNQ_ACCEL_BASE && span == PQC_ZYNQ_ACCEL_SPAN) {
		return g_accel_mem;
	}
	if (phys == PQC_ZYNQ_DMA_BASE && span == PQC_ZYNQ_DMA_SPAN) {
		return g_dma_mem;
	}
	if (phys == TEST_BUF_PHYS && span == BUF_SPAN) {
		return g_buf_mem;
	}
	return NULL;
}

static void fake_unmap(void *addr, uint32_t span, void *user)
{
	(void)addr;
	(void)span;
	(void)user;
}

/* ---- 硬件模型 ---- */

static volatile int      g_model_run;
static volatile int      g_model_input_bad;   /* 模型收到的输入与预期不符 */
static uint8_t           g_want_in[BUF_HALF];
static volatile uint32_t g_want_in_len;
static uint8_t           g_give_out[BUF_HALF];
static volatile uint32_t g_give_out_len;

/* 反证开关：让模型故意不完成某一步，验证驱动会失败而不是返回垃圾 */
static volatile int g_break_s2mm;   /* S2MM 永不置 IOC */
static volatile int g_break_done;   /* 加速器永不置 DONE */

static uint32_t rdr(volatile uint32_t *b, uint32_t off) { return b[off / 4]; }
static void     wrr(volatile uint32_t *b, uint32_t off, uint32_t v) { b[off / 4] = v; }

static void *model_thread(void *arg)
{
	(void)arg;
	volatile uint32_t *ar = g_accel_mem;
	volatile uint32_t *dr = g_dma_mem;

	int mm2s_done = 0;

	while (g_model_run) {
		/* 复位位写 1 之后由硬件自清 */
		if (rdr(dr, DMA_MM2S_DMACR) & DMA_CR_RESET) {
			wrr(dr, DMA_MM2S_DMACR, 0);
			wrr(dr, DMA_MM2S_DMASR, DMA_SR_IDLE | DMA_SR_HALTED);
			mm2s_done = 0;
		}
		if (rdr(dr, DMA_S2MM_DMACR) & DMA_CR_RESET) {
			wrr(dr, DMA_S2MM_DMACR, 0);
			wrr(dr, DMA_S2MM_DMASR, DMA_SR_IDLE | DMA_SR_HALTED);
		}

		/* IOC 由软件写 1 清除。模型置位时一定带 IDLE，因此"只有 IOC"这个取值
		 * 只可能来自软件的清除写 —— 据此区分两者。 */
		if (rdr(dr, DMA_MM2S_DMASR) == DMA_SR_IOC) {
			wrr(dr, DMA_MM2S_DMASR, DMA_SR_IDLE);
			mm2s_done = 0;
		}
		if (rdr(dr, DMA_S2MM_DMASR) == DMA_SR_IOC) {
			wrr(dr, DMA_S2MM_DMASR, DMA_SR_IDLE);
		}

		/* MM2S：软件配好 RS 与 LENGTH 之后，一次传输即完成。
		 * 数据本来就在共享缓冲里，模型在这里做的是**校验**： */
		if (!mm2s_done && (rdr(dr, DMA_MM2S_DMACR) & DMA_CR_RS)
		    && rdr(dr, DMA_MM2S_LENGTH) != 0) {
			uint32_t len = rdr(dr, DMA_MM2S_LENGTH);
			if (rdr(dr, DMA_MM2S_SA) != TEST_BUF_PHYS || len != g_want_in_len) {
				g_model_input_bad = 1;
			} else {
				for (uint32_t i = 0; i < len; i++) {
					if (g_buf_mem[i] != g_want_in[i]) {
						g_model_input_bad = 1;
						break;
					}
				}
			}
			mm2s_done = 1;
			wrr(dr, DMA_MM2S_DMASR, DMA_SR_IOC | DMA_SR_IDLE);
		}

		/* 加速器：CTRL.START 自清，随后按操作码给结果 */
		uint32_t ctrl = rdr(ar, ACCEL_REG_CTRL);
		if (ctrl & ACCEL_CTRL_SOFT_RESET) {
			wrr(ar, ACCEL_REG_CTRL, 0);
			wrr(ar, ACCEL_REG_STATUS, 0);
			wrr(ar, ACCEL_REG_OUT_LEN, 0);
			wrr(ar, ACCEL_REG_ERRCODE, 0);
		} else if (ctrl & ACCEL_CTRL_START) {
			wrr(ar, ACCEL_REG_CTRL, 0);
			wrr(ar, ACCEL_REG_STATUS, ACCEL_ST_BUSY);

			uint32_t mode = rdr(ar, ACCEL_REG_MODE);
			uint32_t ilen = rdr(ar, ACCEL_REG_IN_LEN);
			int ok = ((mode == ACCEL_MODE_NTT_FWD || mode == ACCEL_MODE_NTT_INV)
			          && ilen == 512)
			      || (mode == ACCEL_MODE_KECCAK_F1600 && ilen == 200);

			if (!ok) {
				wrr(ar, ACCEL_REG_OUT_LEN, 0);
				wrr(ar, ACCEL_REG_ERRCODE, 3);
				wrr(ar, ACCEL_REG_STATUS, ACCEL_ST_DONE | ACCEL_ST_ERR);
			} else {
				/* 结果由 S2MM 搬回缓冲的后半 */
				if (!g_break_s2mm) {
					for (uint32_t i = 0; i < g_give_out_len; i++) {
						g_buf_mem[BUF_HALF + i] = g_give_out[i];
					}
					wrr(dr, DMA_S2MM_LENGTH, g_give_out_len);
					wrr(dr, DMA_S2MM_DMASR, DMA_SR_IOC | DMA_SR_IDLE);
				}
				wrr(ar, ACCEL_REG_OUT_LEN, g_give_out_len);
				wrr(ar, ACCEL_REG_ERRCODE, 0);
				if (!g_break_done) {
					wrr(ar, ACCEL_REG_STATUS, ACCEL_ST_DONE);
				}
			}
		}

		usleep(20);
	}
	return NULL;
}

/* ---- 用例 ---- */

static void set_expectation(const uint8_t *in, uint32_t in_len,
                            const uint8_t *out, uint32_t out_len)
{
	memcpy(g_want_in, in, in_len);
	g_want_in_len = in_len;
	memcpy(g_give_out, out, out_len);
	g_give_out_len = out_len;
	g_model_input_bad = 0;
}

static void test_open_and_version(const accel_transport_t *t)
{
	TCASE("打开之后 transport 可用，并如实报告是硬件");
	CHECK(t != NULL);
	CHECK_EQ_INT(t->is_hardware, 1);

	TCASE("VERSION 是常量");
	CHECK_EQ_INT(t->read_reg(REG_VERSION), (long)VERSION_CONST);
}

static void test_keccak_path(const accel_transport_t *t)
{
	uint8_t in[200], want[200], got[200];
	for (int i = 0; i < 200; i++) {
		in[i] = (uint8_t)(i * 7 + 1);
	}

	/* 期望值用软件桩算，模型照它回给驱动 */
	accel_set_transport(accel_transport_stub());
	CHECK_EQ_INT(accel_keccak_f1600(in, want), 0);

	TCASE("Keccak 命令走完整的寄存器 + DMA 路径");
	set_expectation(in, 200, want, 200);
	accel_set_transport(t);
	CHECK_EQ_INT(accel_keccak_f1600(in, got), 0);
	CHECK_EQ_MEM(want, got, 200);
	CHECK_EQ_INT(g_model_input_bad, 0);

	TCASE("DMA 地址与长度按缓冲布局编程");
	CHECK_EQ_INT(g_dma_mem[DMA_MM2S_SA / 4], (long)TEST_BUF_PHYS);
	CHECK_EQ_INT(g_dma_mem[DMA_MM2S_LENGTH / 4], 200);
	CHECK_EQ_INT(g_dma_mem[DMA_S2MM_DA / 4], (long)(TEST_BUF_PHYS + BUF_HALF));

	TCASE("轮询确实发生过");
	CHECK(pqc_zynq_last_poll_count() > 0);
}

static void test_ntt_path(const accel_transport_t *t)
{
	int16_t in[256], want[256], got[256];
	for (int i = 0; i < 256; i++) {
		in[i] = (int16_t)(((i * 41) % 3329) - 1664);
	}
	accel_set_transport(accel_transport_stub());
	CHECK_EQ_INT(accel_ntt(in, want, 0), 0);

	TCASE("NTT 命令走完整的寄存器 + DMA 路径");
	set_expectation((const uint8_t *)in, 512, (const uint8_t *)want, 512);
	accel_set_transport(t);
	CHECK_EQ_INT(accel_ntt(in, got, 0), 0);
	CHECK_EQ_MEM(want, got, sizeof(want));
	CHECK_EQ_INT(g_model_input_bad, 0);
	CHECK_EQ_INT(g_dma_mem[DMA_MM2S_LENGTH / 4], 512);
}

static void test_unsupported_mode(const accel_transport_t *t)
{
	TCASE("未实现的操作码返回 UNSUPPORTED，而不是假装成功");
	accel_set_transport(t);
	pqc_set_backend(pqc_backend_accel());

	const pqc_alg_info_t *info = pqc_alg_info(PQC_ALG_ML_KEM_768);
	uint8_t seed[64] = { 0 };
	uint8_t *pk = malloc(info->pk_len);
	uint8_t *sk = malloc(info->sk_len);
	CHECK_EQ_INT(pqc_keypair_from_seed(PQC_ALG_ML_KEM_768, seed, info->seed_len,
	                                   pk, sk),
	             PQC_ERR_UNSUPPORTED);
	free(pk);
	free(sk);
	pqc_set_backend(pqc_backend_liboqs());
}

/* 反证：模型故意不完成某一步，驱动必须失败。
 * 没有这两条，"命令成功"可能只是因为驱动根本没检查过完成条件。 */
static void test_negative_controls(const accel_transport_t *t)
{
	uint8_t in[200], want[200], got[200];
	for (int i = 0; i < 200; i++) {
		in[i] = (uint8_t)(i + 3);
	}
	accel_set_transport(accel_transport_stub());
	CHECK_EQ_INT(accel_keccak_f1600(in, want), 0);
	accel_set_transport(t);

	TCASE("反证：S2MM 永不完成时命令必须失败");
	set_expectation(in, 200, want, 200);
	g_break_s2mm = 1;
	memset(got, 0xAA, sizeof(got));
	CHECK(accel_keccak_f1600(in, got) != 0);
	g_break_s2mm = 0;
	t->reset();

	TCASE("反证：DONE 永不置位时命令必须失败");
	set_expectation(in, 200, want, 200);
	g_break_done = 1;
	memset(got, 0xAA, sizeof(got));
	CHECK(accel_keccak_f1600(in, got) != 0);
	g_break_done = 0;
	t->reset();

	TCASE("恢复正常后仍能算对 —— 说明失败不是把驱动弄坏了");
	set_expectation(in, 200, want, 200);
	CHECK_EQ_INT(accel_keccak_f1600(in, got), 0);
	CHECK_EQ_MEM(want, got, 200);
}

static void test_input_check_has_teeth(const accel_transport_t *t)
{
	/* 模型对输入的校验本身也要证明有区分能力：故意把预期输入改一个字节，
	 * 模型必须报出不符。否则"输入通路正确"这个结论是空的。 */
	TCASE("反证：模型的输入校验确有区分能力");
	uint8_t in[200], want[200], got[200];
	for (int i = 0; i < 200; i++) {
		in[i] = (uint8_t)(i * 5);
	}
	accel_set_transport(accel_transport_stub());
	CHECK_EQ_INT(accel_keccak_f1600(in, want), 0);
	accel_set_transport(t);

	set_expectation(in, 200, want, 200);
	g_want_in[100] ^= 0x01;          /* 让模型期待一份错的输入 */
	CHECK_EQ_INT(accel_keccak_f1600(in, got), 0);
	CHECK_EQ_INT(g_model_input_bad, 1);
	g_model_input_bad = 0;
}

int main(void)
{
	g_accel_mem = calloc(PQC_ZYNQ_ACCEL_SPAN / 4, sizeof(uint32_t));
	g_dma_mem   = calloc(PQC_ZYNQ_DMA_SPAN / 4, sizeof(uint32_t));
	g_buf_mem   = calloc(BUF_SPAN, 1);
	if (!g_accel_mem || !g_dma_mem || !g_buf_mem) {
		fprintf(stderr, "test_accel_zynq: 分配假映射失败\n");
		return 1;
	}
	g_accel_mem[REG_VERSION / 4] = VERSION_CONST;

	g_model_run = 1;
	pthread_t th;
	if (pthread_create(&th, NULL, model_thread, NULL) != 0) {
		fprintf(stderr, "test_accel_zynq: 起硬件模型线程失败\n");
		return 1;
	}

	pqc_zynq_config_t cfg;
	pqc_zynq_default_config(&cfg);
	cfg.mode     = PQC_ZYNQ_MAP_CUSTOM;
	cfg.mapper   = fake_map;
	cfg.unmapper = fake_unmap;
	cfg.buf_phys = TEST_BUF_PHYS;
	cfg.buf_span = BUF_SPAN;

	int rc = pqc_zynq_open(&cfg);
	CHECK_EQ_INT(rc, 0);

	const accel_transport_t *t = accel_transport_zynq();
	if (rc == 0 && t) {
		test_open_and_version(t);
		test_keccak_path(t);
		test_ntt_path(t);
		test_unsupported_mode(t);
		test_negative_controls(t);
		test_input_check_has_teeth(t);
	}

	g_model_run = 0;
	pthread_join(th, NULL);

	accel_set_transport(NULL);
	pqc_zynq_close();
	free(g_accel_mem);
	free(g_dma_mem);
	free(g_buf_mem);
	return test_report("test_accel_zynq");
}
