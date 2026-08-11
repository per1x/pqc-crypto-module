/* pqchsm_mld_config.h —— mldsa-native 的集成配置（经 MLD_CONFIG_FILE 引入）
 *
 * 与 pqchsm_mlk_config.h 同构：每参数集一个编译单元，命名空间前缀区分；
 * 串行 Keccak + 自定义 FIPS-202 头 + TA 随机源。
 * 未启用 KEYGEN_PCT，需要时加 MLD_CONFIG_KEYGEN_PCT。
 */
#ifndef PQCHSM_MLD_CONFIG_H
#define PQCHSM_MLD_CONFIG_H

#if !defined(MLD_CONFIG_PARAMETER_SET)
#error "编译单元必须先定义 MLD_CONFIG_PARAMETER_SET（44/65/87）"
#endif

#if MLD_CONFIG_PARAMETER_SET == 44
#define MLD_CONFIG_NAMESPACE_PREFIX PQCHSM_MLD44
#elif MLD_CONFIG_PARAMETER_SET == 65
#define MLD_CONFIG_NAMESPACE_PREFIX PQCHSM_MLD65
#elif MLD_CONFIG_PARAMETER_SET == 87
#define MLD_CONFIG_NAMESPACE_PREFIX PQCHSM_MLD87
#else
#error "未知 MLD_CONFIG_PARAMETER_SET"
#endif

#define MLD_CONFIG_SERIAL_FIPS202_ONLY
#define MLD_CONFIG_FIPS202_CUSTOM_HEADER "pqchsm_fips202_mld.h"

#define MLD_CONFIG_CUSTOM_RANDOMBYTES
#include <stddef.h>
#include <stdint.h>
#include "ta_random.h"
static inline int mld_randombytes(uint8_t *out, size_t outlen)
{
	pqchsm_randombytes(out, outlen);
	return 0;
}

#endif /* PQCHSM_MLD_CONFIG_H */
