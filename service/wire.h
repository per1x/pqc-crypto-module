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

/* 单条载荷上限。
 *
 * 从 8192 提到 16384 是 ML-DSA 逼出来的，不是"顺手放宽"：Verify 一条请求要同时
 * 送 pk‖sig‖msg，ML-DSA-87 光 pk(2592)+sig(4627) 就占掉 7219 字节，按老上限
 * 只剩不到 1 KB 留给消息。而 sign.v 的 msg 缓冲本身就有 8192 字节
 * （见 docs/reference/mldsa-sign-design.zh-CN.md §4），也就是说老上限会让
 * **软件这一侧先于硬件成为瓶颈** —— 那种限制查起来最费劲，因为它不在硬件手册里。
 *
 * 16384 = 7219 + 255(ctx 上限) + 8192 还余 718，正好把硬件的能力完整暴露出来。
 * 代价是 daemon 那两个 static 缓冲各多 8 KB，可以忽略。
 * ⚠️ 这是**协议常量**：两侧必须同版本编译，否则新客户端的长请求会被老 daemon
 *    按"len 超限"断开（fail-closed，不会算错，但表现为莫名其妙的掉线）。 */
#define PQCS_MAXPAY  16384u

enum {
	OP_PING          = 1,   /* → 版本串 */
	OP_RANDOM        = 2,   /* a0=字节数 → 随机字节 */
	OP_MLKEM_KEYGEN  = 3,   /* a0=pset → [4 字节句柄][ek] */
	OP_MLKEM_ENCAPS  = 4,   /* a0=pset, 载荷=ek → [32 字节 K][c] */
	OP_MLKEM_DECAPS  = 5,   /* a0=句柄, 载荷=c → [32 字节 K] */
	OP_IMPORT_KEY    = 6,   /* a0=槽, 载荷=key */
	OP_SYM_BLOCK     = 7,   /* a0=alg, a1=槽|(解密<<8), 载荷=16 字节 → 16 字节 */
	OP_AUTH          = 8,   /* 载荷=口令 → 空。**只有 TCP 连接需要** */

	/* ---- ML-DSA（mldsa_axi @ 0x8006_0000）--------------------------------
	 * 从机已落地（槽 6），这三条 2026-08-17 在真硬件上端到端跑通：两种位流形态
	 * 下三个参数集各走一遍 KeyGen → Sign → Verify，见 board/logs/。
	 * 寄存器映射以 hardware/rtl/bus/mldsa_axi.v 的 A_* 为准，
	 * pqchsm_fpgad.c 的 MD_* 必须逐行对着它核 —— MODE/STATUS 曾经在那里写反过，
	 * 而症状是"每次等 done 超时"，与"核算不出来"分不开。 */
	OP_MLDSA_KEYGEN  = 9,   /* a0=pset → [4 字节句柄][pk]（sk 进片内金库，不出总线） */
	OP_MLDSA_SIGN    = 10,  /* a0=句柄, a1=ctx_len, 载荷=msg → sig */
	OP_MLDSA_VERIFY  = 11,  /* a0=pset, a1=ctx_len, 载荷=pk‖sig‖msg → 空 */

	/* ---- 分组密码的**工作模式**（CBC/CTR/CFB/OFB）--------------------------
	 * a0 = alg | (解密<<8) | (模式<<16)，a1 = 槽，
	 * 载荷 = IV(16) ‖ 数据 → 等长的数据
	 *
	 * ⚠️ **链接与异或在 daemon 里，分组变换在硬件里。** 每一个分组都走一次
	 *    sym_axi（密钥仍然只按槽号使唤，一个字节都不出金库），但 CBC 的异或、
	 *    CTR 的计数器都是本进程算的。**不要**把这说成"硬件 CBC" ——
	 *    RTL 里没有模式状态机，只有单分组变换。
	 *
	 * ⚠️ **不做填充。** 长度语义按模式分两类，理由是错误的填充比没有填充更
	 *    危险（padding oracle）：
	 *      ECB/CBC：数据长度必须是 16 的非零整数倍，否则 SDR_INARGERR；
	 *      CTR/CFB/OFB：任意长度，末尾不足一整块就截断（标准做法）。
	 *
	 * ⚠️ **CTR/OFB/CFB 的 (密钥, IV) 绝不能重复**：重复即等于把密钥流复用，
	 *    两条密文异或就把明文异或暴露出来。daemon 不替调用方生成 IV，
	 *    也就不替它承担这个责任 —— 这条写在 sdfe.h 的接口注释里。 */
	OP_SYM_CRYPT     = 12,

	/* ---- SM3 杂凑（sym_axi 里那个核，一直在位流里但从没接出来）----------
	 * 载荷 = 待杂凑的数据 → 32 字节摘要。无密钥，不碰金库。
	 *
	 * ⚠️ 一次一段（one-shot），**不是** GM/T 0018 的 Init/Update/Final 三段式。
	 *    三段式要在 daemon 里替每个连接存一份杂凑中间态，而 sym_axi 里只有
	 *    一份 SM3 上下文 —— 两个会话交错 Update 会把彼此的状态搅在一起，
	 *    算出来的摘要合法但错。要做三段式得先在 RTL 里给上下文分槽，
	 *    那是另一件事。**现在明确只提供 one-shot，别假装支持流式。** */
	OP_SM3           = 13,

	/* 设备 DNA：a0/a1 未用，返回 16 字节（0xFFCA0050-5C 四个字，大端拼接）。
	 *
	 * ⚠️ **DNA 不是秘密**，所以这条操作不需要额外权限 —— 有 JTAG 的人本来就
	 *    能读到它。它存在的唯一理由是：让**跑在远端主机上**的 p11 库也能把
	 *    keystore 绑到这块板（防克隆）。别把"能读 DNA"当成一个需要保护的能力，
	 *    也别把"绑到 DNA"说成"受硬件保护"。 */
	OP_DEVICE_DNA    = 14,
};

/* OP_SYM_CRYPT 的模式编号。数值与任何标准无关，只是本线格式内部的约定；
 * GM/T 0018 的 SGD_SM4_CBC 之类算法 ID 由上层（libsdfe）映射过来。 */
enum {
	PQCS_MODE_ECB = 0,
	PQCS_MODE_CBC = 1,
	PQCS_MODE_CTR = 2,
	PQCS_MODE_CFB = 3,   /* 全分组反馈（CFB-128） */
	PQCS_MODE_OFB = 4,
};

/* ============================================================================
 * 【TCP 前端】
 * ============================================================================
 * 除本机 UNIX socket 外，daemon 还可以监听一个 TCP 端口，好让**另一台机器**
 * 远程调用密码机（内网演示用）。协议逐字节相同，只多一条规矩：
 *
 *   **TCP 连接的第一条请求必须是 OP_AUTH，口令不对就断开。**
 *
 * 口令从 /media/sd-mmcblk1p2/hsm/hsm_token 读（0600）。**没有这个文件就
 * 根本不监听 TCP** —— fail-closed：宁可远程用不了，也不要裸奔一个能驱动
 * 密码机的端口。
 *
 * 这不是完整的访问控制（没有身份、没有权限分级、没有传输加密），
 * 它只挡住"同网段任何人都能直接使唤密码机"这一条。完整的 ACL 与 TLS
 * 是送检口径的事，见 docs/API.md。**别把这层当安全边界** ——
 * 真正的边界在 PL 的防火墙上，那一层与谁在网上说话无关。
 */
#define PQCS_TCP_PORT   9797
#define PQCS_TOKEN_PATH "/media/sd-mmcblk1p2/hsm/hsm_token"
#define PQCS_TOKEN_MAX  128

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
