/* 内部头：软件 TRNG 模型的故障注入钩子。只给测试用，上层不应引用。
 *
 * 存在的理由：告警与空池在真硬件上是"等它自己发生"的事件，软件模型里必须
 * 能主动制造，否则 hwrng_bytes() 里那条"整批作废"的路径永远走不到。
 * 与 RTL 侧用 -P 把 RCT_CUTOFF 压到 2 让告警确定性发生，是同一个思路。 */
#ifndef PQCHSM_HWRNG_STUB_H
#define PQCHSM_HWRNG_STUB_H

/* 置 1 后模型立刻锁存 ALARM（连同 RCT_ALARM），并停止产出。 */
void hwrng_stub_force_alarm(int on);

/* 置 1 后模型不再补充 FIFO —— 用来制造空池，验证驱动不会硬读 RDATA。 */
void hwrng_stub_starve(int on);

/* 再弹出 n 个字之后才锁存 ALARM（n=0 关掉）。
 * 用来制造"取到一半才告警"这种情形 —— 驱动此时已经把前半批数据拷进了
 * 调用方的缓冲区，必须靠取完之后的复查发现问题并把整批清掉。
 * force_alarm 走的是"一开始就告警"的早退路径，覆盖不到复查那条。 */
void hwrng_stub_alarm_after(unsigned words);

/* 把模型复位到上电状态（关掉两个注入开关，清计数与 FIFO）。 */
void hwrng_stub_reset(void);

#endif
