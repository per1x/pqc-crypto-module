/* 经 AXI 总线驱动仿真 RTL 的后端
 *
 * 与 test_accel.c 的分工：那里验证"寄存器语义在 C 里成立"，这里验证
 * "同一套语义在真实总线事务上成立"。命令要经过 AXI4-Lite 的读写握手与
 * AXI4-Stream 的数据包，走的是 docs/register-map.md 里那份契约。
 *
 * 三条判据：
 *   1. 软件桩与仿真 RTL 后端算出来的结果**逐字节相同**；
 *   2. 在 RTL 之上搭出的 SHA3/SHAKE 与 OpenSSL 逐字节相同 ——
 *      这一条覆盖 padding、rate、lane 字节序与多块吸收挤压，而不只是置换；
 *   3. 寄存器契约的软件侧行为：VERSION 为常量、START 自清、DONE 电平锁存、
 *      只读寄存器不被软件改写、未实现的模式如实返回 UNSUPPORTED。
 *
 * 没编 Verilator 支持时 accel_transport_axi() 返回 NULL，整个用例如实 SKIP。
 */
#include "testlib.h"
#include "pqchsm/accel.h"
#include "pqchsm/util.h"

#include <openssl/evp.h>

#include <stdint.h>

/* 用软件桩算一遍同样的输入，作为逐字节比对的基准 */
static void keccak_via_stub(const uint8_t in[200], uint8_t out[200])
{
	accel_set_transport(accel_transport_stub());
	CHECK_EQ_INT(accel_keccak_f1600(in, out), 0);
}

static void test_keccak_equivalence(const accel_transport_t *axi)
{
	TCASE("Keccak-f[1600]：AXI 后端与软件桩逐字节等价");
	for (int trial = 0; trial < 8; trial++) {
		uint8_t in[200], want[200], got[200];
		for (int i = 0; i < 200; i++) {
			in[i] = (uint8_t)(trial == 0 ? 0 : (i * 31 + trial * 7));
		}
		keccak_via_stub(in, want);

		accel_set_transport(axi);
		CHECK_EQ_INT(accel_keccak_f1600(in, got), 0);
		CHECK_EQ_MEM(want, got, 200);
	}
}

static void test_ntt_equivalence(const accel_transport_t *axi)
{
	TCASE("NTT 正逆变换：AXI 后端与软件桩逐字节等价");
	for (int inverse = 0; inverse <= 1; inverse++) {
		for (int trial = 0; trial < 4; trial++) {
			int16_t in[256], want[256], got[256];
			for (int i = 0; i < 256; i++) {
				in[i] = (int16_t)(((i * 37 + trial * 101) % 3329) - 1664);
			}
			accel_set_transport(accel_transport_stub());
			CHECK_EQ_INT(accel_ntt(in, want, inverse), 0);

			accel_set_transport(axi);
			CHECK_EQ_INT(accel_ntt(in, got, inverse), 0);
			CHECK_EQ_MEM(want, got, sizeof(want));
		}
	}
}

/* 海绵结构建在 RTL 置换之上，结果必须与 OpenSSL 逐字节相同 */
static void test_sponge_against_openssl(const accel_transport_t *axi)
{
	TCASE("SHA3/SHAKE 走 AXI 后端与 OpenSSL 逐字节一致");
	const struct {
		const char *name;
		int rate;
		uint8_t suffix;
		size_t out_len;
	} cases[] = {
		{ "SHA3-256", 136, 0x06, 32 },
		{ "SHA3-512", 72,  0x06, 64 },
		{ "SHAKE128", 168, 0x1F, 32 },
		{ "SHAKE256", 136, 0x1F, 64 },
	};

	static const size_t msg_lens[] = { 0, 1, 63, 135, 136, 137, 200, 400 };

	for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		for (size_t m = 0; m < sizeof(msg_lens) / sizeof(msg_lens[0]); m++) {
			uint8_t msg[400];
			size_t msg_len = msg_lens[m];
			for (size_t i = 0; i < msg_len; i++) {
				msg[i] = (uint8_t)(i * 13 + c);
			}

			uint8_t want[64], got[64];
			unsigned int wlen = 0;
			EVP_MD_CTX *ctx = EVP_MD_CTX_new();
			const EVP_MD *md = EVP_get_digestbyname(cases[c].name);
			CHECK(md != NULL);
			CHECK_EQ_INT(EVP_DigestInit_ex(ctx, md, NULL), 1);
			CHECK_EQ_INT(EVP_DigestUpdate(ctx, msg, msg_len), 1);
			if (cases[c].suffix == 0x1F) {
				CHECK_EQ_INT(EVP_DigestFinalXOF(ctx, want, cases[c].out_len), 1);
			} else {
				CHECK_EQ_INT(EVP_DigestFinal_ex(ctx, want, &wlen), 1);
			}
			EVP_MD_CTX_free(ctx);

			accel_set_transport(axi);
			CHECK_EQ_INT(accel_shake(cases[c].rate, cases[c].suffix,
			                         msg, msg_len, got, cases[c].out_len), 0);
			CHECK_EQ_MEM(want, got, cases[c].out_len);
		}
	}
}

