/* accel_stub.c —— 软件桩加速器
 *
 * 暴露与真 PL **完全相同**的寄存器语义：
 *     写 MODE / PARAM / IN_LEN → 送数据 → 写 CTRL.START → 轮询 STATUS.DONE → 读数据
 * 内部调 liboqs 完成实际运算。
 *
 * 真板到手后把本文件换成 accel_mmap.c（/dev/mem + mmap），
 * pqc_accel.c 与其上的一切一行不改。
 */
#include "pqchsm/accel.h"
#include "pqchsm/util.h"

#include <string.h>

static uint32_t g_regs[16];
static uint8_t  g_buf[ACCEL_BUF_MAX];

/* 真 PL 上这些是 BRAM/DMA；这里就是一块内存 */
static void stub_write_data(uint32_t off, const uint8_t *src, size_t n)
{
	if (off < ACCEL_BUF_MAX && n <= ACCEL_BUF_MAX - off) {
		memcpy(g_buf + off, src, n);
	}
}

static void stub_read_data(uint32_t off, uint8_t *dst, size_t n)
{
	if (off < ACCEL_BUF_MAX && n <= ACCEL_BUF_MAX - off) {
		memcpy(dst, g_buf + off, n);
	}
}

static uint32_t stub_read_reg(uint32_t off)
{
	return (off / 4 < 16) ? g_regs[off / 4] : 0;
}

static void set_err(uint32_t code)
{
	g_regs[ACCEL_REG_ERRCODE / 4] = code;
	g_regs[ACCEL_REG_STATUS / 4] = ACCEL_ST_DONE | ACCEL_ST_ERR;
}

static void set_done(uint32_t out_len)
{
	g_regs[ACCEL_REG_OUT_LEN / 4] = out_len;
	g_regs[ACCEL_REG_STATUS / 4] = ACCEL_ST_DONE;
}

/* NTT：桩里用 Python 参考模型同款的软件实现（与 hardware/rtl/ 对拍用的是同一套定义） */
#define NTT_Q 3329
#define NTT_QINV (-3327)

static const int16_t NTT_ZETAS[128] = {
	-1044, -758, -359, -1517, 1493, 1422, 287, 202, -171, 622, 1577, 182, 962,
	-1202, -1474, 1468, 573, -1325, 264, 383, -829, 1458, -1602, -130, -681,
	1017, 732, 608, -1542, 411, -205, -1571, 1223, 652, -552, 1015, -1293, 1491,
	-282, -1544, 516, -8, -320, -666, -1618, -1162, 126, 1469, -853, -90, -271,
	830, 107, -1421, -247, -951, -398, 961, -1508, -725, 448, -1065, 677, -1275,
	-1103, 430, 555, 843, -1251, 871, 1550, 105, 422, 587, 177, -235, -291, -460,
	1574, 1653, -246, 778, 1159, -147, -777, 1483, -602, 1119, -1590, 644, -872,
	349, 418, 329, -156, -75, 817, 1097, 603, 610, 1322, -1285, -1465, 384, -1215,
	-136, 1218, -1335, -874, 220, -1187, -1659, -1185, -1530, -1278, 794, -1510,
	-854, -870, 478, -108, -308, 996, 991, 958, -1460, 1522, 1628,
};

static int16_t mont_reduce(int32_t a)
{
	int16_t t = (int16_t)((int16_t)a * NTT_QINV);
	return (int16_t)((a - (int32_t)t * NTT_Q) >> 16);
}

static int16_t barrett(int16_t a)
{
	const int16_t v = 20159;
	int16_t t = (int16_t)(((int32_t)v * a + (1 << 25)) >> 26);
	return (int16_t)(a - t * NTT_Q);
}

static void ntt_fwd(int16_t *r)
{
	unsigned k = 1;
	for (unsigned len = 128; len >= 2; len >>= 1) {
		for (unsigned start = 0; start < 256; start += 2 * len) {
			int16_t zeta = NTT_ZETAS[k++];
			for (unsigned j = start; j < start + len; j++) {
				int16_t t = mont_reduce((int32_t)zeta * r[j + len]);
				r[j + len] = (int16_t)(r[j] - t);
				r[j] = (int16_t)(r[j] + t);
			}
		}
	}
	for (unsigned j = 0; j < 256; j++) {
		r[j] = barrett(r[j]);
	}
}

