/* pqchsm/shamir.h —— Shamir M-of-N 门限秘密分享
 *
 * 用途：把 KDR 备份份额 / 主密钥拆成 n 片交给 n 个管理员保管，
 * 任意 m 片可以还原，少于 m 片在信息论意义上得不到任何信息。
 *
 * 数学基础：GF(256)（既约多项式 0x11B，与 AES 同一个域）。
 * secret 的每个字节独立走一条 m-1 次多项式，常数项就是该字节，
 * 其余系数取随机；分片 i 的第 j 字节 = 第 j 条多项式在 x=i 处的值。
 * x 从 1 开始 —— x=0 处的值就是秘密本身，绝不能分发出去。
 *
 * 侧信道：域乘法用位运算的常量时间实现，不用 log/antilog 查表
 *（表法的索引依赖秘密数据，有缓存侧信道），拉格朗日插值全程无数据相关分支。
 *
 * ⚠️ 固有性质：分片里**不存 m**。
 * 所以 shamir_combine 无法知道门限到底是多少，它只能拿调用方给的 k 片去插值。
 * k < m 时它会得到一个**错误的秘密并返回 0**，而不是报错 —— 这不是实现缺陷，
 * 而是 Shamir 方案的定义：少于门限的分片对秘密没有任何约束，
 * 任何 k < m 的子集都能"合法地"插出某个值，且该值与真秘密无法区分。
 * 上层（备份策略/管理员流程）必须自己记住 m，并对还原结果做独立校验
 *（例如比对秘密自身的 KMAC tag）。分片自带的 4 字节校验只保证
 * **单片没被篡改**，不保证凑够了门限。
 */
#ifndef PQCHSM_SHAMIR_H
#define PQCHSM_SHAMIR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 分片数上限。索引占 1 字节且从 1 起，理论上限是 255，
 * 这里收到 16 是管理流程的现实约束。 */
#define SHAMIR_MAX_SHARES 16

/* 单个秘密的字节数上限：64 字节足够放下 KDR(32) 或一对 32 字节密钥。 */
#define SHAMIR_MAX_SECRET 64

/* 分片线格式：index(1) | secret_len(1) | data[secret_len] | checksum(4)
 * checksum = SHA3-256(index || secret_len || data) 的前 4 字节。 */
#define SHAMIR_SHARE_OVERHEAD 6

/* 把 secret 分成 n 份，任意 m 份可恢复。
 * shares 是调用方给的二维缓冲：n 行，每行 share_cap 字节
 *（第 i 行起始于 shares + i * share_cap）。
 * 每行写入的实际长度（= secret_len + SHAMIR_SHARE_OVERHEAD）回填到 share_lens[i]。
 *
 * 约束：2 <= m <= n <= SHAMIR_MAX_SHARES，
 *       1 <= secret_len <= SHAMIR_MAX_SECRET，
 *       share_cap >= secret_len + SHAMIR_SHARE_OVERHEAD。
 * 随机系数取自 OpenSSL RAND_bytes，取不到随机数即失败。
 * 成功返回 0，失败返回负数（失败时 shares 的前 n 行会被清零）。 */
int shamir_split(const uint8_t *secret, size_t secret_len,
                 uint8_t m, uint8_t n,
                 uint8_t *shares, size_t share_cap, size_t *share_lens);

/* 用 k 份分片恢复。shares 同样是 k 行 share_cap 的二维缓冲。
 * 先逐片验校验和，任何一片损坏立刻返回负数（不会拿坏数据去插值）。
 * 索引重复、索引为 0、各片 secret_len 不一致、长度字段与 share_lens 对不上、
 * 输出缓冲不足 —— 都返回负数。
 *
 * 注意 k 只要落在 [1, SHAMIR_MAX_SHARES] 就是"合法参数"：
 * 见文件头对 k < m 的说明，本函数无从判断门限是否凑够。
 * 成功返回 0，*out_len 置为秘密长度。 */
int shamir_combine(const uint8_t *shares, size_t share_cap, const size_t *share_lens,
                   uint8_t k, uint8_t *out, size_t out_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif
#endif /* PQCHSM_SHAMIR_H */
