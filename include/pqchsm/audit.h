/* pqchsm/audit.h —— append-only 审计日志（哈希链，路线图 §8.6）
 *
 * 记录"谁在什么时候对哪个槽位做了什么、成没成功"，一条一条往后串成哈希链：
 * 任何一条被改动，从那条起后面每一条的哈希都对不上，验证时能直接指出
 * **第一处**出问题的序号。
 *
 * 文件布局（全部显式小端逐字节编码 —— 绝不 fwrite 结构体，
 * 否则换个编译器/换个架构，同一条记录就会算出不同的哈希）：
 *
 *   文件头 64 字节，**就地更新**：
 *     magic "PQCHSMAL"(8) | version u32 | reserved u32 | count u64 | head[32] | 保留 8 字节
 *   其后是 count 条**定长 96 字节**的记录：
 *     entry_wire(64) | H_i(32)
 *   entry_wire = seq u64 | timestamp u64 | op u32 | role u32 | slot_id u32 | result u32 | detail[32]
 *
 *   于是恒有：文件大小 == 64 + 96 * count。这个恒等式是截断检测的全部秘密，见下。
 *
 * 哈希链：
 *   H_0 = SHA3-256("pqc-hsm/audit/genesis")    （21 字节 ASCII，**不含**结尾的 NUL）
 *   H_i = SHA3-256(H_{i-1} ‖ entry_wire_i)
 *   第 seq 条记录（seq 从 0 数起）落盘的哈希就是 H_{seq+1}，文件头里的 head 是 H_count。
 *   每条都把自己的 H_i 一起存下来，验证时逐条重算并比对 —— 这样能定位到具体是哪一条
 *   出的问题，而不是只知道"整体坏了"。
 *
 * detail 是 32 字节的短文本（不足补 0，超长截断），只放人类可读的补充信息。
 * 写入时**非可打印 ASCII 一律换成 '.'**：审计日志是给人看的，混进控制字符或 ANSI
 * 转义序列，就等于给日志阅读器开了一个注入口子。
 *
 * ---------------------------------------------------------------------------
 * 【截断怎么检测 —— 这是本格式唯一的非平凡设计】
 *
 * 如果格式是纯粹的"一条接一条"，砍掉尾部之后剩下的前缀**仍然自洽**，验证程序
 * 无从发现。而尾部恰恰是最不能丢的一段（刚出事的那几条），砍日志尾巴也是攻击者
 * 成本最低的一招。所以本格式在文件头里存了 count 与 head，并且每次 append
 * 都就地把它们更新掉：
 *
 *   验证时先算 R = (文件大小 - 64) / 96，要求 R == count 且余数为 0；
 *   走完整条链之后，算出来的链头还必须等于文件头里的 head。
 *
 *   砍掉尾部 k 条 ⇒ R = count - k ⇒ 当场失败，bad_seq = R（第一条缺失的序号）。
 *   尾部多追一条    ⇒ R = count + 1 ⇒ 当场失败，bad_seq = count。
 *   砍到半条        ⇒ 余数非 0     ⇒ 当场失败。
 *
 * 但请注意：这挡住的是"只动记录、不动文件头"的攻击者。连文件头一起改的，见下。
 *
 * ---------------------------------------------------------------------------
 * 【这条链能防什么、不能防什么 —— 请务必读完再依赖它】
 *
 * 能防：**改中间而不改后面**。改任意一条记录的任意字段、删掉中间一条、调换两条的
 * 顺序，从被动过的那条起后面每一条的 H_i 都对不上，audit_verify_file 会把第一处
 * 问题的序号写进 *bad_seq。
 * 也能防（靠上面那个文件头）：**朴素的尾部截断**与**朴素的尾部伪造追加**。
 *
 * 不能防：**能写整个文件的攻击者**。哈希链的算法是公开的，链头就明晃晃地写在
 * 文件头里；攻击者完全可以从任意一点起重算整条链、顺手把文件头的 count 和 head
 * 也改成自洽的值，交出一份挑不出毛病的假日志。删掉中间一条再重排序号、砍掉尾部
 * 再把前一条的哈希抄进文件头 —— 这些都是几十行脚本的事。
 * 纯哈希链在数学上做不到自证：它只保证"改动会向后传播"，**不保证"改动无法被抹平"**。
 * 本模块的测试里专门留了两条用例（forge_append / truncate_with_header）把这个洞
 * 显式地跑出来，断言 verify 返回 0 —— 与其假装没有，不如让它一直在眼前。
 *
 * 真正的防线是路线图 §8.6 的那一句：**定期用设备 ML-DSA 身份钥对链头签名固化**。
 * 把 (count, head, 时间) 签出去，送到这台设备改不到的地方（另一台机器/上级系统）。
 * 有了外部锚点，攻击者要抹平就得先伪造签名，那才谈得上不可否认。
 * 本文件**不做**签名固化，它只负责提供随时可被签的链头（audit_head）。
 * 在锚点落地之前，请把本模块的保证理解成"防意外损坏与不完整的篡改"，
 * 而不是"防一个有 root 的对手"。
 *
 * ---------------------------------------------------------------------------
 * 【落盘顺序与掉电窗口】
 *
 * audit_append 的顺序是：把记录写到文件末尾 → fsync → 就地更新文件头 → fsync，
 * 两次 fsync 都成功才返回 0。记录先落盘，是因为"已经答应记下来的事"优先留在盘上。
 * 代价是有一个窗口：掉电正好发生在两次 fsync 之间，盘上会多出一条文件头还不认的
 * 记录，此后 audit_verify_file 会**失败**（bad_seq = count）。
 * 这是**故意 fail-closed**：掉电留下的多余记录和攻击者伪造追加的记录在字节上完全
 * 无法区分，宁可报警也不要默默接受。真要恢复，得靠签名固化过的链头来判定哪一段可信。
 *
 * 【并发】本模块假设**单写者**，不做文件锁。多进程同时 append 会互相覆盖。
 *
 * 【红线（§8.7）】本头文件里没有任何可以传入密钥、种子或 PIN 的入口，
 * detail 是给人看的短文本，调用方不得往里塞密钥材料。
 */
