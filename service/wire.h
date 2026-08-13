/* wire.h —— 库与 daemon 之间的线格式（两侧共用一份定义）
 *
 * 放一处而不是两边各抄：这两侧对不上的症状是"某个字段读出垃圾"，
 * 不是编译错误 —— 那种错查起来极贵（secmmio_uapi.h 同理）。
 *
 * 定长头 + 变长载荷，长度前缀。故意做得很朴素：这是原型的控制平面，
 * 每秒几百次调用，没有任何理由上更复杂的编码。
 */
#ifndef PQCHSM_WIRE_H
#define PQCHSM_WIRE_H

#include <stdint.h>

#define PQCS_MAGIC   0x53434750u   /* "PQCS" */
#define PQCS_MAXPAY  8192u

enum {
	OP_PING          = 1,   /* → 版本串 */
	OP_RANDOM        = 2,   /* a0=字节数 → 随机字节 */
	OP_MLKEM_KEYGEN  = 3,   /* a0=pset → [4 字节句柄][ek] */
	OP_MLKEM_ENCAPS  = 4,   /* a0=pset, 载荷=ek → [32 字节 K][c] */
	OP_MLKEM_DECAPS  = 5,   /* a0=句柄, 载荷=c → [32 字节 K] */
	OP_IMPORT_KEY    = 6,   /* a0=槽, 载荷=key */
	OP_SYM_BLOCK     = 7,   /* a0=alg, a1=槽|(解密<<8), 载荷=16 字节 → 16 字节 */
};

struct pqcs_req {
	uint32_t magic;
	uint32_t op;
	uint32_t a0;
	uint32_t a1;
	uint32_t len;      /* 载荷字节数 */
};

struct pqcs_resp {
	uint32_t magic;
	uint32_t status;   /* 0 = 成功，否则是 SDR_* */
	uint32_t len;
};

#define PQCS_SOCK_PATH "/tmp/pqchsm_fpgad.sock"

#endif
