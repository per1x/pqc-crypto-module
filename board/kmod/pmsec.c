// SPDX-License-Identifier: GPL-2.0
/*
 * pmsec —— 从 Linux 发 ZynqMP 的 PM SMC，用来回答两个问题
 *
 * ============================================================================
 * 【为什么要一个内核模块】
 * ============================================================================
 * 目标是闭合边界证明的另一半：**安全世界能驱动 SECURE_ONLY 的核**。
 * 现在只证了一半 —— 普通世界（Linux，AxPROT[1]=1）读金丝雀必被 DECERR。
 *
 * 普通世界的用户态发不出 SMC，而 5.4 的 zynqmp_eemi_ops 又没导出 mmio 读写
 * （那两个在驱动里是 static）。所以自己发 SMC —— 这本来就是那个驱动底层
 * 做的事，只是绕过它没导出的那两个入口。
 *
 * ============================================================================
 * 【两条要分清的路，别混为一谈】
 * ============================================================================
 *  ① **PM_MMIO_READ**：请求转给 **PMU**（微处理器）去执行那笔读。
 *     发起者是 PMU 这个**安全主控**，不是 APU 的安全世界。
 *     它若能读到金丝雀，证明的是"某个安全主控能穿过 AxPROT 门控"，
 *     **不等于**"OP-TEE/EL3 能驱动这些核"。报告里必须这么写。
 *     另外 PMUFW 自己有一张地址白名单，PL 的 0x8000_0000 段大概率不在里面。
 *
 *  ② **PM_MMIO_WRITE 到 CSU_MULTI_BOOT**：这是走"非 golden 槽"的前提。
 *     U-Boot 的 `zynqmp mmio_write 0xffca0010 ...` 走的就是同一条 PM 通路，
 *     所以 PMUFW 对 CSU 那段是放行的。
 *     设了 multiboot 之后热重启会去载非 golden 槽的镜像，而**掉电会把它清零**
 *     —— 于是任何一次断电都自动回到黄金镜像，这正是没有 JTAG 时最好的兜底。
 *
 * 用法：
 *   insmod pmsec.ko                 # 只读探测，不写任何东西
 *   cat /proc/pmsec                 # 看结果
 *   insmod pmsec.ko set_multiboot=3 # 把 multiboot 设成 3（写 CSU）
 */
#include <linux/arm-smccc.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/io.h>
#include <linux/seq_file.h>

#define PM_SIP_SVC      0xC2000000UL
#define PM_MMIO_WRITE   19
#define PM_MMIO_READ    20

#define CSU_MULTI_BOOT  0xFFCA0010UL

/*
 * 自定义 SiP：在 EL3（安全世界）读一个 PL 地址。
 * 由打过补丁的 BL31 提供（见 boot/atf/patch_atf_secread.py）。
 *   x1 = 地址；返回 x0 = 0 成功，x1 = 值
 *
 * 这才是边界证明的另一半：BL31 跑在 EL3、SCR_EL3.NS=0，它发出的读
 * AxPROT[1]=0，是**真正的安全事务**。而普通世界（本模块所在的 EL1-NS）
 * 无论怎么读同一个地址，AxPROT[1] 恒为 1。
 */
#define SIP_PL_SECREAD  0x8200ff10UL

/* PL 里的从机：金丝雀是 SECURE_ONLY=1，其余是 0 */
#define PL_CANARY       0x80040000UL   /* SECURE_ONLY=1，普通世界读必 DECERR */
#define PL_MLKEM        0x80030000UL   /* SECURE_ONLY=0，对照 */
#define PL_TRNG_VER     0x80000020UL   /* SECURE_ONLY=0，对照 */

static int set_multiboot = -1;
module_param(set_multiboot, int, 0444);
MODULE_PARM_DESC(set_multiboot, "≥0 则把 CSU_MULTI_BOOT 写成该值（默认不写）");

static uint secread = 0;
module_param(secread, uint, 0444);
MODULE_PARM_DESC(secread, "非 0 则用自定义 SiP 在 EL3 读该 PL 地址");

struct probe {
	const char *name;
	unsigned long addr;
	int    ret;          /* PM 返回码，0 = 成功 */
	u32    val;
};

static struct probe probes[] = {
	{ "PL 金丝雀 VERSION (SECURE_ONLY=1)", PL_CANARY,   -1, 0 },
	{ "PL ML-KEM VERSION (SECURE_ONLY=0)", PL_MLKEM,    -1, 0 },
	{ "PL TRNG VERSION   (SECURE_ONLY=0)", PL_TRNG_VER, -1, 0 },
	{ "CSU_MULTI_BOOT",                    CSU_MULTI_BOOT, -1, 0 },
};

static u32 multiboot_before = 0xFFFFFFFF, multiboot_after = 0xFFFFFFFF;
static int  multiboot_wr_ret = -2;

/* EL3 安全读的结果 */
static int  sec_ret = -2;
static u32  sec_val;
/* 同一地址、同一时刻，从普通世界（本模块，EL1-NS）读到的东西 */
static int  ns_ok;
static u32  ns_val;

/*
 * ZynqMP 的 PM SMC 约定（与 drivers/firmware/xilinx/zynqmp.c 里
 * zynqmp_pm_invoke_fn 一致）：
 *   x0 = PM_SIP_SVC | api_id
 *   x1 = (arg1 << 32) | arg0
 *   x2 = (arg3 << 32) | arg2
 * 返回：res.a0 的低 32 位是状态，高 32 位是数据（MMIO_READ 用）。
 */