#ifndef PQCHSM_AUDIT_H
#define PQCHSM_AUDIT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIT_HASH_LEN   32
#define AUDIT_DETAIL_LEN 32

/* 被记录的操作。数值一旦发布就不能改 —— 它进哈希链，改了旧日志全部失效。
 * 本枚举刻意独立于 slot.h：审计模块要能被单独编译、单独验证。 */
typedef enum {
	AUDIT_OP_INIT_TOKEN = 1, AUDIT_OP_LOGIN, AUDIT_OP_LOGIN_FAIL, AUDIT_OP_LOGOUT,
	AUDIT_OP_GENERATE, AUDIT_OP_LOAD, AUDIT_OP_SIGN, AUDIT_OP_DECAPS,
	AUDIT_OP_DESTROY, AUDIT_OP_ZEROIZE, AUDIT_OP_UNLOCK, AUDIT_OP_LOCKOUT,
	AUDIT_OP_BACKUP_EXPORT, AUDIT_OP_BACKUP_IMPORT, AUDIT_OP_RESTORE,
	AUDIT_OP_KEK_ROTATE,
} audit_op_t;

typedef struct audit_log audit_log_t;

/* 打开（不存在则创建并写入创世链头）。失败返回 NULL。
 * 创建走 keystore 的原子写套路：tmp → fsync → rename → fsync(dir)，
 * 保证外界永远看不到一个半截的文件头。
 * 打开时只校验文件头与"大小 == 64 + 96*count"，**不**走完整条链（那是 O(N)）；
 * 上电自检请显式调 audit_verify_file。 */
audit_log_t *audit_open(const char *path);
void audit_close(audit_log_t *log);

/* 追加一条。timestamp 由调用方给（便于测试可复现）；detail 可为 NULL。
 * 成功返回 0。**必须 fsync 后才返回**（审计记录不能因掉电丢失）。
 * op 超出上面枚举的范围一律拒绝。 */
int audit_append(audit_log_t *log, uint64_t timestamp, audit_op_t op,
                 uint32_t role, uint32_t slot_id, uint32_t result,
                 const char *detail);

/* 当前链头哈希 —— 这就是 §8.6 里那个"该拿设备身份钥去签"的值。 */
int audit_head(const audit_log_t *log, uint8_t out[AUDIT_HASH_LEN]);
/* 当前记录条数。log 为 NULL 返回 0。 */
uint64_t audit_count(const audit_log_t *log);

/* 从头完整验证链。返回 0 表示完好；
 * 返回 -1 并把第一处出问题的序号写入 *bad_seq（可为 NULL）。
 * bad_seq == count 表示"每条记录自身都自洽，但链头与文件头的锚点对不上"，
 * 或"尾部多出了文件头不认的记录"。文件头本身就坏掉时 bad_seq 为 0。 */
int audit_verify_file(const char *path, uint64_t *bad_seq);

/* 读第 seq 条（从 0 开始）到调用方缓冲，用于导出/展示。成功返回 0。
 * 各输出指针都可以单独为 NULL。detail 会补上结尾的 NUL（故需 33 字节）。
 * 顺带做一次**局部**校验：用前一条的哈希重算本条，对不上就返回 -1。
 * 这只是廉价的就近检查，整体完好性仍以 audit_verify_file 为准。 */
int audit_read(const char *path, uint64_t seq, uint64_t *timestamp, uint32_t *op,
               uint32_t *role, uint32_t *slot_id, uint32_t *result,
               char detail[AUDIT_DETAIL_LEN + 1]);

#ifdef __cplusplus
}
#endif
#endif /* PQCHSM_AUDIT_H */
