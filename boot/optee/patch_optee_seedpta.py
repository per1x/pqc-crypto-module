#!/usr/bin/env python3
"""给 optee_os 加一个**只转发种子写入**的伪 TA（PTA）。

    python3 boot/optee/patch_optee_seedpta.py /home/build/p4_optee/optee_os

============================================================================
【为什么必须有这一层，不能让 TA 直接发 SMC】
============================================================================
TA 跑在 **S-EL0**，那里发不了 SMC —— SMC 是 EL1 及以上的指令。所以"种子由 TA
保管、经 EL3 写进 PL"这条路上必然要有一个 **S-EL1 的中转**，而在 OP-TEE 里
S-EL1 就是 core 本身。伪 TA（pseudo TA）是 core 提供给 TA 的合法入口。

【这个 PTA 刻意做得极窄】
它只有一条命令，参数是"目标 + 一个 32 位字"，转成一条 SMC 发给 EL3。
它**不是**通用 SMC 转发口 —— 那种东西等于把 EL3 的整个 SiP 表暴露给任何 TA。
UUID 之外还有两道门：

  ① `open_session` 里 `tee_ta_get_calling_session()` 为空就拒 ——
     那意味着调用方是**普通世界**（经 /dev/tee0 直接开 PTA 会话），不是 TA。
     少了这一条，root 只要知道 PTA 的 UUID 就能自己塞种子，
     和 CODE-1 是同一个洞，只是换了个门牌。
  ② `is_user_ta_ctx()`：调用方必须是用户态 TA，不是别的 PTA。

【EL3 那一侧还有一道】
`ZYNQMP_SIP_SVC_PQC_SEED_WORD` 无条件只认安全世界事务。也就是说即便这两道门
都被绕过，普通世界发同一条 SMC 仍然被 EL3 拒掉。两处各自成立，不互相依赖。
"""
import os
import re
import sys

