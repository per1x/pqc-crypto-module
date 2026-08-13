// SPDX-License-Identifier: GPL-2.0
/*
 * protdump —— 从安全世界（EL3）读出 XPPU / XMPU 的实配
 *
 * ============================================================================
 * 【为什么必须走 EL3，普通世界读不到】
 * ============================================================================
 * board/xmpu_probe.c 已经从普通世界试过：149 次读里 146 次是**总线错误**
 * （不是 mmap 失败 —— 这两种必须分清）。也就是说保护单元的配置寄存器
 * 自己就受保护，普通世界连"看一眼现在配成什么样"都做不到。
 *
 * 于是走打过补丁的 BL31：SiP 0x8200ff11 在 EL3 上替我们读，白名单只放行
 * 四个保护单元的寄存器窗口（见 boot/atf/patch_atf_protread.py）。
 *
 * ============================================================================
 * 【分批读，每批落盘 —— 因为 EL3 上的取数错误没人接得住】
 * ============================================================================
 * 在 EL3 读一个未实现的地址会产生同步外部中止，BL31 里没有处理它的东西：
 * 发 SMC 的那个核当场卡死，其余核照常跑 Linux。表现是"半死不活"，
 * 最后被外部看门狗重启 —— 而这一轮读到的东西全丢了。
 *
 * 所以不做"一次读完 600 个寄存器"，而是：
 *   insmod protdump.ko group=N     # 一次只读一组
 *   cat /proc/protdump             # 取这一组的结果
 *   rmmod protdump
 * 组按"越可能出事的排越后"排。哪一组卡住，就知道是哪一段地址的问题，
 * 前面几组的结果已经拿到手了。
 *
 * group=0  XPPU 本体（CTRL/ISR/IMR/LOCK/POISON/ERR_STATUS）—— 最安全
 * group=1  XPPU 的 400 条 aperture 许可表（0xFF98_1000 起）
 * group=2  XMPU_DDR0..5 的 CTRL/POISON/LOCK
 * group=3  XMPU_DDR0..5 的 16 个区域
 * group=4  XMPU_FPD（本体 + 16 个区域）
 * group=5  XMPU_OCM（本体 + 16 个区域）
 */
#include <linux/arm-smccc.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#define SIP_PROT_READ   0x8200ff11UL

/* 另一条读法：把请求转给 PMU 执行（ZynqMP 的 PM 通路，与 pmsec.c 同一套）。
 *
 * 为什么要有第二条路：XMPU_DDR0..5（0xFD00_0000）与 XMPU_FPD（0xFD5D_0000）
 * 不在 BL31 的页表里，要从 EL3 读就得**新增一段映射** —— 而加了那段映射的
 * BL31 在这块板上**起不来**（试过两次，两次断电；MAX_XLAT_TABLES 放宽到 10
 * 也一样，所以不是转换表不够）。为这两个次要目标继续冒险不值得。
 *
 * PMU 本来就是配 XMPU 的那个主控，它的地址白名单很可能放行这些寄存器，
 * 而且**这条路完全不碰启动**：golden 镜像上就能跑。
 *
 * ⚠️ 两条路的发起者不同，报告里必须写清楚是哪一条读到的：
 *   · SiP    → 发起者是 APU 的 EL3（安全世界）
 *   · PM     → 发起者是 PMU 这个独立的安全主控
 * 读到同一个值不代表两者等价，别把它们混成一句"安全世界读到了"。
 */
#define PM_SIP_SVC      0xC2000000UL
#define PM_MMIO_READ    20

#define XPPU_BASE       0xFF980000UL
#define XPPU_APER_BASE  0xFF981000UL
#define XPPU_APER_N     400

#define XMPU_DDR_BASE   0xFD000000UL   /* 6 个实例，步长 0x10000 */
#define XMPU_FPD_BASE   0xFD5D0000UL
#define XMPU_OCM_BASE   0xFFA70000UL

