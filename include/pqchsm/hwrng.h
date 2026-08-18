/* pqchsm/hwrng.h —— 硬件熵源抽象
 *
 * 【这一层解决的问题】
 * 与 accel.h 同一个套路：**先把 AXI 寄存器表定死，再写一个 C 实现的"假 TRNG"**，
 * 它暴露与 PL 里 trng_axi 完全相同的寄存器语义（暖机 → READY → 读 RDATA 弹字），
 * 内部用软件随机源填 FIFO。真板到手后换成 /dev/mem + mmap，**上层一行不改**。
 *
 * 寄存器表与行为契约的权威文档是 docs/REGISTERS.zh-CN.md，
 * RTL 侧由 hardware/tb/cocotb/test_trng_axi.py 逐条验证，
 * 软件侧由 tests/unit/test_hwrng.c 对着同一份契约验证。
 * 两边跑的是同一张表 —— 这正是无板阶段能清掉接口 bug 的原因。
 *
 * 【与软件 RNG 的关系：替换，不是回退】
 * 装上一个 transport 之后，pqc_random_bytes() 与 liboqs 的随机源都改从这里取。
 * 取不到时**返回错误，绝不回退到软件随机源** —— 静默回退会让"熵来自硬件"
 * 变成假象，而这正是密码机最不能含糊的一条。没装 transport 时一切照旧走
 * OpenSSL，所以主机上的既有测试不受影响。
 */
#ifndef PQCHSM_HWRNG_H
#define PQCHSM_HWRNG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 寄存器偏移（对应 hardware/rtl/trng/trng_axi.v）---- */
#define HWRNG_REG_CTRL      0x00u
#define HWRNG_REG_STATUS    0x04u
#define HWRNG_REG_RDATA     0x08u   /* 读一次弹出一个 32 位字 */
#define HWRNG_REG_HEALTH    0x0Cu   /* [31:16]=APT 计数 [15:0]=RCT 游程 */
#define HWRNG_REG_APT_INDEX 0x10u
#define HWRNG_REG_STARTUP   0x14u
#define HWRNG_REG_BLOCKS    0x18u
#define HWRNG_REG_WORDS     0x1Cu
#define HWRNG_REG_VERSION   0x20u
#define HWRNG_REG_PARAM0    0x24u   /* {DECIM, NUM_RO, RATE_LANES, OUT_LANES} */
#define HWRNG_REG_PARAM1    0x28u   /* {APT_CUTOFF[15:0], RCT_CUTOFF[15:0]}   */
#define HWRNG_REG_PARAM2    0x2Cu   /* {STARTUP_SAMPLES[15:0], APT_WINDOW}    */

/* CTRL 位 */
#define HWRNG_CTRL_ENABLE      (1u << 0)   /* 电平 */
#define HWRNG_CTRL_ZEROIZE     (1u << 1)   /* 写 1 脉冲 */
#define HWRNG_CTRL_CLEAR_ALARM (1u << 2)   /* 写 1 脉冲 */

/* STATUS 位 */
#define HWRNG_ST_READY        (1u << 0)
#define HWRNG_ST_DATA_VALID   (1u << 1)
#define HWRNG_ST_ALARM        (1u << 2)   /* 锁存 */
#define HWRNG_ST_RCT_ALARM    (1u << 3)
#define HWRNG_ST_APT_ALARM    (1u << 4)
#define HWRNG_ST_STARTUP_DONE (1u << 5)
#define HWRNG_ST_FIFO_WIPING  (1u << 6)
#define HWRNG_ST_ENABLED      (1u << 7)
#define HWRNG_ST_UNDERRUN     (1u << 8)   /* 锁存 */

#define HWRNG_VERSION_EXPECTED 0x00010000u

/* 驱动编译期常量，与 RTL 默认参数一致。初始化时读 PARAM0/1/2 比对 ——
 * "改了 RTL 参数忘了改驱动"这类不一致在真系统里出过事：健康检测看起来
 * 一切正常，实际判据已经不是标准要求的那个了。 */
#define HWRNG_EXPECT_DECIM           8u
#define HWRNG_EXPECT_NUM_RO          8u
#define HWRNG_EXPECT_RATE_LANES      17u
#define HWRNG_EXPECT_OUT_LANES       4u
#define HWRNG_EXPECT_RCT_CUTOFF      41u
#define HWRNG_EXPECT_APT_CUTOFF      793u
#define HWRNG_EXPECT_APT_WINDOW      1024u
#define HWRNG_EXPECT_STARTUP_SAMPLES 1024u

/* ---- 错误码 ---- */
typedef enum {
	HWRNG_OK          = 0,
	HWRNG_ERR_ARG     = -1,
	HWRNG_ERR_ABSENT  = -2,   /* 没装 transport */
	HWRNG_ERR_ALARM   = -3,   /* 健康检测告警：这一批数据全部作废 */
	HWRNG_ERR_TIMEOUT = -4,   /* 等 READY / DATA_VALID 超时 */
	HWRNG_ERR_PARAM   = -5,   /* PARAM 回读与驱动编译期常量不符 */
	HWRNG_ERR_VERSION = -6,
} hwrng_err_t;

