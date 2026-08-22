/* pqc_accel.c —— 用寄存器接口实现 pqc_backend_t
 *
 * 这是"算法核搬到 FPGA"的那条缝的上半截：它只会写寄存器、送数据、轮询 DONE，
 * **完全不知道**下面接的是软件桩、Verilator 仿真还是真 PL。
 *
 * 与 pqc_backend_native() 可互换：
 *     pqc_set_backend(pqc_backend_accel());
 * 换完之后槽位管理器、密钥库、备份恢复、PKCS#11 全都照常工作 ——
 * tests/unit/test_accel.c 就是把整套 crypto 测试在这个后端下再跑一遍。
 */
#include "pqchsm/accel.h"
#include "pqchsm/util.h"

#include <pthread.h>
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
static pqc_status_t run_param(uint32_t mode, uint32_t param,
                              const uint8_t *in, size_t in_len, uint32_t *out_len)
{
	const accel_transport_t *t = accel_get_transport();
	if (!t || in_len > ACCEL_BUF_MAX) {
		return PQC_ERR_BAD_ARG;
	}
	t->write_reg(ACCEL_REG_MODE, mode);
	t->write_reg(ACCEL_REG_PARAM, param);
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

/* 绝大多数操作码的 PARAM 就是参数集本身 */
static pqc_status_t run(uint32_t mode, pqc_alg_t alg,
                        const uint8_t *in, size_t in_len, uint32_t *out_len)
{
	return run_param(mode, (uint32_t)alg, in, in_len, out_len);
}

static void put_u32le(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++) {
		p[i] = (uint8_t)(v >> (8 * i));
	}
}

/* ---- vtable ---- */

/* ============================================================================
 * 【后端锁：这一层与它下面的 transport 都有共享状态】
 * ============================================================================
 * 这里原来用**进程级 static 缓冲**装私钥，而且没有锁。两个线程
 * 同时调进来，一个的 sk 会盖掉另一个的 —— 不报错，只是算错，而且错的那份
 * 里带着别人的私钥。
 *
 * 两件事一起改：
 *   · 本层的暂存缓冲改成**栈上**（16 KB，进程里不再有常驻的私钥副本）；
 *   · 加一把后端锁 —— 因为 accel_axi.c 的 g_in/g_out 仍然是共享的 static
 *     （那是"一套硬件"的固有属性，不是这一层能消掉的）。缓冲改栈上而不加锁
 *     只解决一半，两个线程照样会在 transport 里交错。
 *
 * ⚠️ 锁的粒度是"一次完整的算子调用"（灌数据 → run → 读结果）。粒度再细就会
 *    在 run 与 read_data 之间放行另一个线程，而那正是要挡的交错。
 */
static pthread_mutex_t g_accel_lock = PTHREAD_MUTEX_INITIALIZER;

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
		pqc_secure_zero(seed, sizeof(seed));
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
	if (info->pk_len + 32 > ACCEL_BUF_MAX) {
		return PQC_ERR_BAD_ARG;
	}
	uint8_t buf[ACCEL_BUF_MAX];
	pthread_mutex_lock(&g_accel_lock);
	memcpy(buf, pk, info->pk_len);
	memcpy(buf + info->pk_len, m, 32);
	uint32_t out = 0;
	pqc_status_t st = run(ACCEL_MODE_KEM_ENCAPS, alg, buf, info->pk_len + 32, &out);
	/* ⚠️ m（封装随机数）是秘密，它在 buf 里。原来这条路**成功时直接 return**，
	 * 一次都没清过 —— 失败路径清了、成功路径没清，是最容易漏的那种。 */
	pqc_secure_zero(buf, sizeof(buf));
	if (st != PQC_OK) {
		pthread_mutex_unlock(&g_accel_lock);
		return st;
	}
	const accel_transport_t *t = accel_get_transport();
	t->read_data(0, ct, info->ct_len);
	t->read_data((uint32_t)info->ct_len, ss, info->ss_len);
	pthread_mutex_unlock(&g_accel_lock);
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
	if (info->sk_len + info->ct_len > ACCEL_BUF_MAX) {
		return PQC_ERR_BAD_ARG;
	}
	uint8_t buf[ACCEL_BUF_MAX];
	pthread_mutex_lock(&g_accel_lock);
	memcpy(buf, sk, info->sk_len);
	memcpy(buf + info->sk_len, ct, info->ct_len);
	uint32_t out = 0;
	pqc_status_t st = run(ACCEL_MODE_KEM_DECAPS, alg, buf, info->sk_len + info->ct_len, &out);
	pqc_secure_zero(buf, sizeof(buf));
	if (st != PQC_OK) {
		pthread_mutex_unlock(&g_accel_lock);
		return st;
	}
	accel_get_transport()->read_data(0, ss, info->ss_len);
	pthread_mutex_unlock(&g_accel_lock);
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
	uint8_t buf[ACCEL_BUF_MAX];
	pthread_mutex_lock(&g_accel_lock);
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
	pqc_secure_zero(buf, sizeof(buf));
	if (st != PQC_OK) {
		pthread_mutex_unlock(&g_accel_lock);
		return st;
	}
	if (!sig_len || out > *sig_len) {
		pthread_mutex_unlock(&g_accel_lock);
		return PQC_ERR_BAD_ARG;
	}
	accel_get_transport()->read_data(0, sig, out);
	*sig_len = out;
	pthread_mutex_unlock(&g_accel_lock);
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
	uint8_t buf[ACCEL_BUF_MAX];
	pthread_mutex_lock(&g_accel_lock);
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
	/* 验签的输入里没有私钥，但仍然清一遍：这块栈会被下一次调用复用，
	 * 而下一次可能是 sign（那里有 sk）。清零成本可以忽略。 */
	pqc_secure_zero(buf, sizeof(buf));
	pthread_mutex_unlock(&g_accel_lock);
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

