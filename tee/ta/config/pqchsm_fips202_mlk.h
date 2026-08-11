/* pqchsm_fips202_mlk.h —— mlkem-native 的 FIPS-202 自定义头
 *
 * 通过 MLK_CONFIG_FIPS202_CUSTOM_HEADER 替换 mlkem 自带 fips202.h，
 * 把 mlk_* 符号映射到 ta_fips202.c 的海绵核。API 面与
 * mlkem-native 文档 FIPS202.md / liboqs 集成胶水头一致：
 *   mlk_shake128ctx / init / absorb_once（含 padding）/ squeezeblocks /
 *   release / mlk_shake256 单发 / mlk_sha3_256 / mlk_sha3_512 / SHAKE128_RATE
 * 全部为 static inline，三个参数集编译单元各自包含不会冲突。
 */
#ifndef PQCHSM_FIPS202_MLK_H
#define PQCHSM_FIPS202_MLK_H

#include "ta_fips202.h"

#define SHAKE128_RATE PQCHSM_SHAKE128_RATE
#define SHAKE256_RATE PQCHSM_SHAKE256_RATE

typedef pqchsm_sponge_t mlk_shake128ctx;

static inline void mlk_shake128_init(mlk_shake128ctx *ctx)
{
	pqchsm_sponge_init(ctx, SHAKE128_RATE);
}

/* absorb_once = 吸收全部输入并完成域分隔 padding（0x1F） */
static inline void mlk_shake128_absorb_once(mlk_shake128ctx *ctx,
                                            const uint8_t *in, size_t inlen)
{
	pqchsm_sponge_absorb(ctx, in, inlen);
	pqchsm_sponge_pad(ctx, PQCHSM_DOMAIN_SHAKE);
}

/* 约定：调用时输出位置块对齐（pos==0 或整块挤出后），mlkem 的 XOF 用法满足 */
static inline void mlk_shake128_squeezeblocks(uint8_t *out, size_t nblocks,
                                              mlk_shake128ctx *ctx)
{
	pqchsm_sponge_squeeze(ctx, out, nblocks * SHAKE128_RATE);
}

static inline void mlk_shake128_release(mlk_shake128ctx *ctx)
{
	pqchsm_bzero(ctx, sizeof(*ctx));
}

static inline void mlk_shake256(uint8_t *out, size_t outlen,
                                const uint8_t *in, size_t inlen)
{
	pqchsm_xof(PQCHSM_SHAKE256_RATE, PQCHSM_DOMAIN_SHAKE,
	           in, inlen, out, outlen);
}

static inline void mlk_sha3_256(uint8_t *out, const uint8_t *in, size_t inlen)
{
	pqchsm_xof(PQCHSM_SHA3_256_RATE, PQCHSM_DOMAIN_SHA3, in, inlen, out, 32);
}

static inline void mlk_sha3_512(uint8_t *out, const uint8_t *in, size_t inlen)
{
	pqchsm_xof(PQCHSM_SHA3_512_RATE, PQCHSM_DOMAIN_SHA3, in, inlen, out, 64);
}

#endif /* PQCHSM_FIPS202_MLK_H */
