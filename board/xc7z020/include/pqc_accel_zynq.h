/* pqc_accel_zynq.h —— Zynq-7000（XC7Z020）上的真实 MMIO 后端
 *
 * 这一层把 include/pqchsm/accel.h 的 accel_transport_t 落到真实硬件上：
 * 控制面对 PL 里的 AXI4-Lite 寄存器组做 volatile 读写，数据面由 AXI-DMA 在
 * DDR 与 PL 之间搬运。命令时序与 docs/register-map.md 完全一致，
 * 与仿真用的 accel_axi.c 也完全一致 —— 区别只在事务怎么发出去。
 *
 * 【地址表】必须与 board/xc7z020/vivado/create_project.tcl 里的地址映射一致。
 * 两边对不上时软件读到的是别的外设，而且读得到值、只是值没有意义，
 * 这类故障在板上最难查，所以两处都写明"改一处必须改另一处"。
 *
 * 【两种映射方式】
 *   UIO       /dev/uioN，由设备树把 PL 的地址段绑给 generic-uio。
 *             推荐：不需要 root 之外的特权，且内核知道这段地址被谁占着。
 *   /dev/mem  直接映射物理地址。任何板子上都能用，但需要 root，
 *             而且没有任何人阻止另一个进程同时映射同一段。
 *
 * 【DMA 缓冲】AXI-DMA 看到的是物理地址，因此输入输出缓冲必须是物理连续的，
 * 且其物理地址要能被拿到。两种来源：
 *   - 设备树里的 reserved-memory 段（本目录 petalinux/ 下有现成的节点），
 *     物理地址固定，编译时以 PQC_ZYNQ_DMA_BUF_PHYS 给出；
 *   - PYNQ 的 pynq.allocate（CMA），物理地址运行时才知道，
 *     那条路径走 Python，不经过本文件。
 */
#ifndef PQC_ACCEL_ZYNQ_H
#define PQC_ACCEL_ZYNQ_H

#include "pqchsm/accel.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 地址表：与 create_project.tcl 的地址映射一一对应 ---- */
#define PQC_ZYNQ_ACCEL_BASE  0x43C00000u   /* 加速器 AXI4-Lite 寄存器组 */
#define PQC_ZYNQ_ACCEL_SPAN  0x00010000u
#define PQC_ZYNQ_DMA_BASE    0x40400000u   /* AXI-DMA 寄存器组 */
#define PQC_ZYNQ_DMA_SPAN    0x00010000u

/* DMA 缓冲的物理地址与大小。默认值对应 petalinux/system-user.dtsi 里
 * 保留的那一段；换成别的保留段时两边一起改。 */
#ifndef PQC_ZYNQ_DMA_BUF_PHYS
#define PQC_ZYNQ_DMA_BUF_PHYS 0x3E000000u
#endif
#ifndef PQC_ZYNQ_DMA_BUF_SPAN
#define PQC_ZYNQ_DMA_BUF_SPAN 0x00010000u   /* 64 KiB，输入输出各占一半 */
#endif

/* ---- AXI-DMA 寄存器偏移（simple mode，不开 scatter-gather）---- */
#define DMA_MM2S_DMACR   0x00u
#define DMA_MM2S_DMASR   0x04u
#define DMA_MM2S_SA      0x18u
#define DMA_MM2S_LENGTH  0x28u
#define DMA_S2MM_DMACR   0x30u
#define DMA_S2MM_DMASR   0x34u
#define DMA_S2MM_DA      0x48u
#define DMA_S2MM_LENGTH  0x58u

#define DMA_CR_RS        (1u << 0)   /* 运行/停止 */
#define DMA_CR_RESET     (1u << 2)
#define DMA_SR_HALTED    (1u << 0)
#define DMA_SR_IDLE      (1u << 1)
#define DMA_SR_ERR_MASK  0x770u      /* 各类错误位 */

/* ---- 映射方式 ---- */
typedef enum {
	PQC_ZYNQ_MAP_AUTO = 0,   /* 先试 UIO，找不到再退到 /dev/mem */
	PQC_ZYNQ_MAP_UIO,
	PQC_ZYNQ_MAP_DEVMEM,
	PQC_ZYNQ_MAP_CUSTOM,     /* 由 cfg.mapper 提供，测试用 */
} pqc_zynq_map_mode_t;

typedef struct {
	pqc_zynq_map_mode_t mode;
	uint32_t accel_base;
	uint32_t dma_base;
	uint32_t buf_phys;
	uint32_t buf_span;
	/* PQC_ZYNQ_MAP_CUSTOM 时用：返回一段可读写的映射，失败返回 NULL。
	 * 存在的意义是让寄存器时序能在没有板子的情况下对着硬件模型验证。 */
	void *(*mapper)(uint32_t phys, uint32_t span, void *user);
	void  (*unmapper)(void *addr, uint32_t span, void *user);
	void *user;
} pqc_zynq_config_t;

/* 用默认配置填一份（AUTO 映射 + 头文件里的地址表） */
void pqc_zynq_default_config(pqc_zynq_config_t *cfg);

/* 打开设备。成功返回 0；失败返回负值并且 transport 保持不可用。 */
int  pqc_zynq_open(const pqc_zynq_config_t *cfg);
void pqc_zynq_close(void);

/* 打开之后可用；未打开时返回 NULL —— 如实反映，不静默退回软件 */
const accel_transport_t *accel_transport_zynq(void);

/* 上一条命令从写 START 到读到 DONE 之间轮询了多少次。
 * 板上用来判断"命令是真的在跑"还是"寄存器根本没响应"。 */
uint64_t pqc_zynq_last_poll_count(void);

#ifdef __cplusplus
}
#endif
#endif /* PQC_ACCEL_ZYNQ_H */
