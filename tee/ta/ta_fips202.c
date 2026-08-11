#include "ta_fips202.h"

#include <string.h>

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "ta_fips202 的海绵实现假设小端字节序"
#endif

/* ---- Keccak-f[1600]（轮常数/旋转表取自 FIPS 202，实现结构同 tiny_sha3）---- */

static const uint64_t keccakf_rndc[24] = {
	0x0000000000000001ULL, 0x0000000000008082ULL,
	0x800000000000808aULL, 0x8000000080008000ULL,
	0x000000000000808bULL, 0x0000000080000001ULL,
	0x8000000080008081ULL, 0x8000000000008009ULL,
	0x000000000000008aULL, 0x0000000000000088ULL,
	0x0000000080008009ULL, 0x000000008000000aULL,
	0x000000008000808bULL, 0x800000000000008bULL,
	0x8000000000008089ULL, 0x8000000000008003ULL,
	0x8000000000008002ULL, 0x8000000000000080ULL,
	0x000000000000800aULL, 0x800000008000000aULL,
	0x8000000080008081ULL, 0x8000000000008080ULL,
	0x0000000080000001ULL, 0x8000000080008008ULL,
};

static const unsigned keccakf_rotc[24] = {
	1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14,
	27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44,
};

static const unsigned keccakf_piln[24] = {
	10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4,
	15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1,
};

static uint64_t rotl64(uint64_t x, unsigned n)
{
	return (x << n) | (x >> (64 - n));
}

static void keccakf(uint64_t st[25])
{
	int      i, j, r;
	uint64_t t, bc[5];

	for (r = 0; r < 24; r++) {
		/* Theta */
		for (i = 0; i < 5; i++)
			bc[i] = st[i] ^ st[i + 5] ^ st[i + 10] ^ st[i + 15] ^
			        st[i + 20];
		for (i = 0; i < 5; i++) {
			t = bc[(i + 4) % 5] ^ rotl64(bc[(i + 1) % 5], 1);
			for (j = 0; j < 25; j += 5)
				st[j + i] ^= t;
		}
		/* Rho + Pi */
		t = st[1];
		for (i = 0; i < 24; i++) {
			j     = (int)keccakf_piln[i];
			bc[0] = st[j];
			st[j] = rotl64(t, keccakf_rotc[i]);
			t     = bc[0];
		}
		/* Chi */
		for (j = 0; j < 25; j += 5) {
			for (i = 0; i < 5; i++)
				bc[i] = st[j + i];
			for (i = 0; i < 5; i++)
				st[j + i] ^= (~bc[(i + 1) % 5]) & bc[(i + 2) % 5];
		}
		/* Iota */
		st[0] ^= keccakf_rndc[r];
	}
}

/* ---- 海绵 ---- */

void pqchsm_sponge_init(pqchsm_sponge_t *s, size_t rate)
{
	memset(s->st, 0, sizeof(s->st));
	s->pos  = 0;
	s->rate = rate;
}

void pqchsm_sponge_absorb(pqchsm_sponge_t *s, const uint8_t *in, size_t len)
{
	uint8_t *sb = (uint8_t *)s->st;

	while (len--) {
		sb[s->pos++] ^= *in++;
		if (s->pos == s->rate) {
			keccakf(s->st);
			s->pos = 0;
		}
	}
}

void pqchsm_sponge_absorb_zeros(pqchsm_sponge_t *s, size_t len)
{
	/* XOR 0 不改变状态，只需推进 pos 并按块置换 */
	while (len--) {
		s->pos++;
		if (s->pos == s->rate) {
			keccakf(s->st);
			s->pos = 0;
		}
	}
}

void pqchsm_sponge_pad(pqchsm_sponge_t *s, uint8_t domain)
{
	uint8_t *sb = (uint8_t *)s->st;

	sb[s->pos] ^= domain;
	sb[s->rate - 1] ^= 0x80;
	keccakf(s->st);
	s->pos = 0;
}