PTA_SRC = r'''// SPDX-License-Identifier: BSD-2-Clause
/*
 * pqchsm_seed.pta —— 把「一个种子字」从 TA（S-EL0）转给 EL3 写进 PL。
 *
 * 由 boot/optee/patch_optee_seedpta.py 生成，别手改这个文件；
 * 要改就改那个脚本，否则下次重建 optee_os 时改动会消失。
 *
 * 极窄的口：一条命令、两个整数参数、一条固定的 SMC。它不是通用 SMC 转发口 ——
 * 那种东西等于把 EL3 的整个 SiP 表暴露给任何一个 TA。
 */
#include <kernel/pseudo_ta.h>
#include <kernel/tee_ta_manager.h>
#include <kernel/thread.h>
#include <kernel/user_ta.h>
#include <trace.h>

#define PTA_PQCHSM_SEED_UUID                                       \
	{ 0x5f2c1b90, 0x9d44, 0x4a3e,                              \
	  { 0xb1, 0x27, 0x6e, 0x53, 0x0c, 0x8a, 0xf4, 0x11 } }

#define PTA_NAME "pqchsm_seed.pta"

/* EL3 侧那条纯通路服务（boot/atf/patch_atf_secmmio.py）。
 * 它无条件只认安全世界调用方 —— 本 PTA 跑在 S-EL1，所以过得去；
 * 普通世界发同一条 SMC 会被 EL3 拒掉。 */
#define ZYNQMP_SIP_SVC_PQC_SEED_WORD	0x8200ff15UL

/* cmd 0：写一个种子字。
 *   params[0].value.a = 目标（0 = ML-KEM，1 = ML-DSA）
 *   params[0].value.b = 那个 32 位字
 *   params[1].value.a = 出参：EL3 回报的「写完之后的字计数」
 * ⚠️ 没有「读回种子」的命令，也不该有。 */
#define PTA_PQCHSM_SEED_CMD_WORD	0

static TEE_Result open_session(uint32_t param_types __unused,
			       TEE_Param params[TEE_NUM_PARAMS] __unused,
			       void **sess_ctx __unused)
{
	struct tee_ta_session *s = NULL;

	/* 调用方必须是一个 TA。为空意味着请求来自**普通世界**
	 * （经 /dev/tee0 直接开 PTA 会话）—— 拒。
	 * 少了这一条，root 只要知道 UUID 就能自己塞种子。 */
	s = tee_ta_get_calling_session();
	if (!s)
		return TEE_ERROR_ACCESS_DENIED;
	if (!is_user_ta_ctx(s->ctx))
		return TEE_ERROR_ACCESS_DENIED;

	return TEE_SUCCESS;
}

static TEE_Result invoke_command(void *sess_ctx __unused, uint32_t cmd_id,
				 uint32_t param_types,
				 TEE_Param params[TEE_NUM_PARAMS])
{
	unsigned long r;

	if (cmd_id != PTA_PQCHSM_SEED_CMD_WORD)
		return TEE_ERROR_NOT_IMPLEMENTED;

	if (param_types != TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
					   TEE_PARAM_TYPE_VALUE_OUTPUT,
					   TEE_PARAM_TYPE_NONE,
					   TEE_PARAM_TYPE_NONE))
		return TEE_ERROR_BAD_PARAMETERS;

	if (params[0].value.a > 1U)
		return TEE_ERROR_BAD_PARAMETERS;

	r = thread_smc(ZYNQMP_SIP_SVC_PQC_SEED_WORD,
		       (unsigned long)params[0].value.a,
		       (unsigned long)params[0].value.b, 0);
	if (r != 0UL)
		return TEE_ERROR_GENERIC;

	/* thread_smc 只带回 x0。字计数要单独问 —— 但**问不到**，
	 * 因为读 SEED_STAT 走的是通用 PL_RD，而那条对种子偏移是放行的
	 * （SEED_STAT 不是 SEED_DATA，它一个种子字节都不报）。
	 * 这里不去问：调用方自己数写了几个字就够了，少一条路少一份面。 */
	params[1].value.a = 0U;

	return TEE_SUCCESS;
}

pseudo_ta_register(.uuid = PTA_PQCHSM_SEED_UUID, .name = PTA_NAME,
		   .flags = PTA_DEFAULT_FLAGS,
		   .open_session_entry_point = open_session,
		   .invoke_command_entry_point = invoke_command);
'''


def main():
    tree = sys.argv[1] if len(sys.argv) > 1 else '/home/build/p4_optee/optee_os'
    pta_dir = os.path.join(tree, 'core', 'pta')
    if not os.path.isdir(pta_dir):
        sys.exit('找不到 %s —— 第一个参数要指向 optee_os 树根' % pta_dir)

    src = os.path.join(pta_dir, 'pqchsm_seed.c')
    with open(src, 'w') as f:
        f.write(PTA_SRC)
    print('PTA：已写 %s' % src)

    sub = os.path.join(pta_dir, 'sub.mk')
    with open(sub) as f:
        mk = f.read()
    if 'pqchsm_seed.c' in mk:
        print('sub.mk：已经有了，不重复加')
    else:
        # 挂在无条件那一组里（不加 CFG_ 开关：这个 PTA 是本项目镜像的一部分，
        # 一个可以忘记打开的开关只会制造「为什么 TA 找不到 PTA」那类问题）
        mk = mk.rstrip('\n') + '\nsrcs-y += pqchsm_seed.c\n'
        with open(sub, 'w') as f:
            f.write(mk)
        print('sub.mk：已加 srcs-y += pqchsm_seed.c')

    # 自检：注册宏与两道门都要在
    body = PTA_SRC
    for need, why in (
            ('pseudo_ta_register', '没有注册宏，PTA 不会被链接进 core'),
            ('tee_ta_get_calling_session', '少了「拒普通世界」那道门'),
            ('is_user_ta_ctx', '少了「必须是用户 TA」那道门'),
            ('0x8200ff15', 'SMC 号不对'),
    ):
        if need not in body:
            sys.exit('自检失败：%s（%s）' % (need, why))
    print('自检：注册宏 + 两道门 + SMC 号都在')
    return 0


if __name__ == '__main__':
    sys.exit(main())
