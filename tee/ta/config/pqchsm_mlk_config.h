/* pqchsm_mlk_config.h —— mlkem-native 的集成配置（经 MLK_CONFIG_FILE 引入）
 *
 * 每个参数集一个编译单元（ta_mlkem512/768/1024.c），单元开头先定义
 * MLK_CONFIG_PARAMETER_SET 再包含 vendor 源码；这里按参数集给出命名空间
 * 前缀，三个单元的全局符号互不冲突。
 *
 * 关键裁剪：
 *   - SERIAL_FIPS202_ONLY：本 TA 只有一个软件 Keccak 核，关掉 x4 并行路径
 *     （否则还要提供 fips202x4 四路 API）。
 *   - FIPS202_CUSTOM_HEADER：换成 config/pqchsm_fips202_mlk.h。
 *   - CUSTOM_RANDOMBYTES：换成 TA 随机源（ta_random.c）。
 * 未启用 KEYGEN_PCT（FIPS 140-3 IG 配对一致性测试），需要时加
 * MLK_CONFIG_KEYGEN_PCT 即可，库内已实现。
 */
#ifndef PQCHSM_MLK_CONFIG_H
#define PQCHSM_MLK_CONFIG_H

#if !defined(MLK_CONFIG_PARAMETER_SET)
#error "编译单元必须先定义 MLK_CONFIG_PARAMETER_SET（512/768/1024）"
#endif

#if MLK_CONFIG_PARAMETER_SET == 512
#define MLK_CONFIG_NAMESPACE_PREFIX PQCHSM_MLK512
#elif MLK_CONFIG_PARAMETER_SET == 768
#define MLK_CONFIG_NAMESPACE_PREFIX PQCHSM_MLK768
#elif MLK_CONFIG_PARAMETER_SET == 1024
#define MLK_CONFIG_NAMESPACE_PREFIX PQCHSM_MLK1024
#else
#error "未知 MLK_CONFIG_PARAMETER_SET"
#endif

#define MLK_CONFIG_SERIAL_FIPS202_ONLY
#define MLK_CONFIG_FIPS202_CUSTOM_HEADER "pqchsm_fips202_mlk.h"

#define MLK_CONFIG_CUSTOM_RANDOMBYTES
#include <stddef.h>
#include <stdint.h>
#include "ta_random.h"
static inline int mlk_randombytes(uint8_t *out, size_t outlen)
{
	pqchsm_randombytes(out, outlen);
	return 0;
}

#endif /* PQCHSM_MLK_CONFIG_H */