void pqchsm_sponge_squeeze(pqchsm_sponge_t *s, uint8_t *out, size_t len)
{
	const uint8_t *sb = (const uint8_t *)s->st;

	while (len--) {
		if (s->pos == s->rate) {
			keccakf(s->st);
			s->pos = 0;
		}
		*out++ = sb[s->pos++];
	}
}

void pqchsm_xof(size_t rate, uint8_t domain,
                const uint8_t *in, size_t in_len,
                uint8_t *out, size_t out_len)
{
	pqchsm_sponge_t s;

	pqchsm_sponge_init(&s, rate);
	if (in_len)
		pqchsm_sponge_absorb(&s, in, in_len);
	pqchsm_sponge_pad(&s, domain);
	pqchsm_sponge_squeeze(&s, out, out_len);
	pqchsm_bzero(&s, sizeof(s));
}

/* ---- SP 800-185 ---- */

static uint8_t enc_nbytes(uint64_t x)
{
	uint8_t n = 1;
	while (x >>= 8)
		n++;
	return n;
}

size_t pqchsm_left_encode(uint64_t x, uint8_t *out)
{
	uint8_t n = enc_nbytes(x);
	uint8_t i;

	out[0] = n;
	for (i = 0; i < n; i++)
		out[1 + i] = (uint8_t)(x >> (8 * (n - 1 - i)));
	return (size_t)n + 1;
}

size_t pqchsm_right_encode(uint64_t x, uint8_t *out)
{
	uint8_t n = enc_nbytes(x);
	uint8_t i;

	for (i = 0; i < n; i++)
		out[i] = (uint8_t)(x >> (8 * (n - 1 - i)));
	out[n] = n;
	return (size_t)n + 1;
}

/* encode_string(S) = left_encode(|S|*8) || S，直接吸进海绵并累计字节数 */
static size_t absorb_encode_string(pqchsm_sponge_t *s,
                                   const uint8_t *str, size_t len)
{
	uint8_t enc[9];
	size_t  l = pqchsm_left_encode((uint64_t)len * 8, enc);

	pqchsm_sponge_absorb(s, enc, l);
	if (len)
		pqchsm_sponge_absorb(s, str, len);
	return l + len;
}

/* bytepad(X, w) 的补零：把已吸收计数 cnt 补到 w 的整数倍 */
static void absorb_bytepad_tail(pqchsm_sponge_t *s, size_t w, size_t cnt)
{
	size_t pad = (w - cnt % w) % w;
	if (pad)
		pqchsm_sponge_absorb_zeros(s, pad);
}

void pqchsm_cshake256(const uint8_t *x, size_t x_len,
                      const uint8_t *n, size_t n_len,
                      const uint8_t *s_str, size_t s_len,
                      uint8_t *out, size_t out_len)
{
	pqchsm_sponge_t sp;
	uint8_t         enc[9];
	size_t          l, cnt;

	if (n_len == 0 && s_len == 0) {
		pqchsm_xof(PQCHSM_SHAKE256_RATE, PQCHSM_DOMAIN_SHAKE,
		           x, x_len, out, out_len);
		return;
	}

	pqchsm_sponge_init(&sp, PQCHSM_SHAKE256_RATE);
	/* bytepad(encode_string(N) || encode_string(S), 136) */
	l = pqchsm_left_encode(PQCHSM_SHAKE256_RATE, enc);
	pqchsm_sponge_absorb(&sp, enc, l);
	cnt = l;
	cnt += absorb_encode_string(&sp, n, n_len);
	cnt += absorb_encode_string(&sp, s_str, s_len);
	absorb_bytepad_tail(&sp, PQCHSM_SHAKE256_RATE, cnt);

	if (x_len)
		pqchsm_sponge_absorb(&sp, x, x_len);
	pqchsm_sponge_pad(&sp, PQCHSM_DOMAIN_CSHAKE);
	pqchsm_sponge_squeeze(&sp, out, out_len);
	pqchsm_bzero(&sp, sizeof(sp));
}

void pqchsm_bzero(void *p, size_t len)
{
	volatile uint8_t *v = (volatile uint8_t *)p;
	while (len--)
		*v++ = 0;
}