/* ---- transport：怎么把寄存器读写发出去 ---- */
typedef struct hwrng_transport {
	const char *name;
	/* 是否具备真实硬件语义（软件模型为 0）。上层据此决定要不要相信
	 * "熵来自硬件"这句话 —— 见 hwrng_is_hardware()。 */
	int is_hardware;

	void     (*write_reg)(uint32_t off, uint32_t val);
	uint32_t (*read_reg)(uint32_t off);
} hwrng_transport_t;

/* ---- 第二种接缝：**按字节**取，而不是按寄存器 ----
 *
 * 上面那个 transport 是寄存器级的，前提是调用方够得到 TRNG 的寄存器。
 * 在交付形态里这个前提**结构上不成立**：四个密码核都是 SECURE_ONLY=1，
 * 普通世界的每一笔访问都被 PL 的防火墙拒掉（读回 0）。寄存器级的
 * transport 在用户态根本没法实现 —— 不是没写，是写不出来。
 *
 * 那条形态下唯一通向硬件熵源的路是：经 pqchsm_fpgad → /dev/secmmio →
 * EL3 的 SiP → trng_axi。而那条路的接口是**字节**（SDFE_GenerateRandom），
 * 不是寄存器 —— daemon 不会、也不该把任意寄存器读写代理出来，
 * 那等于在密码边界上开一个后门。
 *
 * 所以这里加第二种接缝。装上字节源之后：
 *   · hwrng_bytes() 走它；
 *   · hwrng_available() / hwrng_is_hardware() 如实反映它；
 *   · 那些寄存器级的接口（hwrng_status / hwrng_zeroize / 健康计数）
 *     **不可用**，因为它们要的东西这条路上取不到。这一点没有回退：
 *     假装能读到状态比读不到更糟。
 */
typedef struct hwrng_byte_source {
	const char *name;
	int         is_hardware;
	/* 返回 0 成功。取不到就失败，**不许回退到软件源** ——
	 * 静默回退会让"熵来自硬件"恰好在最要紧的时刻悄悄变成假话。 */
	int (*bytes)(uint8_t *out, size_t n);
} hwrng_byte_source_t;

void hwrng_set_byte_source(const hwrng_byte_source_t *src);

/* 经密码机服务（pqchsm_fpgad）取硬件熵源。本机走 UNIX socket；
 * 设了 PQCHSM_SDFE_HOST + PQCHSM_SDFE_PKI 就走 TCP（远程演示，**mTLS**）。
 * ⚠️ 老的 PQCHSM_SDFE_TOKEN 已经没有任何作用 —— 远程口不再是预共享口令。 */
const hwrng_byte_source_t *hwrng_byte_source_sdfe(void);
const hwrng_byte_source_t *hwrng_get_byte_source(void);

/* 软件模型：逐位复现 trng_axi 的寄存器语义（暖机计数、FIFO 深度、
 * 空读锁存 UNDERRUN、告警锁存、ZEROIZE 清池重跑启动检测），
 * FIFO 内容由 OpenSSL 填。始终可用。 */
const hwrng_transport_t *hwrng_transport_stub(void);

/* /dev/mem + mmap 打真 PL。基址由构建时的 PQCHSM_HWRNG_MMAP_BASE 给出；
 * 未定义（或不在 Linux 上）时返回 NULL —— 如实反映"这条路没有可用的目标"。 */
const hwrng_transport_t *hwrng_transport_mmap(void);

/* 选择当前 transport；NULL 表示卸掉（回到 OpenSSL 软件随机源）。
 * 装载时会跑一次 hwrng_selftest()，不过不返回结果 —— 结果要自己查。 */
void                     hwrng_set_transport(const hwrng_transport_t *t);
const hwrng_transport_t *hwrng_get_transport(void);

/* 当前是否有 transport（不论真假硬件） */
int hwrng_available(void);
/* 当前 transport 是否是真硬件。审计与自检报告里要如实记这一条。 */
int hwrng_is_hardware(void);

/* 启动自检：VERSION 与 PARAM0/1/2 回读比对、等暖机、检查无告警。
 * 成功返回 HWRNG_OK。驱动初始化时必须调，失败就不该往下用。 */
int hwrng_selftest(void);

/* 取 n 字节。严格照 docs/REGISTERS.zh-CN.md 的契约：
 * 每次读 RDATA 之前先查 DATA_VALID，取完之后复查 UNDERRUN 与 ALARM，
 * 任一置位则**整批作废**（缓冲区清零）并返回 HWRNG_ERR_ALARM。
 * 成功返回 HWRNG_OK。 */
int hwrng_bytes(uint8_t *out, size_t n);

/* 读 STATUS 原值；没装 transport 返回 0 */
uint32_t hwrng_status(void);

/* 触发 ZEROIZE：擦 FIFO、清海绵、重跑启动检测。与 tamper 引脚等价。 */
void hwrng_zeroize(void);

/* 清告警。注意这会**重跑启动健康检测**，在 READY 重新拉高之前取不到数。 */
void hwrng_clear_alarm(void);

#ifdef __cplusplus
}
#endif
#endif /* PQCHSM_HWRNG_H */
