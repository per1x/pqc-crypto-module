// SPDX-License-Identifier: GPL-2.0
/*
 * xmpupoison —— 开机时把 XMPU_DDR 的「按属性毒化」打开，让 OP-TEE 的安全内存
 *               真的挡住普通世界的读。
 *
 * ============================================================================
 * 【为什么需要它 —— XMPU 只检测，不拦截】
 * ============================================================================
 * XAPP1320《Isolation Methods in Zynq UltraScale+ MPSoCs》v3.0 第 10 页：
 *
 *   "If an illegal transaction is attempted, the XMPU asserts AxUser[10] but the
 *    transaction **is passed to the memory controller** … **The transaction is
 *    gated by the end point, not the XMPU itself.**"
 *
 * BL31 已经把 OP-TEE 的 core 段（0x6000_0000+0x1000_0000）在六个 XMPU_DDR 实例上
 * 各配了一条安全区域，NS 检查也确实在工作 —— 板上能读到 ERR_MASTER 落在 0x80-0xBF
 * （= APU，主控 ID 表见 PMUFW xpfw_xpu.c:71）、ISR bit3 = SECURTYVIO。
 * **但 POISON 寄存器是 0，没有任何东西去 gate，所以数据照样返回。**
 *
 * 把 POISON.ATTRIB（bit20）置上之后，板上实测：
 *     毒化前  devmem 0x60000000 → 0xAA0003F3   （OP-TEE 的一条 AArch64 指令）
 *     毒化后  devmem 0x60000000 → Bus error（SIGBUS）
 *     区域外  devmem 0x10000000 → 照常
 *     0x70000000（OP-TEE 共享内存，故意不圈）→ 照常
 *     sdf_demo 九节全绿，daemon / 网络 / PL 均不受影响
 *
 * ============================================================================
 * 【⚠️ 2026-08-17 收尾：这个模块已经用不上了，而且它那条路是不通的】
 * ============================================================================
 * **默认镜像里 BL31 开机就把毒化打开了**，不需要这个模块。留着它只为记录
 * 一条走不通的路，以及万一落到不带那段 BL31 的镜像时有个（无效的）备选。
 *
 * 两条要更正的话：
 *
 * ① **"BL31 那条路不可靠、时灵时不灵"——错了。** BL31 那条路一直是通的；
 *    "时灵时不灵"的真因是 FSBL 武装了一条 100 秒的启动看门狗、而
 *    LOG_LEVEL=50 的页表转储把启动拖过了那条线，于是 WDT 复位 + multiboot++
 *    落到了别的槽。降到 LOG_LEVEL=20 之后三份镜像零失败。
 *    见 board/logs/RESULT_slot_boot_wdt.txt。
 *
 * ② **PMU 这条路本身是死的。** PM_MMIO_WRITE（EEMI API #19）写 XMPU_DDR
 *    被 PMU 拒绝，返回 2002 = XST_PM_NO_ACCESS，六个实例全拒 ——
 *    PMU 固件对 MMIO 写有自己的地址白名单，XMPU_DDR 不在里面。
 *    本模块会如实报错并拒绝加载，不会假装成功。
 *
 * 正确的做法就是 BL31 在 EL3 里写：boot/atf/patch_atf_secmmio.py。
 *
 * ============================================================================
 * 【口径：这不是"完整的内存隔离"】
 * ============================================================================
 * 它挡住的是**普通世界读 OP-TEE 的 core 段**这一条。它**不**改变：
 *   · root 仍然能命令硬件做运算（SiP 不校验调用来源）；
 *   · 共享内存段（0x7000_0000）本来就该双向可读，没有圈进去；
 *   · 没有烧 eFUSE，也就没有防替换的信任根。
 *
 * 用法：
 *     insmod xmpupoison.ko            # 打开毒化
 *     insmod xmpupoison.ko off=1      # 关掉（回到只检测不拦截）
 *     cat /proc/xmpupoison            # 看六个实例的实配与每一笔写的返回码
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/arm-smccc.h>

#define PM_SIP_SVC      0xC2000000UL
#define PM_MMIO_WRITE   19
#define PM_MMIO_READ    20

/* 六个 XMPU_DDR 实例，步长 0x10000。POISON 在实例 +0x0C。
 * 偏移出处：xddr_xmpu0_cfg.h（Vitis 2020.1 standalone BSP）
 *   +0x00 CTRL  +0x04 ERR_STS1  +0x08 ERR_STS2  +0x0C POISON
 *   +0x10 ISR   +0x20 LOCK
 * POISON 字段：[31:20] ATTRIB，[19:0] BASE。 */
