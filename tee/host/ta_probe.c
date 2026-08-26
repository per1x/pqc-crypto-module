/* ta_probe —— 批 2 的硬门槛：**最小 TA 在这块板上真的跑起来了**
 *
 * ============================================================================
 * 【为什么这个程序值得单独存在】
 * ============================================================================
 * ARCHITECTURE-TARGET §9 与 FINAL-PLAN §4 都把"OP-TEE TA 从未上板跑通（卡 BL32
 * 入口）"记为**整条 TEE 主线的前置死结**。那句话必须由一次**真的在板上发生过的
 * 会话**来推翻或确认 —— 不能靠"驱动加载了"或"/dev/tee0 在"这类间接证据：
 *
 *   · `optee: initialized driver` 只说明**内核侧驱动**起来了；
 *   · `/dev/tee0` 存在也只是驱动创建了设备节点；
 *   · 这两条在**根本没有 BL32**、SMC 全部返回 unknown 的板子上……其实是不成立的，
 *     但它们**都不检查 TA 能不能加载**：TA 要经 tee-supplicant 从普通世界文件系统
 *     取出来、由 OP-TEE 核校验签名、在 S-EL0 建实例。中间任何一环断了，症状都是
 *     "驱动好好的，但 TEEC_OpenSession 失败"。
 *
 * 所以判据只有一条：**TEEC_OpenSession 成功，且 TA 里的代码真的执行过并把结果
 * 送回来了**（CMD_GET_INFO 返回它自己编译进去的协议版本）。
 *
 * 逐级打印每一步的 TEEC_Result 与 origin，因为这三步的失败原因完全不同：
 *   InitializeContext 失败 → 驱动/设备节点/权限
 *   OpenSession 失败       → TA 找不到、签名不对、ABI 不匹配、supplicant 没跑
 *   InvokeCommand 失败     → TA 里的逻辑
 * 只打一句"失败"会把这三种混成一种。
 *
 * 编（构建机上）：
 *   aarch64-linux-gnu-gcc -O2 -Wall -Wextra -o ta_probe ta_probe.c \
 *       -I<optee_client>/public -L<oc_out>/libteec -lteec
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tee_client_api.h>

#include "pqchsm_ta_proto.h"

static const char *orig(uint32_t o)
{
	switch (o) {
	case TEEC_ORIGIN_API:   return "API";
	case TEEC_ORIGIN_COMMS: return "COMMS（普通世界与 OP-TEE 之间）";
	case TEEC_ORIGIN_TEE:   return "TEE（OP-TEE 核，多半是找不到 TA/签名/ABI）";
	case TEEC_ORIGIN_TRUSTED_APP: return "TA（TA 自己返回的）";
	default: return "?";
	}
}

int main(void)
{
	TEEC_Context ctx;
	TEEC_Session sess;
	TEEC_Operation op;
	TEEC_UUID uuid = TA_PQCHSM_UUID;
	uint32_t o = 0;
	TEEC_Result r;

	printf("=== 批 2 门槛：pqc-hsm TA 上板会话 ===\n");
	printf("TA UUID = 4e2d9c1a-7b35-4f68-9a2c-d15e88406fa3\n");

	r = TEEC_InitializeContext(NULL, &ctx);
	if (r != TEEC_SUCCESS) {
		printf("✗ [1] TEEC_InitializeContext = 0x%08x\n", r);
		printf("   这一步失败与 TA 无关：看 /dev/tee0 在不在、权限、驱动。\n");
		return 2;
	}
	printf("✓ [1] TEEC_InitializeContext —— 上下文建起来了\n");

	/* ---- [2a] 先证明**匿名登录被拒**（PS-22 那道门在真板上生效）----
	 * 这一步放在成功那一步之前是有意的：如果只测"能开会话"，一个把门拆掉的
	 * TA 也能通过。两步合起来才说明"门在，而且开的是对的那扇"。 */
	memset(&op, 0, sizeof op);
	r = TEEC_OpenSession(&ctx, &sess, &uuid, TEEC_LOGIN_PUBLIC, NULL, &op, &o);
	if (r == TEEC_SUCCESS) {
		printf("✗ [2a] 匿名登录（LOGIN_PUBLIC）居然开成功了 —— "
		       "PS-22 那道门没生效\n");
		TEEC_CloseSession(&sess);
		TEEC_FinalizeContext(&ctx);
		return 3;
	}
	printf("✓ [2a] 匿名登录被拒：0x%08x（%s），origin = %s\n",
	       r, r == TEEC_ERROR_ACCESS_DENIED ? "ACCESS_DENIED" : "其它", orig(o));
	if (o != TEEC_ORIGIN_TRUSTED_APP) {
		printf("   ⚠️ origin 不是 TA —— 那说明拒绝发生在 TA 之前"
		       "（找不到 TA / 签名 / ABI），不是那道门在起作用。\n");
		TEEC_FinalizeContext(&ctx);
		return 3;
	}
	printf("        origin = TA 这一点很要紧：它意味着 **TA 已经被找到、加载、\n"
	       "        并且执行到了自己的 open-session 入口** —— 是它主动拒的。\n");

	/* ---- [2b] 正常登录 ---- */
	memset(&op, 0, sizeof op);
	r = TEEC_OpenSession(&ctx, &sess, &uuid, TEEC_LOGIN_USER, NULL, &op, &o);
	if (r != TEEC_SUCCESS) {
		printf("✗ [2b] TEEC_OpenSession(LOGIN_USER) = 0x%08x，origin = %s\n",
		       r, orig(o));
		printf("   常见成因：tee-supplicant 没在跑（TA 要从普通世界文件系统取）、\n"
		       "   .ta 不在 /lib/optee_armtz/、签名与这份 OP-TEE 的公钥不符、\n"
		       "   或 TA dev kit 与板上 OP-TEE 的 ABI 不是同一版。\n");
		TEEC_FinalizeContext(&ctx);
		return 3;
	}
	printf("✓ [2b] TEEC_OpenSession(LOGIN_USER) —— **TA 已加载并建立实例**\n");

	memset(&op, 0, sizeof op);
	op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_OUTPUT, TEEC_NONE,
					 TEEC_NONE, TEEC_NONE);
	r = TEEC_InvokeCommand(&sess, TA_PQCHSM_CMD_GET_INFO, &op, &o);
	if (r != TEEC_SUCCESS) {
		printf("✗ [3] CMD_GET_INFO = 0x%08x，origin = %s\n", r, orig(o));
		TEEC_CloseSession(&sess);
		TEEC_FinalizeContext(&ctx);
		return 4;
	}
	printf("✓ [3] CMD_GET_INFO —— TA 里的代码执行过并返回：\n");
	printf("        version  = 0x%08x（编译进 TA 的 PQCHSM_TA_PROTO_VERSION）\n",
	       op.params[0].value.a);
	printf("        features = 0x%08x\n", op.params[0].value.b);

	if (op.params[0].value.a != PQCHSM_TA_PROTO_VERSION) {
		printf("✗ 版本对不上：host 期望 0x%08x —— 板上那份 .ta 是旧的\n",
		       PQCHSM_TA_PROTO_VERSION);
		TEEC_CloseSession(&sess);
		TEEC_FinalizeContext(&ctx);
		return 5;
	}
	printf("✓ 版本与 host 一致 —— 板上跑的就是这次编出来的那份 TA\n");

	TEEC_CloseSession(&sess);
	TEEC_FinalizeContext(&ctx);
	printf("\nta_probe: 通过 —— **TEE 主线的前置死结不成立，OP-TEE TA 在这块板上跑通了**\n");
	return 0;
}