static void ntt_inv(int16_t *r)
{
	const int16_t f = 1441;
	unsigned k = 127;
	for (unsigned len = 2; len <= 128; len <<= 1) {
		for (unsigned start = 0; start < 256; start += 2 * len) {
			int16_t zeta = NTT_ZETAS[k--];
			for (unsigned j = start; j < start + len; j++) {
				int16_t t = r[j];
				r[j] = barrett((int16_t)(t + r[j + len]));
				r[j + len] = (int16_t)(r[j + len] - t);
				r[j + len] = mont_reduce((int32_t)zeta * r[j + len]);
			}
		}
	}
	for (unsigned j = 0; j < 256; j++) {
		r[j] = mont_reduce((int32_t)f * r[j]);
	}
}

/* Keccak-f[1600] 的软件实现（桩用）。与 hardware/rtl/keccak/keccak_f1600.v 同一套定义，
 * 二者的一致性由 test_accel 的"RTL 与桩逐字节相同"断言保证。 */
static void keccak_f1600_sw(uint64_t A[25])
{
	static const uint64_t RC[24] = {
		0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808AULL,
		0x8000000080008000ULL, 0x000000000000808BULL, 0x0000000080000001ULL,
		0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008AULL,
		0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000AULL,
		0x000000008000808BULL, 0x800000000000008BULL, 0x8000000000008089ULL,
		0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
		0x000000000000800AULL, 0x800000008000000AULL, 0x8000000080008081ULL,
		0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL,
	};
	static const int R[5][5] = {
		{ 0, 36,  3, 41, 18 }, { 1, 44, 10, 45,  2 }, { 62, 6, 43, 15, 61 },
		{ 28, 55, 25, 21, 56 }, { 27, 20, 39,  8, 14 },
	};
	for (int round = 0; round < 24; round++) {
		uint64_t C[5], D[5], B[25];
		for (int x = 0; x < 5; x++) {
			C[x] = A[x] ^ A[x + 5] ^ A[x + 10] ^ A[x + 15] ^ A[x + 20];
		}
		for (int x = 0; x < 5; x++) {
			uint64_t t = C[(x + 1) % 5];
			D[x] = C[(x + 4) % 5] ^ ((t << 1) | (t >> 63));
		}
		for (int x = 0; x < 5; x++) {
			for (int y = 0; y < 5; y++) {
				A[x + 5 * y] ^= D[x];
			}
		}
		for (int x = 0; x < 5; x++) {
			for (int y = 0; y < 5; y++) {
				int n = R[x][y];
				uint64_t v = A[x + 5 * y];
				B[y + 5 * ((2 * x + 3 * y) % 5)] = n ? ((v << n) | (v >> (64 - n))) : v;
			}
		}
		for (int x = 0; x < 5; x++) {
			for (int y = 0; y < 5; y++) {
				A[x + 5 * y] = B[x + 5 * y]
				             ^ ((~B[((x + 1) % 5) + 5 * y]) & B[((x + 2) % 5) + 5 * y]);
			}
		}
		A[0] ^= RC[round];
	}
}