#define XMPU_DDR_BASE   0xFD000000UL
#define XMPU_DDR_STRIDE 0x10000UL
#define XMPU_DDR_N      6
#define XMPU_POISON_OFF 0x0CUL

/* 只置 ATTRIB（按属性毒化，XAPP1320 明确推荐的那种），BASE 保持 0。
 * ⚠️ 绝不动 CTRL：CTRL 现在是 0x0b，其中 DEFRDALWD/DEFWRALWD 管的是"不匹配任何
 *    区域的访问"，清掉等于把整个 DDR 都拒了，板子起不来。
 *    而 CTRL.POISONCFG 走的是**按地址**重定向（打到 POISON.BASE），BASE=0 时
 *    一次违规的**写**会打进物理地址 0 —— 在把 BASE 指到安全去处之前绝不能置它。 */
#define POISON_ATTRIB   0x00100000U

static int off;
module_param(off, int, 0444);
MODULE_PARM_DESC(off, "非 0 则关闭毒化（把 POISON 写回 0）");

static struct {
	unsigned long addr;
	int  wr_ret;
	u32  before, after;
} slot[XMPU_DDR_N];

static int nr_ok;

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

static int xp_show(struct seq_file *m, void *v)
{
	unsigned int i;

	seq_printf(m, "XMPU_DDR 按属性毒化：目标值 0x%08x，off=%d\n\n",
		   off ? 0U : POISON_ATTRIB, off);
	seq_puts(m, "  实例  POISON 地址   写前        写后        PM 返回\n");
	for (i = 0; i < XMPU_DDR_N; i++)
		seq_printf(m, "  DDR%u  0x%08lx    0x%08x  0x%08x  %d%s\n",
			   i, slot[i].addr, slot[i].before, slot[i].after,
			   slot[i].wr_ret,
			   slot[i].wr_ret ? "  ← PMU 拒绝" : "");

	seq_printf(m, "\n生效实例数：%d / %d\n", nr_ok, XMPU_DDR_N);
	if (nr_ok != XMPU_DDR_N) {
		seq_puts(m, "\n⚠️ 没有全部生效 —— **不要当成已加固**。\n"
			    "   DDR 地址在六个实例之间交织，少一个就等于漏一块。\n");
	} else if (!off) {
		seq_puts(m, "\n判据（别只看这张表）：到板上跑\n"
			    "    devmem 0x60000000 32   → 应当是 Bus error\n"
			    "    devmem 0x10000000 32   → 应当照常返回\n");
	}
	return 0;
}

static int xp_open(struct inode *ino, struct file *f)
{
	return single_open(f, xp_show, NULL);
}

/* 5.4 还没有 struct proc_ops（5.6 才引入） */
static const struct file_operations xp_pops = {
	.owner   = THIS_MODULE,
	.open    = xp_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int __init xp_init(void)
{
	u32 want = off ? 0U : POISON_ATTRIB;
	unsigned int i;

	for (i = 0; i < XMPU_DDR_N; i++) {
		unsigned long a = XMPU_DDR_BASE + i * XMPU_DDR_STRIDE +
				  XMPU_POISON_OFF;

		slot[i].addr = a;
		pm_call(PM_MMIO_READ, (u32)a, 0, 0, 0, &slot[i].before);
		slot[i].wr_ret = pm_call(PM_MMIO_WRITE, (u32)a,
					 0xFFFFFFFFU, want, 0, NULL);
		pm_call(PM_MMIO_READ, (u32)a, 0, 0, 0, &slot[i].after);

		if (slot[i].wr_ret == 0 && slot[i].after == want)
			nr_ok++;
	}

	pr_info("xmpupoison: %d/%d 个实例的 POISON 已写成 0x%08x\n",
		nr_ok, XMPU_DDR_N, want);

	if (nr_ok != XMPU_DDR_N)
		pr_warn("xmpupoison: **没有全部生效，别当成已加固**（看 /proc/xmpupoison）\n");

	proc_create("xmpupoison", 0444, NULL, &xp_pops);
	return 0;
}

static void __exit xp_exit(void)
{
	remove_proc_entry("xmpupoison", NULL);
}

module_init(xp_init);
module_exit(xp_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("经 PMU 的 PM_MMIO_WRITE 打开 XMPU_DDR 的按属性毒化");
