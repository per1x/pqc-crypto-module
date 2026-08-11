/* pqchsm_fips202_mld.h —— mldsa-native 的 FIPS-202 自定义头
 *
 * 通过 MLD_CONFIG_FIPS202_CUSTOM_HEADER 替换 mldsa 自带 fips202.h。
 * mldsa 用的是 PQClean 风格的增量 API（init/absorb/finalize/squeeze/release，
 * squeeze 支持任意长度、跨调用保持 pos），外加 shake256 单发。
 * 与 liboqs 集成胶水头暴露的符号一致。
 */
#ifndef PQCHSM_FIPS202_MLD_H
#define PQCHSM_FIPS202_MLD_H

#include "ta_fips202.h"

#define SHAKE128_RATE PQCHSM_SHAKE128_RATE
#define SHAKE256_RATE PQCHSM_SHAKE256_RATE

typedef pqchsm_sponge_t mld_shake128ctx;
typedef pqchsm_sponge_t mld_shake256ctx;

static inline void mld_shake128_init(mld_shake128ctx *ctx)
{
	pqchsm_sponge_init(ctx, SHAKE128_RATE);
}
static inline void mld_shake128_absorb(mld_shake128ctx *ctx,
                                       const uint8_t *in, size_t inlen)
{
	pqchsm_sponge_absorb(ctx, in, inlen);
}
static inline void mld_shake128_finalize(mld_shake128ctx *ctx)
{
	pqchsm_sponge_pad(ctx, PQCHSM_DOMAIN_SHAKE);
}
static inline void mld_shake128_squeeze(uint8_t *out, size_t outlen,
                                        mld_shake128ctx *ctx)
{
	pqchsm_sponge_squeeze(ctx, out, outlen);
}
static inline void mld_shake128_release(mld_shake128ctx *ctx)
{
	pqchsm_bzero(ctx, sizeof(*ctx));
}

static inline void mld_shake256_init(mld_shake256ctx *ctx)
{
	pqchsm_sponge_init(ctx, SHAKE256_RATE);
}
static inline void mld_shake256_absorb(mld_shake256ctx *ctx,
                                       const uint8_t *in, size_t inlen)
{
	pqchsm_sponge_absorb(ctx, in, inlen);
}
static inline void mld_shake256_finalize(mld_shake256ctx *ctx)
{
	pqchsm_sponge_pad(ctx, PQCHSM_DOMAIN_SHAKE);
}
static inline void mld_shake256_squeeze(uint8_t *out, size_t outlen,
                                        mld_shake256ctx *ctx)
{
	pqchsm_sponge_squeeze(ctx, out, outlen);
}
static inline void mld_shake256_release(mld_shake256ctx *ctx)
{
	pqchsm_bzero(ctx, sizeof(*ctx));
}

static inline void mld_shake256(uint8_t *out, size_t outlen,
                                const uint8_t *in, size_t inlen)
{
	pqchsm_xof(PQCHSM_SHAKE256_RATE, PQCHSM_DOMAIN_SHAKE,
	           in, inlen, out, outlen);
}

#endif /* PQCHSM_FIPS202_MLD_H */