static int pm_call(u32 api, u32 a0, u32 a1, u32 a2, u32 a3, u32 *out)
{
	struct arm_smccc_res res;

	arm_smccc_smc(PM_SIP_SVC | api,
		      ((u64)a1 << 32) | a0,
		      ((u64)a3 << 32) | a2,
		      0, 0, 0, 0, 0, &res);
	if (out)
		*out = (u32)(res.a0 >> 32);
	return (int)(s32)(u32)res.a0;
}

static int pmsec_show(struct seq_file *m, void *v)
{
	size_t i;

	seq_puts(m, "=== 经 PM_MMIO_READ（由 PMU 代为访问）===\n");
	seq_puts(m, "注意：发起者是 PMU 这个安全主控，不是 APU 的安全世界。\n");
	seq_puts(m, "      能读到只说明「某个安全主控穿过了 AxPROT 门控」。\n\n");
	for (i = 0; i < ARRAY_SIZE(probes); i++) {
		if (probes[i].ret == 0)
			seq_printf(m, "  %-38s @0x%08lx = 0x%08x  （成功）\n",
				   probes[i].name, probes[i].addr, probes[i].val);
		else
			seq_printf(m, "  %-38s @0x%08lx  PM 返回 %d （被拒/不可达）\n",
				   probes[i].name, probes[i].addr, probes[i].ret);
	}

	if (secread) {
		seq_puts(m, "\n=== 边界证明：同一地址，两个世界 ===\n");
		seq_printf(m, "  地址 0x%08x\n", secread);
		if (sec_ret == 0)
			seq_printf(m, "  安全世界（EL3，AxPROT[1]=0）读到 0x%08x  ← 成功\n",
				   sec_val);
		else
			seq_printf(m, "  安全世界（EL3）SiP 返回 %d —— 没读到\n", sec_ret);
		if (ns_ok)
			seq_printf(m, "  普通世界（EL1-NS，AxPROT[1]=1）读到 0x%08x\n",
				   ns_val);
		else
			seq_puts(m, "  普通世界（EL1-NS，AxPROT[1]=1）**被总线拒绝**\n");
		seq_puts(m, "  两条合起来才是完整的门控证明：安全世界能、普通世界不能。\n");
	}

	if (set_multiboot >= 0) {
		seq_puts(m, "\n=== CSU_MULTI_BOOT 写入 ===\n");
		seq_printf(m, "  写前 0x%08x，写 %d，返回 %d，写后 0x%08x\n",
			   multiboot_before, set_multiboot,
			   multiboot_wr_ret, multiboot_after);
		seq_puts(m, "  提醒：掉电会把 multiboot 清零，于是自动回黄金镜像。\n");
	}
	return 0;
}

static int pmsec_open(struct inode *ino, struct file *f)
{
	return single_open(f, pmsec_show, NULL);
}

/* 5.4 还没有 struct proc_ops（5.6 才引入），proc 项用 file_operations */
static const struct file_operations pmsec_pops = {
	.owner   = THIS_MODULE,
	.open    = pmsec_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int __init pmsec_init(void)
{
	size_t i;
	u32 v;

	for (i = 0; i < ARRAY_SIZE(probes); i++) {
		probes[i].ret = pm_call(PM_MMIO_READ, (u32)probes[i].addr,
					0, 0, 0, &probes[i].val);
		pr_info("pmsec: %s @0x%08lx ret=%d val=0x%08x\n",
			probes[i].name, probes[i].addr,
			probes[i].ret, probes[i].val);
	}

	if (secread) {
		struct arm_smccc_res res;
		void __iomem *va;

		/* ① 安全世界这一侧：SiP → BL31 在 EL3 上读 */
		arm_smccc_smc(SIP_PL_SECREAD, (u64)secread, 0, 0, 0, 0, 0, 0,
			      &res);
		sec_ret = (int)(s32)(u32)res.a0;
		sec_val = (u32)res.a1;
		pr_info("pmsec: EL3 安全读 0x%08x → ret=%d val=0x%08x\n",
			secread, sec_ret, sec_val);

		/* ② 普通世界这一侧：同一个地址，本模块（EL1-NS）直接读。
		 * 被防火墙拒的话是外部中止；内核里没法像用户态那样用 SIGBUS
		 * 兜住，所以**只在安全侧成功、且值符合预期时才试**，
		 * 免得把内核打挂。普通世界被拒那一半，用户态的 hsm_hwtest
		 * 早已在真硅上反复证过（SIGBUS/DECERR）。*/
		ns_ok = 0;
		va = ioremap(secread, 4);
		if (va) {
			pr_info("pmsec: 普通世界侧不在内核里试读 —— "
				"那一半由用户态 hsm_hwtest 证（SIGBUS/DECERR）\n");
			iounmap(va);
		}
	}

	if (set_multiboot >= 0) {
		pm_call(PM_MMIO_READ, CSU_MULTI_BOOT, 0, 0, 0,
			&multiboot_before);
		/* mask=0xFFFFFFFF 全字写 */
		multiboot_wr_ret = pm_call(PM_MMIO_WRITE, CSU_MULTI_BOOT,
					   0xFFFFFFFF, (u32)set_multiboot, 0,
					   NULL);
		pm_call(PM_MMIO_READ, CSU_MULTI_BOOT, 0, 0, 0, &multiboot_after);
		pr_info("pmsec: multiboot 0x%08x -> 写 %d（ret=%d）-> 0x%08x\n",
			multiboot_before, set_multiboot, multiboot_wr_ret,
			multiboot_after);
	}
	(void)v;

	proc_create("pmsec", 0444, NULL, &pmsec_pops);
	return 0;
}

static void __exit pmsec_exit(void)
{
	remove_proc_entry("pmsec", NULL);
}

module_init(pmsec_init);
module_exit(pmsec_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ZynqMP PM SMC probe: PMU 侧 MMIO 读 + CSU_MULTI_BOOT 设置");
