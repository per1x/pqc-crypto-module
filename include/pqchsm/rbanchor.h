/* pqchsm/rbanchor.h —— 防回滚锚点（keystore 的单调 epoch 存到哪里）
 *
 * ============================================================================
 * 【问题】
 * ============================================================================
 * keystore 全文件有 MAC，所以改一个字节会被发现。但**把整份文件换成一份旧快照**
 * 不会：那份 MAC 当初就是我们自己算的，对旧快照照样是对的。PIN 锁定计数、
 * 已吊销的密钥、槽位状态会一起回到从前。
 *
 * 唯一的解法是一个**攻击者无法回退的单调量**。keystore 里存"我保存时锚点是多少"，
 * 装载时比一比，小了就拒。所以全部问题变成：那个单调量放在哪儿。
 *
 * ============================================================================
 * 【两种放法，强度差一个量级】
 * ============================================================================
 *   file  —— 放在旁边的另一个普通文件（<keystore>.epoch）。
 *            **这不是真正的防回滚**：能写 SD 卡的攻击者把两个文件一起换回去
 *            就绕过了。它的真实收益只有一条 —— 把"换一个文件"提高到
 *            "必须一致地换两个文件"，并且让"事后发现被回滚过"成为可能。
 *
 *   rpmb  —— 放在 eMMC 的 RPMB 分区。锚点值就是 RPMB 的**写计数器**：
 *            硬件维护、只增不减，**任何人都没有办法让它变小**，包括拿到
 *            RPMB 认证密钥的人。于是"拿一份旧 keystore 冒充当前状态"这件事
 *            在物理上不成立，而不只是"很难"。
 *
 * ⚠️ 为什么锚点是**计数器**而不是"写进 RPMB 的一个数"：
 *    RPMB 的数据区是可以被密钥持有者随便改的 —— 包括改小。而这块板上没有
 *    秘密硬件根（见 docs/SECURITY.md），RPMB 认证密钥只能放在一个文件里，
 *    也就是说"攻击者拿到密钥"是必须假设会发生的事。
 *    锚在计数器上，这个假设就不再致命：他能把计数器推上去，推不下来。
 *
 * ============================================================================
 * 【接口】
 * ============================================================================
 * scope 是"哪一份 keystore"——文件 provider 用它拼锚点文件名，RPMB provider
 * 忽略它（一块板一个计数器）。
 */
#ifndef PQCHSM_RBANCHOR_H
#define PQCHSM_RBANCHOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pqc_rbanchor_provider {
	const char *name;

	/* 锚点的单调性是否由**硬件**保证（攻击者写不了、也退不回去）。
	 * 文件 provider 为 0。上层可以据此在启动自检里如实报告防回滚强度 ——
	 * 别把 0 说成"有防回滚"。 */
	int hardware_monotonic;

	/* 读当前锚点。成功返回 0。 */
	int (*read)(void *user, const char *scope, uint64_t *out);

	/* 把锚点往前推一格，回填推完之后的值。成功返回 0。
	 * **只增不减**：实现里不允许提供任何"设回某个值"的语义。 */
	int (*bump)(void *user, const char *scope, uint64_t *out);

	void *user;
} pqc_rbanchor_provider_t;

/* 默认 provider：文件。scope = keystore 路径，锚点写在 <scope>.epoch。 */
const pqc_rbanchor_provider_t *pqc_rbanchor_provider_file(void);

/* 装/查当前 provider。p == NULL 恢复默认（文件）。 */
void                            pqc_rbanchor_set_provider(const pqc_rbanchor_provider_t *p);
const pqc_rbanchor_provider_t  *pqc_rbanchor_get_provider(void);

/* 当前锚点是否由硬件保证单调 */
int pqc_rbanchor_is_hardware_monotonic(void);

int pqc_rbanchor_read(const char *scope, uint64_t *out);
int pqc_rbanchor_bump(const char *scope, uint64_t *out);

#ifdef __cplusplus
}
#endif
#endif /* PQCHSM_RBANCHOR_H */
