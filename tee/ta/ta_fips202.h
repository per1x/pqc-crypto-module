/* ta_fips202.h —— TA 内置 Keccak 核（SHA-3 / SHAKE / cSHAKE / KMAC 底座）
 *
 * 为什么自己写：mlkem-native / mldsa-native 允许用
 * MLK_CONFIG_FIPS202_CUSTOM_HEADER / MLD_CONFIG_FIPS202_CUSTOM_HEADER
 * 替换 FIPS-202 后端，TA 里没有 OpenSSL/liboqs 可用，所以这里提供一个
 * 可移植的软件 Keccak-f[1600]，并套上两个库各自要求的 API 皮肤
 * （见 config/pqchsm_fips202_mlk.h 和 config/pqchsm_fips202_mld.h）。
 * KMAC-256（SP 800-185）也建在这上面（ta_kdf.c）。
 *
 * 假设小端（aarch64 / x86_64 均成立），ta_fips202.c 里有编译期检查。
 */
#ifndef PQCHSM_TA_FIPS202_H
#define PQCHSM_TA_FIPS202_H

#include <stddef.h>
#include <stdint.h>

#define PQCHSM_SHAKE128_RATE  168
#define PQCHSM_SHAKE256_RATE  136
#define PQCHSM_SHA3_256_RATE  136
#define PQCHSM_SHA3_512_RATE  72

/* 域分隔字节（FIPS 202 / SP 800-185） */
#define PQCHSM_DOMAIN_SHAKE   0x1F
#define PQCHSM_DOMAIN_SHA3    0x06
#define PQCHSM_DOMAIN_CSHAKE  0x04

typedef struct {
	uint64_t st[25];  /* Keccak 状态，小端字节视图 */
	size_t   pos;     /* 当前块内偏移（字节），0..rate */
	size_t   rate;    /* 速率（字节） */
} pqchsm_sponge_t;

void pqchsm_sponge_init(pqchsm_sponge_t *s, size_t rate);
void pqchsm_sponge_absorb(pqchsm_sponge_t *s, const uint8_t *in, size_t len);
void pqchsm_sponge_absorb_zeros(pqchsm_sponge_t *s, size_t len);
/* 域分隔 padding：pos 处 XOR domain，rate-1 处 XOR 0x80，置换，pos 归零 */
void pqchsm_sponge_pad(pqchsm_sponge_t *s, uint8_t domain);
void pqchsm_sponge_squeeze(pqchsm_sponge_t *s, uint8_t *out, size_t len);

/* 一次性 XOF/哈希：init → absorb → pad(domain) → squeeze */
void pqchsm_xof(size_t rate, uint8_t domain,
                const uint8_t *in, size_t in_len,
                uint8_t *out, size_t out_len);

/* SP 800-185 编码原语。out 至少 9 字节，返回写入长度。 */
size_t pqchsm_left_encode(uint64_t x, uint8_t *out);
size_t pqchsm_right_encode(uint64_t x, uint8_t *out);

/* cSHAKE256(X, L, N, S)：N、S 均为空时按标准退化为 SHAKE256 */
void pqchsm_cshake256(const uint8_t *x, size_t x_len,
                      const uint8_t *n, size_t n_len,
                      const uint8_t *s_str, size_t s_len,
                      uint8_t *out, size_t out_len);

/* 防编译器优化掉的清零（私钥/中间态销毁用） */
void pqchsm_bzero(void *p, size_t len);

#endif /* PQCHSM_TA_FIPS202_H */
