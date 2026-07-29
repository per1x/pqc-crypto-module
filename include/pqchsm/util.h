/* pqchsm/util.h —— 十六进制、安全清零、常量时间比较 */
#ifndef PQCHSM_UTIL_H
#define PQCHSM_UTIL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 不会被编译器优化掉的清零（路线图 §8.7「中间值用后即清」）。
 * 实现用 OPENSSL_cleanse —— 不要换成 memset。 */
void pqc_secure_zero(void *p, size_t n);

/* 常量时间比较：相等返回 1。用于 PIN 校验（§7.3）与 tag 比对。 */
int pqc_ct_equal(const void *a, const void *b, size_t n);

/* 取 n 字节随机数。成功返回 0。
 * 单独包一层是为了让 HAL 等上层不必直接依赖 OpenSSL 头；
 * Phase 7 起这里会换成片内 TRNG。 */
int pqc_random_bytes(uint8_t *out, size_t n);

/* 存放密钥材料的缓冲：分配后尽力 mlock（防换页到磁盘），
 * 释放前必定清零。mlock 失败不算错误（容器/无权限环境常见），
 * 但清零永远执行 —— 对应 Phase 5 的"mlock + 用后清零"过渡性妥协。 */
void *pqc_secure_alloc(size_t n);
void  pqc_secure_free(void *p, size_t n);

/* hex → bytes。返回写入字节数，出错返回 -1（奇数长度 / 非法字符 / 缓冲不足）。
 * hex_len 为 SIZE_MAX 时按 strlen 计算。 */
long pqc_hex_decode(const char *hex, size_t hex_len, uint8_t *out, size_t out_cap);

/* bytes → 小写 hex，写入 out（需要 2n+1 字节）。成功返回 0。 */
int pqc_hex_encode(const uint8_t *in, size_t n, char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
#endif /* PQCHSM_UTIL_H */
