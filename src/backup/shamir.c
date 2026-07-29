#include "pqchsm/shamir.h"
#include "pqchsm/kdf.h"
#include "pqchsm/util.h"

#include <openssl/rand.h>
#include <string.h>

/* 校验字节数，包含在 SHAMIR_SHARE_OVERHEAD 里 */
#define SHAMIR_CHECKSUM_LEN 4
/* index(1) + secret_len(1) */
#define SHAMIR_HDR_LEN 2

/* ---------------------------------------------------------------- GF(256) */
/* 既约多项式 x^8+x^4+x^3+x+1 = 0x11B，低 8 位是 0x1B。
 *
 * 这里坚持位运算的俄罗斯农民乘法，而不是 log/antilog 查表：
 * 表法要用秘密数据当下标，会在 D-cache 上留下时序痕迹（§8.4 要求常量时间插值）。
 * 下面 8 轮循环次数固定、无分支、无数据相关内存访问，
 * 系数选择全部靠掩码（0x00 / 0xFF）完成。 */
static uint8_t gf_mul(uint8_t a, uint8_t b)
{
	uint8_t r = 0;
	uint8_t x = a;
	uint8_t y = b;

	for (int i = 0; i < 8; i++) {
		/* y 的最低位为 1 时 mask = 0xFF，否则 0x00 */
		uint8_t mask = (uint8_t)(0u - (unsigned)(y & 1u));
		r = (uint8_t)(r ^ (x & mask));

		/* x 的最高位为 1 时 hmask = 0xFF，左移后需要模约简 */
		uint8_t hmask = (uint8_t)(0u - (unsigned)((x >> 7) & 1u));
		x = (uint8_t)(x << 1);
		x = (uint8_t)(x ^ (0x1bu & hmask));

		y = (uint8_t)(y >> 1);
	}
	return r;
}

static uint8_t gf_sqr(uint8_t a)
{
	return gf_mul(a, a);
}

/* 乘法逆元：a^254 = a^-1（GF(256)* 的阶是 255）。
 * 用固定加法链而不是扩展欧几里得 —— 链长与 a 无关，天然常量时间。
 * 约定 gf_inv(0) = 0（0^254 = 0），调用方必须保证不会传 0。 */
static uint8_t gf_inv(uint8_t a)
{
	uint8_t x2   = gf_sqr(a);            /* a^2   */
	uint8_t x3   = gf_mul(x2, a);        /* a^3   */
	uint8_t x6   = gf_sqr(x3);           /* a^6   */
	uint8_t x7   = gf_mul(x6, a);        /* a^7   */
	uint8_t x14  = gf_sqr(x7);           /* a^14  */
	uint8_t x15  = gf_mul(x14, a);       /* a^15  */
	uint8_t x30  = gf_sqr(x15);          /* a^30  */
	uint8_t x31  = gf_mul(x30, a);       /* a^31  */
	uint8_t x62  = gf_sqr(x31);          /* a^62  */
	uint8_t x63  = gf_mul(x62, a);       /* a^63  */
	uint8_t x126 = gf_sqr(x63);          /* a^126 */
	uint8_t x127 = gf_mul(x126, a);      /* a^127 */

	return gf_sqr(x127);                 /* a^254 */
}

/* ------------------------------------------------------------------ 校验和 */
/* checksum = SHA3-256(index || secret_len || data) 的前 4 字节。
 * line 的前 SHAMIR_HDR_LEN + secret_len 字节正好就是这三段，直接整段哈希。 */
static int share_checksum(const uint8_t *line, size_t secret_len,
                          uint8_t out[SHAMIR_CHECKSUM_LEN])
{
	uint8_t h[32];

	if (pqc_sha3_256(line, SHAMIR_HDR_LEN + secret_len, h) != 0) {
		return -1;
	}
	memcpy(out, h, SHAMIR_CHECKSUM_LEN);
	pqc_secure_zero(h, sizeof(h));
	return 0;
}

/* -------------------------------------------------------------------- 拆分 */
int shamir_split(const uint8_t *secret, size_t secret_len,
                 uint8_t m, uint8_t n,
                 uint8_t *shares, size_t share_cap, size_t *share_lens)
{
	/* coef[d][j]：第 j 个字节那条多项式的 d 次系数。
	 * d 最大 m-1 <= SHAMIR_MAX_SHARES-1，所以 SHAMIR_MAX_SHARES 行够用。 */
	uint8_t coef[SHAMIR_MAX_SHARES][SHAMIR_MAX_SECRET];
	size_t line_len;
	int rc = -1;

	if (!secret || !shares || !share_lens) {
		return -1;
	}
	if (secret_len == 0 || secret_len > SHAMIR_MAX_SECRET) {
		return -1;
	}
	if (m < 2 || n < m || n > SHAMIR_MAX_SHARES) {
		return -1;
	}
	line_len = secret_len + SHAMIR_SHARE_OVERHEAD;
	if (share_cap < line_len) {
		return -1;
	}

	memset(coef, 0, sizeof(coef));
	for (size_t j = 0; j < secret_len; j++) {
		coef[0][j] = secret[j];   /* 常数项 = 秘密字节，即 f(0) */
	}
	/* 1..m-1 次系数取随机；随机数拿不到就整体失败，绝不退化成弱多项式 */
	for (uint8_t d = 1; d < m; d++) {
		if (RAND_bytes(coef[d], (int)secret_len) != 1) {
			goto fail;
		}
	}

	for (uint8_t i = 0; i < n; i++) {
		uint8_t *line = shares + (size_t)i * share_cap;
		uint8_t x = (uint8_t)(i + 1);   /* x 从 1 开始，x=0 就是秘密本身 */

		line[0] = x;
		line[1] = (uint8_t)secret_len;
		for (size_t j = 0; j < secret_len; j++) {
			/* Horner：acc = ((a_{m-1} * x + a_{m-2}) * x + ...) + a_0 */
			uint8_t acc = coef[m - 1][j];
			for (int d = (int)m - 2; d >= 0; d--) {
				acc = (uint8_t)(gf_mul(acc, x) ^ coef[d][j]);
			}
			line[SHAMIR_HDR_LEN + j] = acc;
		}
		if (share_checksum(line, secret_len,
		                   line + SHAMIR_HDR_LEN + secret_len) != 0) {
			goto fail;
		}
		share_lens[i] = line_len;
	}

	rc = 0;
	goto out;

fail:
	/* 半成品分片也是秘密相关数据，不留在调用方缓冲里 */
	for (uint8_t i = 0; i < n; i++) {
		pqc_secure_zero(shares + (size_t)i * share_cap, share_cap);
		share_lens[i] = 0;
	}
out:
	pqc_secure_zero(coef, sizeof(coef));
	return rc;
}

