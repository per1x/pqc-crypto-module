/* xmpu_protection_draft.c —— TZDRAM 围护：DDR XMPU 寄存器配置（FSBL 补丁草案）
 *
 * 用法：编入 meta-user/recipes-bsp/fsbl 的补丁 FSBL（与看门狗补丁同渠道），
 * 在 xfsbl_handoff.c 的 psu_protection() 中调用 pqchsm_xmpu_protect_tzdram()，
 * 再由 psu_protection_lock() 走 pqchsm_xmpu_lock()（或直接在这里锁）。
 * 同时去掉 xfsbl_hw.h 的 XFSBL_PROT_BYPASS（否则 psu_protection 不被调用）。
 *
 * 寄存器事实来源：QEMU ZynqMP XMPU 设备模型（xlnx-zynqmp-xmpu.c，由寄存器
 * 规范自动生成）+ PMUFW xpfw_xpu.c + XAPP1320。设计文档：
 * docs/xmpu-xppu-isolation.zh-CN.md。
 *
 * ⚠️ 必须先有看门狗+黄金镜像回退再上板：配错 XMPU 会拦死 DDR 流量。
 * ⚠️ 本文件是草案：尚未上板验证，位定义上板前按 UG1087 复核一遍。
 */

#include "xfsbl_hw.h"

/* ---- 6 个 DDR XMPU 实例基地址（全部广播同一配置，防绕行端口）---- */
#define XMPU_DDR0_BASE  0xFD000000U
#define XMPU_DDR1_BASE  0xFD010000U
#define XMPU_DDR2_BASE  0xFD020000U
#define XMPU_DDR3_BASE  0xFD030000U
#define XMPU_DDR4_BASE  0xFD040000U
#define XMPU_DDR5_BASE  0xFD050000U

/* ---- 寄存器偏移 ---- */
#define XMPU_CTRL        0x000U /* bit0 DEFRD, bit1 DEFWR, bit2 POISONCFG, bit3 ALIGNCFG */
#define XMPU_POISON      0x00CU /* bit20 ATTRIB, [19:0] BASE */
#define XMPU_LOCK        0x020U /* bit0 REGWRDIS */
#define XMPU_R00_START   0x100U /* region n 步长 0x10：START/END/MASTER/CONFIG */

/* CTRL 位 */
#define XMPU_CTRL_DEFRD     (1U << 0)
#define XMPU_CTRL_DEFWR     (1U << 1)
#define XMPU_CTRL_POISONCFG (1U << 2)
#define XMPU_CTRL_ALIGNCFG  (1U << 3)

#define XMPU_POISON_ATTR    (1U << 20) /* poison by attribute（XAPP1320 推荐） */

/* region CONFIG 位 */
#define XMPU_RCFG_ENABLE     (1U << 0)
#define XMPU_RCFG_RD         (1U << 1)
#define XMPU_RCFG_WR         (1U << 2)
#define XMPU_RCFG_REGIONNS   (1U << 3)
#define XMPU_RCFG_NSSTRICT   (1U << 4)

/* region MASTER 组装：ID[9:0] | MASK[25:16] */
#define XMPU_MASTER(id, mask) (((u32)(mask) << 16) | (u32)(id))

/* master ID（PMUFW xpfw_xpu.c LUT） */
#define XMPU_MID_APU   0x080U  /* APU: 0x80-0xBF → MASK 0x3C0 */
#define XMPU_MASK_APU  0x3C0U
#define XMPU_MID_DAP   0x062U  /* JTAG 调试器，精确匹配 */
#define XMPU_MASK_EXACT 0x3FFU

/* TZDRAM：0x60000000-0x70000000（256MB），ALIGNCFG=1 时地址按 1MB 单位 */
#define TZDRAM_START_MB  (0x60000000U >> 20) /* 0x600 */
#define TZDRAM_END_MB    (0x70000000U >> 20) /* 0x700，开区间 */

static const u32 xmpu_bases[6] = {
	XMPU_DDR0_BASE, XMPU_DDR1_BASE, XMPU_DDR2_BASE,
	XMPU_DDR3_BASE, XMPU_DDR4_BASE, XMPU_DDR5_BASE,
};

static void xmpu_wr(u32 base, u32 offset, u32 val)
{
	XFsbl_Out32(base + offset, val);
}

static void xmpu_config_one(u32 base)
{
	/*
	 * R0：TZDRAM ← APU（secure 放行；NS=REE 触发安全违规被 poison）。
	 * relaxed NS 检查（NSCHECKTYPE=0）：secure 可访问任何 region，
	 * NS 只能访问 REGIONNS=1 的 region。
	 */
	xmpu_wr(base, XMPU_R00_START + 0x00, TZDRAM_START_MB);
	xmpu_wr(base, XMPU_R00_START + 0x04, TZDRAM_END_MB);
	xmpu_wr(base, XMPU_R00_START + 0x08,
	        XMPU_MASTER(XMPU_MID_APU, XMPU_MASK_APU));
	xmpu_wr(base, XMPU_R00_START + 0x0C,
	        XMPU_RCFG_ENABLE | XMPU_RCFG_RD | XMPU_RCFG_WR);

	/*
	 * R1：TZDRAM ← DAP（JTAG 调试器），ENABLE 但不给任何权限。
	 * 扫描顺序 15→0 先匹配者生效，R1 在 R0 之后检查、先命中 DAP。
	 * 过渡保护；eFUSE 关 JTAG（P3）后此条仍留作纵深。
	 */
	xmpu_wr(base, XMPU_R00_START + 0x10, TZDRAM_START_MB);
	xmpu_wr(base, XMPU_R00_START + 0x14, TZDRAM_END_MB);
	xmpu_wr(base, XMPU_R00_START + 0x18,
	        XMPU_MASTER(XMPU_MID_DAP, XMPU_MASK_EXACT));
	xmpu_wr(base, XMPU_R00_START + 0x1C, XMPU_RCFG_ENABLE);

	/* poison by attribute；默认放行其余地址（维持现有行为，缺口见文档 §2） */
	xmpu_wr(base, XMPU_POISON, XMPU_POISON_ATTR);
	xmpu_wr(base, XMPU_CTRL,
	        XMPU_CTRL_DEFRD | XMPU_CTRL_DEFWR |
	        XMPU_CTRL_POISONCFG | XMPU_CTRL_ALIGNCFG);
}

u32 pqchsm_xmpu_protect_tzdram(void)
{
	u32 i;

	for (i = 0; i < 6; i++) {
		xmpu_config_one(xmpu_bases[i]);
	}
	return XFSBL_SUCCESS;
}

u32 pqchsm_xmpu_lock(void)
{
	u32 i;

	for (i = 0; i < 6; i++) {
		xmpu_wr(xmpu_bases[i], XMPU_LOCK, 1U);
	}
	return XFSBL_SUCCESS;
}