/* XMPU 寄存器偏移（QEMU 的 xlnx-zynqmp-xmpu 设备模型 + PMUFW xpfw_xpu.c） */
#define XMPU_CTRL       0x000
#define XMPU_ERR_STATUS1 0x004
#define XMPU_ERR_STATUS2 0x008
#define XMPU_POISON     0x00C
#define XMPU_ISR        0x010
#define XMPU_IMR        0x014
#define XMPU_LOCK       0x01C
#define XMPU_R0_START   0x100          /* 区域 n 步长 0x10 */

static int group = 0;
module_param(group, int, 0444);
MODULE_PARM_DESC(group, "读哪一组（0..5，见文件头）");

static int via_pm = 0;
module_param(via_pm, int, 0444);
MODULE_PARM_DESC(via_pm, "非 0 则走 PM_MMIO_READ（由 PMU 代读）而不是 EL3 的 SiP");

struct ent {
	char  name[40];
	u32   addr;
	int   ret;
	u32   val;
};

static struct ent *tab;
static int n_ent;

/* 读一个保护单元寄存器。返回 0 成功。 */
static int prot_read(u32 addr, u32 *out)
{
	struct arm_smccc_res res;

	if (via_pm) {
		/* PM 约定：x1 = (arg1<<32)|arg0；返回 a0 低 32 位是状态，
		 * 高 32 位是数据（与 drivers/firmware/xilinx/zynqmp.c 一致）。 */
		arm_smccc_smc(PM_SIP_SVC | PM_MMIO_READ, (u64)addr, 0,
			      0, 0, 0, 0, 0, &res);
		*out = (u32)(res.a0 >> 32);
		return (int)(s32)(u32)res.a0;
	}
	arm_smccc_smc(SIP_PROT_READ, (u64)addr, 0, 0, 0, 0, 0, 0, &res);
	*out = (u32)res.a1;
	return (int)(s32)(u32)res.a0;
}

static void add(const char *name, u32 addr)
{
	if (n_ent >= 2048)
		return;
	strncpy(tab[n_ent].name, name, sizeof(tab[n_ent].name) - 1);
	tab[n_ent].addr = addr;
	/* 每读一个就打一条 pr_info：万一下一个把核卡死，
	 * dmesg 里留着的最后一行就是"卡在哪个地址"。 */
	tab[n_ent].ret = prot_read(addr, &tab[n_ent].val);
	pr_info("protdump: %s @0x%08x ret=%d val=0x%08x\n",
		tab[n_ent].name, addr, tab[n_ent].ret, tab[n_ent].val);
	n_ent++;
}

static void add_xmpu_core(const char *tag, u32 base)
{
	char nm[40];

	snprintf(nm, sizeof nm, "%s CTRL", tag);        add(nm, base + XMPU_CTRL);
	snprintf(nm, sizeof nm, "%s ERR_ST1", tag);     add(nm, base + XMPU_ERR_STATUS1);
	snprintf(nm, sizeof nm, "%s ERR_ST2", tag);     add(nm, base + XMPU_ERR_STATUS2);
	snprintf(nm, sizeof nm, "%s POISON", tag);      add(nm, base + XMPU_POISON);
	snprintf(nm, sizeof nm, "%s ISR", tag);         add(nm, base + XMPU_ISR);
	snprintf(nm, sizeof nm, "%s LOCK", tag);        add(nm, base + XMPU_LOCK);
}

static void add_xmpu_regions(const char *tag, u32 base)
{
	char nm[40];
	int r;

	for (r = 0; r < 16; r++) {
		u32 o = base + XMPU_R0_START + r * 0x10;

		snprintf(nm, sizeof nm, "%s R%02d START", tag, r);  add(nm, o + 0x0);
		snprintf(nm, sizeof nm, "%s R%02d END", tag, r);    add(nm, o + 0x4);
		snprintf(nm, sizeof nm, "%s R%02d MASTER", tag, r); add(nm, o + 0x8);
		snprintf(nm, sizeof nm, "%s R%02d CONFIG", tag, r); add(nm, o + 0xC);
	}
}

