/* pqchsm/accel.h —— 硬件加速器抽象（路线图 §5.7.1 + §5.8.2）
 *
 * 【这一层解决的问题】
 * §5.7.1 的原话：**先把 AXI 寄存器表定死，然后写一个 C 实现的"假加速器"**，
 * 它暴露与真 PL 完全相同的寄存器语义（写 CTRL.START → 轮询 STATUS.DONE →
 * 读结果），内部调 liboqs 完成实际运算。真板到手后换成 /dev/mem + mmap 即可，
 * **上层槽位管理器、密钥库、PKCS#11 一行不改**。
 *
 * 直接收益：接口约定的 bug 在无板阶段就清光了，板上联调只剩真正的硬件问题。
 *
 * 【三种 transport】
 *   stub       软件桩，内部调 liboqs                    —— 现在就能跑，全套测试通过
 *   verilator  驱动 Verilator 仿真出来的真 RTL          —— 目前只实现了 NTT 模式
 *   mmap       /dev/mem + mmap 打真 PL                  —— Phase 3+，待板子
 *
 * 三者实现同一张 accel_transport_t，pqc_accel.c 之上完全不区分。
 *
 * 【寄存器表】（§5.8.2 要求在写 RTL 之前定死）
 *   偏移  名字      读写  说明
 *   0x00  CTRL      W     [0]=START（写 1 触发）[1]=SOFT_RESET
 *   0x04  STATUS    R     [0]=DONE [1]=BUSY [2]=ERR
 *   0x08  MODE      RW    操作码，见 accel_mode_t
 *   0x0C  PARAM     RW    参数集（pqc_alg_t 的数值）
 *   0x10  IN_LEN    RW    输入字节数
 *   0x14  OUT_LEN   R     输出字节数（由加速器回填）
 *   0x18  ERRCODE   R     出错时的细分原因
 *
 * 数据不走寄存器：真 PL 上是 AXI-DMA 搬运，所以这里单列 write_data/read_data，
 * 对应"把一块缓冲送进去 / 取出来"，而不是逐字读寄存器。
 */
#ifndef PQCHSM_ACCEL_H
#define PQCHSM_ACCEL_H

#include "pqchsm/pqc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 寄存器偏移 ---- */
#define ACCEL_REG_CTRL    0x00u
#define ACCEL_REG_STATUS  0x04u
#define ACCEL_REG_MODE    0x08u
#define ACCEL_REG_PARAM   0x0Cu
#define ACCEL_REG_IN_LEN  0x10u
#define ACCEL_REG_OUT_LEN 0x14u
#define ACCEL_REG_ERRCODE 0x18u

/* CTRL 位 */
#define ACCEL_CTRL_START      (1u << 0)
#define ACCEL_CTRL_SOFT_RESET (1u << 1)

/* STATUS 位 */
#define ACCEL_ST_DONE (1u << 0)
#define ACCEL_ST_BUSY (1u << 1)
#define ACCEL_ST_ERR  (1u << 2)

/* 操作码 */
typedef enum {
	ACCEL_MODE_IDLE          = 0,
	ACCEL_MODE_KEM_KEYGEN    = 1,   /* in: seed(64)        out: pk‖sk        */
	ACCEL_MODE_KEM_ENCAPS    = 2,   /* in: pk‖m(32)        out: ct‖ss        */
	ACCEL_MODE_KEM_DECAPS    = 3,   /* in: sk‖ct           out: ss           */
	ACCEL_MODE_SIG_KEYGEN    = 4,   /* in: seed(32)        out: pk‖sk        */
	ACCEL_MODE_SIG_SIGN      = 5,   /* in: sk‖rnd‖ctx_len‖ctx‖msg  out: sig  */
	ACCEL_MODE_SIG_VERIFY    = 6,   /* in: pk‖siglen‖sig‖ctx_len‖ctx‖msg     */
	ACCEL_MODE_NTT_FWD       = 7,   /* in: 256×int16       out: 256×int16    */
	ACCEL_MODE_NTT_INV       = 8,
} accel_mode_t;

/* 数据缓冲上限：够放最大的 pk‖sk（ML-DSA-87: 2592+4896）与签名输入 */
#define ACCEL_BUF_MAX 16384

typedef struct accel_transport {
	const char *name;
	/* 该 transport 是否具备真实硬件语义（stub / 仿真为 0） */
	int is_hardware;

	int      (*reset)(void);
	void     (*write_reg)(uint32_t off, uint32_t val);
	uint32_t (*read_reg)(uint32_t off);
	void     (*write_data)(uint32_t off, const uint8_t *src, size_t n);
	void     (*read_data)(uint32_t off, uint8_t *dst, size_t n);
} accel_transport_t;

/* 软件桩：内部调 liboqs，暴露与真 PL 相同的寄存器语义（§5.7.1） */
const accel_transport_t *accel_transport_stub(void);

/* Verilator 仿真：驱动真 RTL。**目前只实现 NTT 模式**，
 * 其余模式置 ERR —— 完整的 ML-KEM/ML-DSA 核属于路线图 Phase 1–4。
 * 没编 Verilator 支持时返回 NULL。 */
const accel_transport_t *accel_transport_verilator(void);

/* 选择当前 transport；NULL 恢复默认（stub） */
void                     accel_set_transport(const accel_transport_t *t);
const accel_transport_t *accel_get_transport(void);

/* 经寄存器接口实现的 pqc_backend_t —— 与 pqc_backend_liboqs() 可互换 */
const pqc_backend_t *pqc_backend_accel(void);

/* 直接跑一次 NTT（给 RTL 对拍用；正常密码路径不经这里）。
 * in/out 都是 256 个 int16，小端。成功返回 0。 */
int accel_ntt(const int16_t *in, int16_t *out, int inverse);

#ifdef __cplusplus
}
#endif
#endif /* PQCHSM_ACCEL_H */
