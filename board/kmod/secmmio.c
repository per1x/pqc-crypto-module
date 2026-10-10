// SPDX-License-Identifier: GPL-2.0
/*
 * secmmio —— 把用户态的核访问转成 SMC，由**安全世界（EL3）**替它发出
 *
 * ============================================================================
 * 【它存在的理由】
 * ============================================================================
 * 这一版 bitstream 把四个功能核全设成 SECURE_ONLY=1，普通世界（Linux，
 * AxPROT[1] 恒为 1）一个寄存器都摸不到。要证明"这些核确实能用、而且只有安全
 * 世界能用"，就得让整套 KAT 的每一笔核访问都从 EL3 发出。
 *
 * 用户态发不了 SMC；而把 KAT 逻辑与向量表整个搬进内核太重。所以这个模块只做
 * 一件事：**当信使**。KAT 与向量留在用户态，一次 ioctl 换一笔 32 位访问。
 *
 * ============================================================================
 * 【这个模块不是信任边界 —— EL3 才是】
 * ============================================================================
 * 白名单（哪些地址、哪些偏移、读还是写）**全部在 BL31 里判**，不在这里。
 * 用户态就算递一个越界地址过来，也只会拿到 EL3 的拒绝。
 *
 * 这个分工是有意的：内核模块可以被 root 换掉，而 BL31 在 EL3、
 * 普通世界改不了它。把策略放在这里等于把锁挂在门外面。
 *
 * ============================================================================
 * 【保险：先确认 PL 在，再允许发第一笔 SMC】
 * ============================================================================
 * 若 PL 没配置好就发 SMC，那笔访问会在 EL3 上出错，而 BL31 里没有处理它的
 * 东西 —— 发 SMC 的核当场卡死，其余核照常跑 Linux（"半死不活"，之前踩过）。
 *
 * 所以默认上保险：没 ARM 过一律返回 -EPERM。解除保险的判据放在**用户态**、
 * 而且**不碰 PL 总线**：读 /sys/class/fpga_manager/fpga0/state，确认是
 * "operating" 再 ioctl(SECMMIO_ARM)。
 *
 * 为什么不在内核里读那个 sysfs：模块里做文件 I/O 又丑又容易出错，而这个判据
 * 本来就是"调用方的责任"。放在用户态还有一个好处 —— 它是**可见**的一步，
 * 写在测试程序里能被审，藏在模块 init 里就没人看得见了。
 */
#include <linux/arm-smccc.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#include "secmmio_uapi.h"

/* 与 boot/atf/patch_atf_secmmio.py 里的定义一一对应 */
#define SIP_PL_RD   0x8200ff12UL
#define SIP_PL_WR   0x8200ff13UL
/* 种子装载：EL3 自己取熵、自己写进 PL，这边只发命令、只收返回码。
 * 普通世界（包括本模块）从头到尾接触不到种子明文。 */
#define SIP_PQC_SEED 0x8200ff14UL

static int armed;
static unsigned long n_rd, n_wr, n_seed, n_refused;
static u32 last_refused_addr;

static int sec_rd(u32 addr, u32 *out)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SIP_PL_RD, (u64)addr, 0, 0, 0, 0, 0, 0, &res);
	*out = (u32)res.a1;
	return (int)(s32)(u32)res.a0;
}

static int sec_wr(u32 addr, u32 val)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SIP_PL_WR, (u64)addr, (u64)val, 0, 0, 0, 0, 0, &res);
	return (int)(s32)(u32)res.a0;
}

static int sec_seed(u32 target, u32 *world)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SIP_PQC_SEED, (u64)target, 0, 0, 0, 0, 0, 0, &res);
	*world = (u32)res.a1;
	return (int)(s32)(u32)res.a0;
}

static long secmmio_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct secmmio_op op;
	int ret;

	if (cmd == SECMMIO_ARM) {
		armed = 1;
		pr_info("secmmio: 保险已解除（调用方声明 PL 已 programmed）\n");
		return 0;
	}

	if (!armed)
		return -EPERM;

	if (copy_from_user(&op, (void __user *)arg, sizeof(op)))
		return -EFAULT;

	switch (cmd) {
	case SECMMIO_RD:
		ret = sec_rd(op.addr, &op.val);
		if (ret) {
			n_refused++;
			last_refused_addr = op.addr;
			return -EACCES;
		}
		n_rd++;
		if (copy_to_user((void __user *)arg, &op, sizeof(op)))
			return -EFAULT;
		return 0;

	case SECMMIO_WR:
		ret = sec_wr(op.addr, op.val);
		if (ret) {
			n_refused++;
			last_refused_addr = op.addr;
			return -EACCES;
		}
		n_wr++;
		return 0;

	case SECMMIO_SEED: {
		struct secmmio_seed sd;

		if (copy_from_user(&sd, (void __user *)arg, sizeof(sd)))
			return -EFAULT;
		ret = sec_seed(sd.target, &sd.world);
		if (ret) {
			n_refused++;
			return -EIO;
		}
		n_seed++;
		if (copy_to_user((void __user *)arg, &sd, sizeof(sd)))
			return -EFAULT;
		return 0;
	}

	default:
		return -ENOTTY;
	}
}

static const struct file_operations secmmio_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = secmmio_ioctl,
	.compat_ioctl   = secmmio_ioctl,
	.llseek         = noop_llseek,
};

static struct miscdevice secmmio_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "secmmio",
	.fops  = &secmmio_fops,
	.mode  = 0600,
};

static int secmmio_show(struct seq_file *m, void *v)
{
	seq_printf(m, "armed        %d\n", armed);
	seq_printf(m, "secure_reads  %lu\n", n_rd);
	seq_printf(m, "secure_writes %lu\n", n_wr);
	seq_printf(m, "refused       %lu", n_refused);
	if (n_refused)
		seq_printf(m, "  (最后一个被 EL3 白名单拒的地址 0x%08x)",
			   last_refused_addr);
	seq_puts(m, "\n");
	seq_puts(m,
		 "\n说明：refused 是 **EL3 白名单**拒的，不是 PL 防火墙拒的 ——\n"
		 "      白名单在 BL31 里，这个模块只是信使（见文件头）。\n");
	return 0;
}

static int secmmio_open(struct inode *i, struct file *f)
{
	return single_open(f, secmmio_show, NULL);
}

/* 5.4 还没有 struct proc_ops */
static const struct file_operations secmmio_pops = {
	.owner   = THIS_MODULE,
	.open    = secmmio_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int __init secmmio_init(void)
{
	int ret = misc_register(&secmmio_dev);

	if (ret)
		return ret;
	proc_create("secmmio", 0444, NULL, &secmmio_pops);
	pr_info("secmmio: /dev/secmmio 就绪（默认上保险）\n");
	return 0;
}

static void __exit secmmio_exit(void)
{
	remove_proc_entry("secmmio", NULL);
	misc_deregister(&secmmio_dev);
}

module_init(secmmio_init);
module_exit(secmmio_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("把 PL 核访问经 SMC 交给 EL3 发出（白名单在 BL31 里）");
