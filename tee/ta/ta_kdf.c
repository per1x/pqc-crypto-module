#include "ta_kdf.h"

#include <string.h>

#include "ta_fips202.h"

/* KMAC256(K, X, L, S) =
 *   cSHAKE256(bytepad(encode_string(K), 136) || X || right_encode(L),
 *             L, N="KMAC", S)
 * 展开成一次海绵吸收（全流式，不拼大缓冲）：
 *   bytepad(enc_str("KMAC") || enc_str(S), 136)        -- cSHAKE 前缀
 *   bytepad(enc_str(K), 136) || X || right_encode(L)  -- KMAC 输入
 * 域分隔 0x04。
 */

static size_t absorb_left_encode(pqchsm_sponge_t *sp, uint64_t x)
{
	uint8_t enc[9];
	size_t  l = pqchsm_left_encode(x, enc);

	pqchsm_sponge_absorb(sp, enc, l);
	return l;
}

static size_t absorb_encode_string(pqchsm_sponge_t *sp,
                                   const uint8_t *s, size_t len)
{
	size_t cnt = absorb_left_encode(sp, (uint64_t)len * 8);

	if (len)
		pqchsm_sponge_absorb(sp, s, len);
	return cnt + len;
}

static void absorb_bytepad_tail(pqchsm_sponge_t *sp, size_t w, size_t cnt)
{
	size_t pad = (w - cnt % w) % w;

	if (pad)
		pqchsm_sponge_absorb_zeros(sp, pad);
}

int ta_kmac256(const uint8_t *key, size_t key_len,
               const uint8_t *data, size_t data_len,
               const uint8_t *custom, size_t custom_len,
               uint8_t *out, size_t out_len)
{
	static const uint8_t N_KMAC[4] = { 'K', 'M', 'A', 'C' };
	pqchsm_sponge_t sp;
	uint8_t         enc[9];
	size_t          l, cnt;

	if (!key || key_len == 0 || !out || out_len == 0)
		return -1;
	if (!data && data_len != 0)
		return -1;
	if (!custom)
		custom_len = 0;

	pqchsm_sponge_init(&sp, PQCHSM_SHAKE256_RATE);

	/* cSHAKE 前缀：bytepad(encode_string("KMAC") || encode_string(S), 136) */
	cnt = absorb_left_encode(&sp, PQCHSM_SHAKE256_RATE);
	cnt += absorb_encode_string(&sp, N_KMAC, sizeof(N_KMAC));
	cnt += absorb_encode_string(&sp, custom, custom_len);
	absorb_bytepad_tail(&sp, PQCHSM_SHAKE256_RATE, cnt);

	/* KMAC 输入：bytepad(encode_string(K), 136) || X || right_encode(L) */
	cnt = absorb_left_encode(&sp, PQCHSM_SHAKE256_RATE);
	cnt += absorb_encode_string(&sp, key, key_len);
	absorb_bytepad_tail(&sp, PQCHSM_SHAKE256_RATE, cnt);
	if (data_len)
		pqchsm_sponge_absorb(&sp, data, data_len);
	l = pqchsm_right_encode((uint64_t)out_len * 8, enc);
	pqchsm_sponge_absorb(&sp, enc, l);

	pqchsm_sponge_pad(&sp, PQCHSM_DOMAIN_CSHAKE);
	pqchsm_sponge_squeeze(&sp, out, out_len);
	pqchsm_bzero(&sp, sizeof(sp));
	return 0;
}

int ta_kdf_derive(const uint8_t *ikm, size_t ikm_len,
                  const uint8_t *salt, size_t salt_len,
                  const char *label,
                  uint8_t *out, size_t out_len)
{
	if (!label || label[0] == '\0')
		return -1;
	if (!salt)
		salt_len = 0;
	return ta_kmac256(ikm, ikm_len, salt, salt_len,
	                  (const uint8_t *)label, strlen(label), out, out_len);
}
