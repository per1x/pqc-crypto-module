/* pqc_accel.c —— 用寄存器接口实现 pqc_backend_t
 *
 * 这是"算法核搬到 FPGA"的那条缝的上半截：它只会写寄存器、送数据、轮询 DONE，
 * **完全不知道**下面接的是软件桩、Verilator 仿真还是真 PL。
 *
 * 与 pqc_backend_liboqs() 可互换：
 *     pqc_set_backend(pqc_backend_accel());
 * 换完之后槽位管理器、密钥库、备份恢复、PKCS#11 全都照常工作 ——
 * tests/unit/test_accel.c 就是把整套 crypto 测试在这个后端下再跑一遍。
 */
#include "pqchsm/accel.h"
#include "pqchsm/util.h"

#include <string.h>

static const accel_transport_t *g_tr;

void accel_set_transport(const accel_transport_t *t)
{
	g_tr = t;
	if (g_tr && g_tr->reset) {
		g_tr->reset();
	}
}

const accel_transport_t *accel_get_transport(void)
{
	if (!g_tr) {
		g_tr = accel_transport_stub();
	}
	return g_tr;
}

/* 一次完整的命令握手：写模式 → 送输入 → START → 等 DONE → 查 ERR。
 * 真 PL 上 START 之后是等中断或轮询 STATUS，这里的形状与之完全一致。 */
static pqc_status_t run(uint32_t mode, pqc_alg_t alg,
                        const uint8_t *in, size_t in_len, uint32_t *out_len)
{
	const accel_transport_t *t = accel_get_transport();
	if (!t || in_len > ACCEL_BUF_MAX) {
		return PQC_ERR_BAD_ARG;
	}
	t->write_reg(ACCEL_REG_MODE, mode);
	t->write_reg(ACCEL_REG_PARAM, (uint32_t)alg);
	t->write_reg(ACCEL_REG_IN_LEN, (uint32_t)in_len);
	if (in_len) {
		t->write_data(0, in, in_len);
	}
	t->write_reg(ACCEL_REG_CTRL, ACCEL_CTRL_START);

	/* 轮询 DONE。软件桩里是立刻置位；真 PL 上这里会转几十到几千圈，
	 * 或者改成等中断 —— 上层看不出区别。 */
	uint32_t st = 0;
	for (int spin = 0; spin < 1000000; spin++) {
		st = t->read_reg(ACCEL_REG_STATUS);
		if (st & ACCEL_ST_DONE) {
			break;
		}
	}
	if (!(st & ACCEL_ST_DONE)) {
		return PQC_ERR_BACKEND;      /* 超时：真硬件上多半是核挂了 */
	}
	if (st & ACCEL_ST_ERR) {
		uint32_t e = t->read_reg(ACCEL_REG_ERRCODE);
		/* 3 = 该模式没实现（例如 Verilator 后端只有 NTT） */
		return (e == 3) ? PQC_ERR_UNSUPPORTED : PQC_ERR_BACKEND;
	}
	if (out_len) {
		*out_len = t->read_reg(ACCEL_REG_OUT_LEN);
	}
	return PQC_OK;
}

static void put_u32le(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++) {
		p[i] = (uint8_t)(v >> (8 * i));
	}
}

/* ---- vtable ---- */

static pqc_status_t ac_keypair_from_seed(pqc_alg_t alg, const uint8_t *seed, size_t seed_len,
                                         uint8_t *pk, uint8_t *sk)
{
	const pqc_alg_info_t *info = pqc_alg_info(alg);
	if (!info || seed_len != info->seed_len) {
		return PQC_ERR_BAD_ARG;
	}
	uint32_t mode = (info->kind == PQC_KIND_KEM) ? ACCEL_MODE_KEM_KEYGEN
	                                             : ACCEL_MODE_SIG_KEYGEN;
	uint32_t out = 0;
	pqc_status_t st = run(mode, alg, seed, seed_len, &out);
	if (st != PQC_OK) {
		return st;
	}
	if (out != info->pk_len + info->sk_len) {
		return PQC_ERR_BACKEND;
	}
	const accel_transport_t *t = accel_get_transport();
	t->read_data(0, pk, info->pk_len);
	t->read_data((uint32_t)info->pk_len, sk, info->sk_len);
	return PQC_OK;
}

