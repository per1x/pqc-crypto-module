/* protunit_probe —— 经 EL3 只读窗口读 XMPU / XPPU 的**实配**
 *
 * ============================================================================
 * 【为什么不能沿用 xmpu_probe】
 * ============================================================================
 * board/src/xmpu_probe 走 /dev/mem 从普通世界直读，**这块板上一个寄存器都读不到**
 * ——	XPPU 连"保护单元自己的配置寄存器"也保护着，每一次读都是总线错误
 * （那个工具的输出全是"总线错误（硬件挡的）"）。
 *
 * 它记录的是"读不到"这个事实，本身有价值；但"配成什么样"仍然无法回答。
 * 于是本项目在 BL31 的白名单里开了一个**只读**窗口（0xFF98_0000 段、
 * 0xFD00_0000 段等），本工具经它去读。
 *
 * ⚠️ 写口一律没有。能写 XMPU/XPPU 等于能关掉全部内存保护。
 *
 * ⚠️ 这个只读口是**攻击面的净增加**（普通世界从此能读出保护配置）。
 *    判断是值得：配置不是秘密（谁拿到同款板都能在自己板上读出来），
 *    而看不见它就只能一直猜 —— 本项目在"照文档抄一份会漂移"上栽过好几次。
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../kmod/secmmio_uapi.h"

static int fd = -1;

/* 读一个字。被 EL3 拒绝返回 -1（**不是**总线错误 —— 白名单在 mmio 之前就挡了）。 */
static int rd(uint32_t a, uint32_t *v)
{
	struct secmmio_op op = { .addr = a, .val = 0 };

	if (ioctl(fd, SECMMIO_RD, &op) < 0)
		return -1;
	*v = op.val;
	return 0;
}

static void show(const char *nm, uint32_t a)
{
	uint32_t v;

	if (rd(a, &v))
		printf("  %-22s @0x%08x = **被 EL3 拒绝（白名单里没有）**\n", nm, a);
	else
		printf("  %-22s @0x%08x = 0x%08x\n", nm, a, v);
}

int main(void)
{
	int i, n;

	fd = open("/dev/secmmio", O_RDWR);
	if (fd < 0) {
		perror("open /dev/secmmio");
		return 1;
	}
	if (ioctl(fd, SECMMIO_ARM) < 0) {
		perror("SECMMIO_ARM");
		return 1;
	}

	printf("=== ZynqMP 保护单元实配（经 EL3 只读窗口）===\n\n");

	printf("[XPPU —— LPD 从机的许可表  base 0xFF980000]\n");
	show("XPPU_CTRL",        0xFF980000);
	show("XPPU_ERR_STATUS1", 0xFF980004);
	show("XPPU_ERR_STATUS2", 0xFF980008);
	show("XPPU_POISON",      0xFF98000C);
	show("XPPU_ISR",         0xFF980010);
	show("XPPU_IMR",         0xFF980014);
	show("XPPU_LOCK",        0xFF98001C);

	/* XMPU_DDR0..5：每个实例 16 个区域，区域寄存器从 +0x100 起，每区 0x10。
	 * 只把**使能了的**区域打出来 —— 16×6 = 96 个区域全打会淹掉真正的信息。 */
	printf("\n[XMPU_DDR0..5 —— 只列使能的区域]\n");
	n = 0;
	for (i = 0; i < 6; i++) {
		uint32_t base = 0xFD000000u + (uint32_t)i * 0x10000u;
		uint32_t ctrl, poison;
		int r;

		if (rd(base + 0x00, &ctrl)) {
			printf("  XMPU_DDR%d：CTRL 被 EL3 拒绝\n", i);
			continue;
		}
		(void)rd(base + 0x04, &poison);
		printf("  XMPU_DDR%d CTRL=0x%08x POISON=0x%08x\n", i, ctrl, poison);
		for (r = 0; r < 16; r++) {
			uint32_t ro = base + 0x100u + (uint32_t)r * 0x10u;
			uint32_t st, en, ms, cf;

			if (rd(ro + 0x0, &st) || rd(ro + 0x4, &en) ||
			    rd(ro + 0x8, &ms) || rd(ro + 0xC, &cf))
				break;
			if ((cf & 1u) == 0u)     /* bit0 = Enable */
				continue;
			n++;
			/* 地址寄存器存的是物理地址 >> 12。
			 * cf: bit0 EN, bit1 RdAllowed, bit2 WrAllowed, bit3 NSCheckType,
			 *     bit4 RegionNS */
			printf("    R%02d  0x%09llx..0x%09llx  master=0x%08x  cfg=0x%08x"
			       "  [%s%s%s]\n",
			       r,
			       (unsigned long long)st << 12,
			       (((unsigned long long)en << 12) | 0xFFFULL),
			       ms, cf,
			       (cf & 2u) ? "R" : "-",
			       (cf & 4u) ? "W" : "-",
			       (cf & 16u) ? " NS" : " S");
		}
	}
	if (n == 0)
		printf("  —— 六个实例里**没有一个使能的区域**\n");

	printf("\n[XMPU_FPD  base 0xFD5D0000]\n");
	show("XMPU_FPD_CTRL", 0xFD5D0000);
	printf("\n[XMPU_OCM  base 0xFFA70000]\n");
	show("XMPU_OCM_CTRL", 0xFFA70000);

	close(fd);
	return 0;
}