static int protdump_show(struct seq_file *m, void *v)
{
	int i, refused = 0, okc = 0;

	seq_printf(m, "=== 保护单元实配（%s，group=%d）===\n",
		   via_pm ? "PM_MMIO_READ：由 **PMU** 代读" : "EL3 的 SiP：由 **APU 安全世界** 读",
		   group);
	for (i = 0; i < n_ent; i++) {
		if (tab[i].ret == 0) {
			seq_printf(m, "  %-32s @0x%08x = 0x%08x\n",
				   tab[i].name, tab[i].addr, tab[i].val);
			okc++;
		} else {
			seq_printf(m, "  %-32s @0x%08x  被拒（%s，码 %d）\n",
				   tab[i].name, tab[i].addr,
				   via_pm ? "PMUFW 白名单" : "SiP 白名单",
				   tab[i].ret);
			refused++;
		}
	}
	seq_printf(m, "--- 本组 %d 条，读到 %d，被 SiP 拒 %d ---\n",
		   n_ent, okc, refused);
	seq_puts(m,
		 "注意：这里的\"读到\"是 EL3 读到的。普通世界读同一批地址是总线错误\n"
		 "      （board/xmpu_probe.c 实测 146/149）—— 那正是要走这条路的原因。\n");
	return 0;
}

static int protdump_open(struct inode *ino, struct file *f)
{
	return single_open(f, protdump_show, NULL);
}

/* 5.4 还没有 struct proc_ops（5.6 才引入） */
static const struct file_operations protdump_pops = {
	.owner   = THIS_MODULE,
	.open    = protdump_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int __init protdump_init(void)
{
	char nm[40];
	int i;

	tab = kcalloc(2048, sizeof(*tab), GFP_KERNEL);
	if (!tab)
		return -ENOMEM;

	pr_info("protdump: group=%d 开始\n", group);

	switch (group) {
	case 0:
		add("XPPU CTRL",        XPPU_BASE + 0x000);
		add("XPPU ERR_STATUS1", XPPU_BASE + 0x004);
		add("XPPU ERR_STATUS2", XPPU_BASE + 0x008);
		add("XPPU POISON",      XPPU_BASE + 0x00C);
		add("XPPU ISR",         XPPU_BASE + 0x010);
		add("XPPU IMR",         XPPU_BASE + 0x014);
		add("XPPU IER",         XPPU_BASE + 0x018);
		add("XPPU LOCK",        XPPU_BASE + 0x01C);
		/* 白名单反证：窗口外的地址必须被 SiP 拒，而不是读出东西来。
		 * 没有这一条的话，"全都读到了"也可能只是因为白名单根本没生效。 */
		add("反证 DDR(0x00100000)", 0x00100000);
		add("反证 BL31(0x00001000)", 0x00001000);
		break;
	case 1:
		for (i = 0; i < XPPU_APER_N; i++) {
			snprintf(nm, sizeof nm, "XPPU APERPERM_%03d", i);
			add(nm, XPPU_APER_BASE + i * 4);
		}
		break;
	case 2:
		for (i = 0; i < 6; i++) {
			snprintf(nm, sizeof nm, "XMPU_DDR%d", i);
			add_xmpu_core(nm, XMPU_DDR_BASE + i * 0x10000);
		}
		break;
	case 3:
		for (i = 0; i < 6; i++) {
			snprintf(nm, sizeof nm, "DDR%d", i);
			add_xmpu_regions(nm, XMPU_DDR_BASE + i * 0x10000);
		}
		break;
	case 4:
		add_xmpu_core("XMPU_FPD", XMPU_FPD_BASE);
		add_xmpu_regions("FPD", XMPU_FPD_BASE);
		break;
	case 5:
		add_xmpu_core("XMPU_OCM", XMPU_OCM_BASE);
		add_xmpu_regions("OCM", XMPU_OCM_BASE);
		break;
	default:
		pr_info("protdump: 不认识的 group\n");
		break;
	}

	pr_info("protdump: group=%d 读完 %d 条\n", group, n_ent);
	proc_create("protdump", 0444, NULL, &protdump_pops);
	return 0;
}

static void __exit protdump_exit(void)
{
	remove_proc_entry("protdump", NULL);
	kfree(tab);
}

module_init(protdump_init);
module_exit(protdump_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("EL3 读 XPPU/XMPU 实配（分组，配合打过补丁的 BL31）");
