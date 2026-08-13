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

#define SECMMIO_MAGIC   'S'
/* 解除保险。**必须由用户态先确认 PL 已 programmed 再调**（见模块文件头）。 */
#define SECMMIO_ARM     _IO(SECMMIO_MAGIC, 0)
#define SECMMIO_RD      _IOWR(SECMMIO_MAGIC, 1, struct secmmio_op)
#define SECMMIO_WR      _IOW(SECMMIO_MAGIC, 2, struct secmmio_op)

#endif