/* 写 CTRL.START 时真正干活 —— 与真 PL 的行为一致：命令是"边沿触发"的 */
static void stub_write_reg(uint32_t off, uint32_t val)
{
	if (off / 4 >= 16) {
		return;
	}
	g_regs[off / 4] = val;
	if (off != ACCEL_REG_CTRL) {
		return;
	}
	if (val & ACCEL_CTRL_SOFT_RESET) {
		memset(g_regs, 0, sizeof(g_regs));
		pqc_secure_zero(g_buf, sizeof(g_buf));
		return;
	}
	if (!(val & ACCEL_CTRL_START)) {
		return;
	}

	uint32_t mode = g_regs[ACCEL_REG_MODE / 4];
	pqc_alg_t alg = (pqc_alg_t)g_regs[ACCEL_REG_PARAM / 4];
	uint32_t in_len = g_regs[ACCEL_REG_IN_LEN / 4];
	const pqc_alg_info_t *info = pqc_alg_info(alg);
	g_regs[ACCEL_REG_STATUS / 4] = ACCEL_ST_BUSY;

	switch (mode) {
	case ACCEL_MODE_KEM_KEYGEN:
	case ACCEL_MODE_SIG_KEYGEN: {
		if (!info || in_len != info->seed_len) {
			set_err(1);
			return;
		}
		uint8_t seed[64];
		memcpy(seed, g_buf, in_len);
		/* 桩内部直接调软件后端 —— 注意用 liboqs 后端而不是当前后端，
		 * 否则会自己调自己形成递归。 */
		const pqc_backend_t *sw = pqc_backend_liboqs();
		if (sw->keypair_from_seed(alg, seed, in_len, g_buf, g_buf + info->pk_len) != PQC_OK) {
			pqc_secure_zero(seed, sizeof(seed));
			set_err(2);
			return;
		}
		pqc_secure_zero(seed, sizeof(seed));
		set_done((uint32_t)(info->pk_len + info->sk_len));
		return;
	}
	case ACCEL_MODE_KEM_ENCAPS: {
		if (!info || in_len != info->pk_len + 32) {
			set_err(1);
			return;
		}
		uint8_t pk[2048], m[32];
		memcpy(pk, g_buf, info->pk_len);
		memcpy(m, g_buf + info->pk_len, 32);
		const pqc_backend_t *sw = pqc_backend_liboqs();
		if (sw->encaps_derand(alg, pk, m, 32, g_buf, g_buf + info->ct_len) != PQC_OK) {
			set_err(2);
			return;
		}
		set_done((uint32_t)(info->ct_len + info->ss_len));
		return;
	}
	case ACCEL_MODE_KEM_DECAPS: {
		if (!info || in_len != info->sk_len + info->ct_len) {
			set_err(1);
			return;
		}
		uint8_t sk[4096], ct[2048];
		memcpy(sk, g_buf, info->sk_len);
		memcpy(ct, g_buf + info->sk_len, info->ct_len);
		const pqc_backend_t *sw = pqc_backend_liboqs();
		int rc = sw->decaps(alg, sk, ct, g_buf) != PQC_OK;
		pqc_secure_zero(sk, sizeof(sk));
		if (rc) {
			set_err(2);
			return;
		}
		set_done((uint32_t)info->ss_len);
		return;
	}
	case ACCEL_MODE_SIG_SIGN: {
		/* 布局：sk ‖ rnd(32) ‖ ctx_len(4) ‖ ctx ‖ msg */
		if (!info || in_len < info->sk_len + 32 + 4) {
			set_err(1);
			return;
		}
		const uint8_t *p = g_buf;
		static uint8_t sk[4096];
		memcpy(sk, p, info->sk_len);
		p += info->sk_len;
		const uint8_t *rnd = p;
		p += 32;
		uint32_t ctx_len = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		                   ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
		p += 4;
		if (ctx_len > 255 || (size_t)(p - g_buf) + ctx_len > in_len) {
			/* sk 已经拷进本地缓冲了：任何提前返回都得先把它抹掉 */
			pqc_secure_zero(sk, sizeof(sk));
			set_err(1);
			return;
		}
		const uint8_t *ctx = p;
		p += ctx_len;
		size_t msg_len = in_len - (size_t)(p - g_buf);
		static uint8_t msg[ACCEL_BUF_MAX];
		memcpy(msg, p, msg_len);
		size_t sig_len = info->sig_len;
		const pqc_backend_t *sw = pqc_backend_liboqs();
		/* rnd 全 0 视为"让后端自取 TRNG"，与 pqc_sign(rnd=NULL) 语义一致：
		 * 寄存器接口里没法传 NULL，只能约定一个哨兵。
		 *
		 * 哨兵判定必须无分支。rnd 是签名随机化输入，属于秘密数据；
		 * 逐字节 `if (rnd[z])` 会把"哪一个字节先非零"泄漏成时序差异。
		 * 按位或起来再比一次，循环次数固定，没有数据相关跳转。 */
		uint8_t rnd_or = 0;
		for (int z = 0; z < 32; z++) {
			rnd_or |= rnd[z];
		}
		int all_zero = (rnd_or == 0);
		pqc_status_t st = sw->sign(alg, sk, msg, msg_len, ctx_len ? ctx : NULL, ctx_len,
		                           all_zero ? NULL : rnd, g_buf, &sig_len);
		pqc_secure_zero(sk, sizeof(sk));
		if (st != PQC_OK) {
			set_err(2);
			return;
		}
		set_done((uint32_t)sig_len);
		return;
	}
	case ACCEL_MODE_SIG_VERIFY: {
		/* 布局：pk ‖ sig_len(4) ‖ sig ‖ ctx_len(4) ‖ ctx ‖ msg */
		if (!info || in_len < info->pk_len + 8) {
			set_err(1);
			return;
		}
		const uint8_t *p = g_buf;
		const uint8_t *pk = p;
		p += info->pk_len;
		uint32_t sig_len = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		                   ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
		p += 4;
		if (sig_len > info->sig_len || (size_t)(p - g_buf) + sig_len + 4 > in_len) {
			set_err(1);
			return;
		}
		const uint8_t *sig = p;
		p += sig_len;
		uint32_t ctx_len = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		                   ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
		p += 4;
		if (ctx_len > 255 || (size_t)(p - g_buf) + ctx_len > in_len) {
			set_err(1);
			return;
		}
		const uint8_t *ctx = p;
		p += ctx_len;
		size_t msg_len = in_len - (size_t)(p - g_buf);
		const pqc_backend_t *sw = pqc_backend_liboqs();
		pqc_status_t st = sw->verify(alg, pk, p, msg_len, ctx_len ? ctx : NULL, ctx_len,
		                             sig, sig_len);
		/* 验签不过是"结果"，用 OUT_LEN 回传：1 = 通过，0 = 不通过 */
		g_regs[ACCEL_REG_ERRCODE / 4] = 0;
		g_regs[ACCEL_REG_OUT_LEN / 4] = (st == PQC_OK) ? 1u : 0u;
		g_regs[ACCEL_REG_STATUS / 4] = ACCEL_ST_DONE;
		return;
	}
	case ACCEL_MODE_KECCAK_F1600: {
		if (in_len != 200) {
			set_err(1);
			return;
		}
		uint64_t st[25];
		for (int i = 0; i < 25; i++) {
			uint64_t v = 0;
			for (int b = 0; b < 8; b++) {
				v |= (uint64_t)g_buf[i * 8 + b] << (8 * b);
			}
			st[i] = v;
		}
		keccak_f1600_sw(st);
		for (int i = 0; i < 25; i++) {
			for (int b = 0; b < 8; b++) {
				g_buf[i * 8 + b] = (uint8_t)(st[i] >> (8 * b));
			}
		}
		set_done(200);
		return;
	}
	case ACCEL_MODE_NTT_FWD:
	case ACCEL_MODE_NTT_INV: {
		if (in_len != 512) {
			set_err(1);
			return;
		}
		int16_t poly[256];
		memcpy(poly, g_buf, 512);
		if (mode == ACCEL_MODE_NTT_FWD) {
			ntt_fwd(poly);
		} else {
			ntt_inv(poly);
		}
		memcpy(g_buf, poly, 512);
		set_done(512);
		return;
	}
	default:
		set_err(3);
		return;
	}
}

static int stub_reset(void)
{
	memset(g_regs, 0, sizeof(g_regs));
	pqc_secure_zero(g_buf, sizeof(g_buf));
	return 0;
}

static const accel_transport_t g_stub = {
	.name        = "stub(software, liboqs inside)",
	.is_hardware = 0,
	.reset       = stub_reset,
	.write_reg   = stub_write_reg,
	.read_reg    = stub_read_reg,
	.write_data  = stub_write_data,
	.read_data   = stub_read_data,
};

const accel_transport_t *accel_transport_stub(void)
{
	return &g_stub;
}
