/* ta_random.h —— TA 内统一随机源
 *
 * OP-TEE 构建（-DPQCHSM_TA_OPTEE=1）走 TEE_GenerateRandom（内核 RNG，
 * zynqmp 上由 CSU TRNG 喂）；native 测试构建走 getrandom(2)/arc4random。
 * 接口不返回错误：两个后端在系统层面失败都属于致命状态，OP-TEE 的
 * TEE_GenerateRandom 本身也是 void。
 */
#ifndef PQCHSM_TA_RANDOM_H
#define PQCHSM_TA_RANDOM_H

#include <stddef.h>
#include <stdint.h>

void pqchsm_randombytes(uint8_t *out, size_t len);

#endif /* PQCHSM_TA_RANDOM_H */