static pqc_status_t ac_keypair(pqc_alg_t alg, uint8_t *pk, uint8_t *sk)
{
	const pqc_alg_info_t *info = pqc_alg_info(alg);
	if (!info) {
		return PQC_ERR_BAD_ARG;
	}
	/* 真 PL 上种子由片内 TRNG 出；这里从软件随机源取，语义相同 */
	uint8_t seed[64];
	if (pqc_random_bytes(seed, info->seed_len) != 0) {
		return PQC_ERR_BACKEND;
	}
	pqc_status_t st = ac_keypair_from_seed(alg, seed, info->seed_len, pk, sk);
	pqc_secure_zero(seed, sizeof(seed));
	return st;
}

static pqc_status_t ac_encaps_derand(pqc_alg_t alg, const uint8_t *pk,
                                     const uint8_t *m, size_t m_len,
                                     uint8_t *ct, uint8_t *ss)
{
	const pqc_alg_info_t *info = pqc_alg_info(alg);
	if (!info || info->kind != PQC_KIND_KEM || m_len != 32) {
		return PQC_ERR_BAD_ARG;
	}
	static uint8_t buf[ACCEL_BUF_MAX];
	memcpy(buf, pk, info->pk_len);
	memcpy(buf + info->pk_len, m, 32);
	uint32_t out = 0;
	pqc_status_t st = run(ACCEL_MODE_KEM_ENCAPS, alg, buf, info->pk_len + 32, &out);
	if (st != PQC_OK) {
		return st;
	}
	const accel_transport_t *t = accel_get_transport();
	t->read_data(0, ct, info->ct_len);
	t->read_data((uint32_t)info->ct_len, ss, info->ss_len);
	return PQC_OK;
}

static pqc_status_t ac_encaps(pqc_alg_t alg, const uint8_t *pk, uint8_t *ct, uint8_t *ss)
{
	uint8_t m[32];
	if (pqc_random_bytes(m, sizeof(m)) != 0) {
		return PQC_ERR_BACKEND;
	}
	pqc_status_t st = ac_encaps_derand(alg, pk, m, sizeof(m), ct, ss);
	pqc_secure_zero(m, sizeof(m));
	return st;
}

static pqc_status_t ac_decaps(pqc_alg_t alg, const uint8_t *sk, const uint8_t *ct, uint8_t *ss)
{
	const pqc_alg_info_t *info = pqc_alg_info(alg);
	if (!info || info->kind != PQC_KIND_KEM) {
		return PQC_ERR_BAD_ARG;
	}
	static uint8_t buf[ACCEL_BUF_MAX];
	memcpy(buf, sk, info->sk_len);
	memcpy(buf + info->sk_len, ct, info->ct_len);
	uint32_t out = 0;
	pqc_status_t st = run(ACCEL_MODE_KEM_DECAPS, alg, buf, info->sk_len + info->ct_len, &out);
	pqc_secure_zero(buf, info->sk_len);
	if (st != PQC_OK) {
		return st;
	}
	accel_get_transport()->read_data(0, ss, info->ss_len);
	return PQC_OK;
}