int accel_keccak_f1600(const uint8_t state_in[200], uint8_t state_out[200])
{
	if (!state_in || !state_out) {
		return -1;
	}
	uint8_t buf[200];
	memcpy(buf, state_in, 200);
	uint32_t olen = 0;
	pqc_status_t st = run(ACCEL_MODE_KECCAK_F1600, PQC_ALG_ML_KEM_768,
	                      buf, 200, &olen);
	if (st != PQC_OK || olen != 200) {
		return -1;
	}
	accel_get_transport()->read_data(0, state_out, 200);
	return 0;
}

/* 整条海绵委托给 PL（模式 10）。
 *
 * 成功返回 0；该 transport 没实现模式 10 时返回 1（调用方退回软件海绵）；
 * 真出错返回 -1。**把"没实现"与"出错"分开**是有意的：前者要静默回落，
 * 后者绝不能被回落盖住 —— 一块坏了的加速器如果每次都被软件兜住，
 * 就再也没人会发现它坏了。 */
static int shake_in_pl(int rate, uint8_t suffix,
                       const uint8_t *msg, size_t msg_len,
                       uint8_t *out, size_t out_len)
{
	if (out_len == 0 || msg_len > ACCEL_SHAKE_MAX || out_len > ACCEL_SHAKE_MAX) {
		return 1;
	}
	uint32_t olen = 0;
	uint32_t param = ACCEL_SHAKE_PARAM((uint32_t)rate, suffix, (uint32_t)out_len);
	pqc_status_t st = run_param(ACCEL_MODE_SHAKE, param, msg, msg_len, &olen);
	if (st == PQC_ERR_UNSUPPORTED) {
		return 1;
	}
	if (st != PQC_OK || olen != out_len) {
		return -1;
	}
	accel_get_transport()->read_data(0, out, out_len);
	return 0;
}

/* SHAKE / SHA3。优先整条委托给 PL，退不回来才在 C 侧做 framing。
 * 两条路的分工与回落理由见 accel.h。
 *
 * C 侧这条路径把 padding（pad10*1 + 域分隔后缀）、rate、lane 小端序、
 * 多块吸收与多块挤压全都串起来 —— 换句话说，它验的不只是置换本身。
 * 正确性由 tests/unit/test_accel.c 对 OpenSSL 的逐字节比对钉住。 */
int accel_shake(int rate, uint8_t suffix,
                const uint8_t *msg, size_t msg_len,
                uint8_t *out, size_t out_len)
{
	if (rate <= 0 || rate > 200 || rate % 8 != 0 || (!msg && msg_len) || !out) {
		return -1;
	}
	int hw = shake_in_pl(rate, suffix, msg, msg_len, out, out_len);
	if (hw <= 0) {
		return hw;               /* 0 = 硬件算完了；-1 = 硬件真出错 */
	}
	uint8_t state[200];
	memset(state, 0, sizeof(state));

	size_t off = 0;
	/* 吸收整块 */
	for (; msg_len - off >= (size_t)rate; off += (size_t)rate) {
		for (int i = 0; i < rate; i++) {
			state[i] ^= msg[off + i];
		}
		if (accel_keccak_f1600(state, state) != 0) {
			return -1;
		}
	}
	/* 末块 + pad10*1 */
	uint8_t last[200];
	memset(last, 0, sizeof(last));
	memcpy(last, msg + off, msg_len - off);
	last[msg_len - off] = suffix;
	last[rate - 1] ^= 0x80;
	for (int i = 0; i < rate; i++) {
		state[i] ^= last[i];
	}
	pqc_secure_zero(last, sizeof(last));
	if (accel_keccak_f1600(state, state) != 0) {
		return -1;
	}

	/* 挤压 */
	size_t done = 0;
	for (;;) {
		size_t n = out_len - done < (size_t)rate ? out_len - done : (size_t)rate;
		memcpy(out + done, state, n);
		done += n;
		if (done >= out_len) {
			break;
		}
		if (accel_keccak_f1600(state, state) != 0) {
			return -1;
		}
	}
	pqc_secure_zero(state, sizeof(state));
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
