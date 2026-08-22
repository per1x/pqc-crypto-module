/* secmmio_uapi.h —— 内核模块与用户态共用的 ioctl 约定
 *
 * 放在一处而不是两边各抄一份：这两侧一旦对不上，症状是"某个寄存器读出垃圾"，
 * 而不是编译错误 —— 那种错查起来极贵。
 */
#ifndef PQCHSM_SECMMIO_UAPI_H
#define PQCHSM_SECMMIO_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct secmmio_op {
	__u32 addr;    /* PL 物理地址（白名单在 EL3 里判，不在这里） */
	__u32 val;     /* 写：入参；读：出参 */
};

/* 种子装载：**只发一条命令，不传也不收任何密钥材料**。
 *
 * target 是目标（0 = ML-KEM，1 = ML-DSA），world 回填调用方世界
 * （1 = 安全世界）。种子由 EL3 自己取熵、自己写进 PL 的暂存口，
 * 普通世界从头到尾看不到明文 —— 这是 CODE-1 的修复（见
 * boot/atf/patch_atf_secmmio.py 里 ZYNQMP_SIP_SVC_PQC_SEED 那段）。
 *
 * ⚠️ 结构里**没有任何能放种子的字段**，这是有意的：接口形状本身就该
 *    说明"这条路上不流通密钥材料"。 */
struct secmmio_seed {
	__u32 target;   /* 入参：0 = ML-KEM，1 = ML-DSA */
	__u32 world;    /* 出参：调用方世界（1 = 安全世界），供上层如实记录 */
};

#define SECMMIO_MAGIC   'S'
/* 解除保险。**必须由用户态先确认 PL 已 programmed 再调**（见模块文件头）。 */
#define SECMMIO_ARM     _IO(SECMMIO_MAGIC, 0)
#define SECMMIO_RD      _IOWR(SECMMIO_MAGIC, 1, struct secmmio_op)
#define SECMMIO_WR      _IOW(SECMMIO_MAGIC, 2, struct secmmio_op)
#define SECMMIO_SEED    _IOWR(SECMMIO_MAGIC, 3, struct secmmio_seed)

#endif