static pqc_status_t ac_sign(pqc_alg_t alg, const uint8_t *sk,
                            const uint8_t *msg, size_t msg_len,
                            const uint8_t *ctx, size_t ctx_len,
                            const uint8_t *rnd, uint8_t *sig, size_t *sig_len)
{
	const pqc_alg_info_t *info = pqc_alg_info(alg);
	if (!info || info->kind != PQC_KIND_SIG || ctx_len > 255) {
		return PQC_ERR_BAD_ARG;
	}
	size_t need = info->sk_len + 32 + 4 + ctx_len + msg_len;
	if (need > ACCEL_BUF_MAX) {
		return PQC_ERR_BAD_ARG;
	}
	static uint8_t buf[ACCEL_BUF_MAX];
	uint8_t *p = buf;
	memcpy(p, sk, info->sk_len);
	p += info->sk_len;
	/* rnd == NULL → 送全 0，桩/PL 侧约定为"自取 TRNG"（见 accel_stub.c） */
	if (rnd) {
		memcpy(p, rnd, 32);
	} else {
		memset(p, 0, 32);
	}
	p += 32;
	put_u32le(p, (uint32_t)ctx_len);
	p += 4;
	if (ctx_len) {
		memcpy(p, ctx, ctx_len);
		p += ctx_len;
	}
	if (msg_len) {
		memcpy(p, msg, msg_len);
	}
	uint32_t out = 0;
	pqc_status_t st = run(ACCEL_MODE_SIG_SIGN, alg, buf, need, &out);
	pqc_secure_zero(buf, info->sk_len);
	if (st != PQC_OK) {
		return st;
	}
	if (!sig_len || out > *sig_len) {
		return PQC_ERR_BAD_ARG;
	}
	accel_get_transport()->read_data(0, sig, out);
	*sig_len = out;
	return PQC_OK;
}

static pqc_status_t ac_verify(pqc_alg_t alg, const uint8_t *pk,
                              const uint8_t *msg, size_t msg_len,
                              const uint8_t *ctx, size_t ctx_len,
                              const uint8_t *sig, size_t sig_len)
{
	const pqc_alg_info_t *info = pqc_alg_info(alg);
	if (!info || info->kind != PQC_KIND_SIG || ctx_len > 255) {
		return PQC_ERR_BAD_ARG;
	}
	size_t need = info->pk_len + 4 + sig_len + 4 + ctx_len + msg_len;
	if (need > ACCEL_BUF_MAX) {
		return PQC_ERR_BAD_ARG;
	}
	static uint8_t buf[ACCEL_BUF_MAX];
	uint8_t *p = buf;
	memcpy(p, pk, info->pk_len);
	p += info->pk_len;
	put_u32le(p, (uint32_t)sig_len);
	p += 4;
	memcpy(p, sig, sig_len);
	p += sig_len;
	put_u32le(p, (uint32_t)ctx_len);
	p += 4;
	if (ctx_len) {
		memcpy(p, ctx, ctx_len);
		p += ctx_len;
	}
	if (msg_len) {
		memcpy(p, msg, msg_len);
	}
	uint32_t out = 0;
	pqc_status_t st = run(ACCEL_MODE_SIG_VERIFY, alg, buf, need, &out);
	if (st != PQC_OK) {
		return st;
	}
	/* 验签结果由 OUT_LEN 回传：1 = 通过 */
	return out ? PQC_OK : PQC_ERR_VERIFY;
}

int accel_ntt(const int16_t *in, int16_t *out, int inverse)
{
	if (!in || !out) {
		return -1;
	}
	uint8_t buf[512];
	memcpy(buf, in, 512);
	uint32_t olen = 0;
	pqc_status_t st = run(inverse ? ACCEL_MODE_NTT_INV : ACCEL_MODE_NTT_FWD,
	                      PQC_ALG_ML_KEM_768, buf, 512, &olen);
	if (st != PQC_OK || olen != 512) {
		return -1;
	}
	accel_get_transport()->read_data(0, (uint8_t *)out, 512);
	return 0;
}

static const pqc_backend_t g_accel = {
	.name              = "accel(register-interface)",
	.keypair           = ac_keypair,
	.keypair_from_seed = ac_keypair_from_seed,
	.encaps            = ac_encaps,
	.encaps_derand     = ac_encaps_derand,
	.decaps            = ac_decaps,
	.sign              = ac_sign,
	.verify            = ac_verify,
};

const pqc_backend_t *pqc_backend_accel(void)
{
	return &g_accel;
}
