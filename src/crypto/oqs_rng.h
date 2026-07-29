/* 内部头：liboqs 确定性随机源注入。上层不应直接使用。 */
#ifndef PQCHSM_OQS_RNG_H
#define PQCHSM_OQS_RNG_H

#include <stddef.h>
#include <stdint.h>

/* 幂等；安装自定义随机源回调 */
void pqc_oqs_rng_init(void);

/* 进入脚本模式并加锁。script 须在 end() 之前保持有效。 */
int pqc_oqs_rng_begin(const uint8_t *script, size_t len);

/* 退出脚本模式并解锁。
 * 返回 0 正常；-1 表示脚本被消费完后后端仍索取随机数（模型判断错误）。
 * consumed 可为 NULL；否则回填实际消费的字节数。 */
int pqc_oqs_rng_end(size_t *consumed);

#endif