/* -------------------------------------------------------------------- 恢复 */
int shamir_combine(const uint8_t *shares, size_t share_cap, const size_t *share_lens,
                   uint8_t k, uint8_t *out, size_t out_cap, size_t *out_len)
{
	uint8_t xs[SHAMIR_MAX_SHARES];
	uint8_t basis[SHAMIR_MAX_SHARES];
	size_t secret_len = 0;
	int rc = -1;

	if (!shares || !share_lens || !out || !out_len) {
		return -1;
	}
	if (k < 1 || k > SHAMIR_MAX_SHARES) {
		return -1;
	}
	if (share_cap < SHAMIR_SHARE_OVERHEAD + 1) {
		return -1;
	}

	memset(xs, 0, sizeof(xs));
	memset(basis, 0, sizeof(basis));

	/* 第一步：逐片校验。任何一片有问题都立刻收工，绝不带坏数据进插值。 */
	for (uint8_t i = 0; i < k; i++) {
		const uint8_t *line = shares + (size_t)i * share_cap;
		uint8_t sum[SHAMIR_CHECKSUM_LEN];
		size_t len;

		if (share_lens[i] > share_cap || share_lens[i] <= SHAMIR_SHARE_OVERHEAD) {
			goto out;
		}
		len = share_lens[i] - SHAMIR_SHARE_OVERHEAD;
		if (len > SHAMIR_MAX_SECRET) {
			goto out;
		}
		/* 长度字段必须和调用方给的行长自洽 */
		if ((size_t)line[1] != len) {
			goto out;
		}
		if (i == 0) {
			secret_len = len;
		} else if (len != secret_len) {
			goto out;   /* 不是同一次 split 出来的分片 */
		}
		/* 索引为 0 意味着这片直接就是秘密，永远不该出现 */
		if (line[0] == 0 || line[0] > SHAMIR_MAX_SHARES) {
			goto out;
		}
		if (share_checksum(line, secret_len, sum) != 0) {
			goto out;
		}
		if (!pqc_ct_equal(sum, line + SHAMIR_HDR_LEN + secret_len,
		                  SHAMIR_CHECKSUM_LEN)) {
			goto out;   /* 分片被篡改 */
		}
		xs[i] = line[0];
	}

	/* 索引必须两两不同，否则拉格朗日分母为 0（同一个点给不出两条约束） */
	for (uint8_t i = 0; i < k; i++) {
		for (uint8_t t = (uint8_t)(i + 1); t < k; t++) {
			if (xs[i] == xs[t]) {
				goto out;
			}
		}
	}

	if (out_cap < secret_len) {
		goto out;
	}

	/* 第二步：算 x=0 处的拉格朗日基 —— 只跟公开的索引有关，与秘密无关。
	 * L_i(0) = prod_{t!=i} (0 - x_t) / (x_i - x_t)，
	 * GF(2^k) 上加减同为 XOR，所以 = prod_{t!=i} x_t / (x_i ^ x_t)。 */
	for (uint8_t i = 0; i < k; i++) {
		uint8_t num = 1, den = 1;
		for (uint8_t t = 0; t < k; t++) {
			if (t == i) {
				continue;
			}
			num = gf_mul(num, xs[t]);
			den = gf_mul(den, (uint8_t)(xs[i] ^ xs[t]));
		}
		basis[i] = gf_mul(num, gf_inv(den));   /* den != 0：索引已去重 */
	}

	/* 第三步：逐字节插值。乘法常量时间，累加只是 XOR，无分支。 */
	for (size_t j = 0; j < secret_len; j++) {
		uint8_t acc = 0;
		for (uint8_t i = 0; i < k; i++) {
			const uint8_t *line = shares + (size_t)i * share_cap;
			acc = (uint8_t)(acc ^ gf_mul(line[SHAMIR_HDR_LEN + j], basis[i]));
		}
		out[j] = acc;
		pqc_secure_zero(&acc, sizeof(acc));
	}

	*out_len = secret_len;
	rc = 0;

out:
	pqc_secure_zero(basis, sizeof(basis));
	pqc_secure_zero(xs, sizeof(xs));
	return rc;
}
