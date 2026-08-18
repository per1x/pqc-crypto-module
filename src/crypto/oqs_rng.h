/* 内部头：liboqs 随机源的注入与串行化。上层不应直接使用。
 *
 * 【为什么这里需要一把全局锁，而不只是"脚本模式期间加锁"】
 * liboqs 的随机源是**进程级全局**的一个函数指针（OQS_randombytes_custom_algorithm），
 * 而脚本模式又是这个模块自己的一段全局状态。于是只给 begin/end 加锁是不够的：
 * 另一个线程在那段区间里调 OQS_KEM_keypair / OQS_SIG_sign 之类**普通**（随机化）
 * 操作时，它的 randombytes 一样会落进同一个回调，把脚本里的字节吃掉。
 *
 * 症状极难查：确定性那条路会得到"少了几个字节"的种子（end() 里的 consumed 校验
 * 能发现，于是表现为 KAT 偶发失败），而随机那条路会拿到**本该做种子的常量字节**
 * —— 后者不报任何错，只是密钥不再随机。
 *
 * 所以规矩是：**凡是会让 liboqs 调 randombytes 的调用，一律在这把锁里做。**
 * 锁是递归的，因此回调自身也能安全地再上一次锁 —— 这样即使将来出现一条没被
 * 包起来的路径（liboqs 内部新增的随机消费点），它也仍然是串行的，只是拿不到
 * 脚本（拿不到才是对的：脚本属于开启它的那个线程）。
 */
#ifndef PQCHSM_OQS_RNG_H
#define PQCHSM_OQS_RNG_H

#include <stddef.h>
#include <stdint.h>

/* 幂等；安装自定义随机源回调并初始化那把递归锁 */
void pqc_oqs_rng_init(void);

/* 进入/退出"随机源临界区"。任何会消费 liboqs randombytes 的调用都必须包在里面。
 * 可重入（递归锁），因此嵌套调用是安全的。 */
void pqc_oqs_rng_lock(void);
void pqc_oqs_rng_unlock(void);

/* 进入脚本模式并加锁（等价于 lock() + 装上脚本）。script 须在 end() 之前有效。
 * 脚本只对**开启它的那个线程**生效。 */
int pqc_oqs_rng_begin(const uint8_t *script, size_t len);

/* 退出脚本模式并解锁。
 * 返回 0 正常；-1 表示脚本被消费完后后端仍索取随机数（模型判断错误）。
 * consumed 可为 NULL；否则回填实际消费的字节数。 */
int pqc_oqs_rng_end(size_t *consumed);

#endif