static void test_register_contract(const accel_transport_t *axi)
{
	accel_set_transport(axi);

	TCASE("VERSION 是常量");
	CHECK_EQ_INT(axi->read_reg(0x1C), 0x00010000);

	TCASE("START 由硬件自清");
	axi->write_reg(ACCEL_REG_MODE, ACCEL_MODE_KEM_KEYGEN);   /* 未实现的模式 */
	axi->write_reg(ACCEL_REG_IN_LEN, 64);
	axi->write_reg(ACCEL_REG_CTRL, ACCEL_CTRL_START);
	for (int i = 0; i < 4; i++) {
		CHECK_EQ_INT(axi->read_reg(ACCEL_REG_CTRL), 0);
	}

	TCASE("未实现的模式如实置 ERR 与 ERRCODE=3");
	CHECK(axi->read_reg(ACCEL_REG_STATUS) & ACCEL_ST_DONE);
	CHECK(axi->read_reg(ACCEL_REG_STATUS) & ACCEL_ST_ERR);
	CHECK_EQ_INT(axi->read_reg(ACCEL_REG_ERRCODE), 3);

	TCASE("DONE 是电平锁存，反复读不清除");
	uint8_t state[200];
	memset(state, 0x5A, sizeof(state));
	uint8_t out[200];
	CHECK_EQ_INT(accel_keccak_f1600(state, out), 0);
	for (int i = 0; i < 16; i++) {
		CHECK(axi->read_reg(ACCEL_REG_STATUS) & ACCEL_ST_DONE);
		CHECK(!(axi->read_reg(ACCEL_REG_STATUS) & ACCEL_ST_BUSY));
	}

	TCASE("STATUS / OUT_LEN / ERRCODE 软件写入被忽略");
	uint32_t st_before = axi->read_reg(ACCEL_REG_STATUS);
	uint32_t ol_before = axi->read_reg(ACCEL_REG_OUT_LEN);
	axi->write_reg(ACCEL_REG_STATUS, 0xDEADBEEF);
	axi->write_reg(ACCEL_REG_OUT_LEN, 0xDEADBEEF);
	axi->write_reg(ACCEL_REG_ERRCODE, 0xDEADBEEF);
	CHECK_EQ_INT(axi->read_reg(ACCEL_REG_STATUS), st_before);
	CHECK_EQ_INT(axi->read_reg(ACCEL_REG_OUT_LEN), ol_before);
	CHECK(axi->read_reg(ACCEL_REG_ERRCODE) != 0xDEADBEEF);

	TCASE("SOFT_RESET 清状态位");
	axi->write_reg(ACCEL_REG_CTRL, ACCEL_CTRL_SOFT_RESET);
	CHECK_EQ_INT(axi->read_reg(ACCEL_REG_STATUS), 0);
}

static void test_unsupported_modes(const accel_transport_t *axi)
{
	TCASE("完整算法模式在 AXI 后端上返回 UNSUPPORTED");
	accel_set_transport(axi);
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

int main(void)
{
	const accel_transport_t *axi = accel_transport_axi();
	if (!axi) {
		printf("test_accel_axi: SKIP —— 没编 Verilator 支持，"
		       "AXI transport 不可用\n");
		return 0;
	}
	CHECK_EQ_INT(axi->is_hardware, 0);       /* 仿真不是硬件，不得谎报 */

	test_keccak_equivalence(axi);
	test_ntt_equivalence(axi);
	test_sponge_against_openssl(axi);
	test_register_contract(axi);
	test_unsupported_modes(axi);

	accel_set_transport(NULL);
	return test_report("test_accel_axi");
}
