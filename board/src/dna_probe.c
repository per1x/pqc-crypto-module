/* dna_probe —— 验设备 DNA 只读窗口（0xFFCA0050-5C）
 *
 *   dna_probe            读四个字并打印
 *   dna_probe -w         **再额外**试一次写，验证 EL3 拒绝
 *
 * ============================================================================
 * 【为什么试写是安全的，而平时试写不是】
 * ============================================================================
 * 本项目的老结论是"写的总线拒绝是 SError、接不住、代价是断电"。那说的是
 * **写真的发到了总线上**、由从机回 SLVERR 的情形。
 *
 * 这里不一样：DNA 窗口的写请求在 **EL3 的白名单里就被挡下来了**
 * （pl_permit 里 `return is_write ? 0 : 1`），根本没有 mmio_write_32 发生。
 * SMC 直接返回 ~0，驱动把它变成一个普通的 ioctl 错误。总线上什么都没发生。
 *
 * 所以 -w 是安全的，而且值得跑：**它是"这个窗口确实只读"的唯一正面证据**。
 * 光看源码不算 —— 白名单表历史上就漏过一行（槽 6 那次）。
 *
 * ============================================================================
 * 【DNA 不是秘密 —— 打印它是故意的】
 * ============================================================================
 * 有 JTAG 的人本来就能读 DNA（本项目就是这么先读到的）。它给的是**设备绑定 /
 * 防克隆**，不是机密性。所以这个工具直接把它打到 stdout，不做任何遮掩 ——
 * 假装它是秘密，反而会让人误以为派生出来的根也是秘密的。
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../kmod/secmmio_uapi.h"

#define DNA_LO 0xFFCA0050u
#define DNA_HI 0xFFCA005Cu

int main(int argc, char **argv)
{
	int try_write = (argc > 1 && strcmp(argv[1], "-w") == 0);
	uint32_t a;
	int fd, bad = 0;

	fd = open("/dev/secmmio", O_RDWR);
	if (fd < 0) {
		perror("open /dev/secmmio");
		return 1;
	}
	/* 保险：调用方声明 PL 已 programmed。DNA 在 CSU 里、与 PL 无关，
	 * 但驱动的保险是一道总闸，不解就什么都读不了。 */
	if (ioctl(fd, SECMMIO_ARM) < 0) {
		perror("SECMMIO_ARM");
		return 1;
	}

	printf("DNA = ");
	for (a = DNA_LO; a <= DNA_HI; a += 4) {
		struct secmmio_op op = { .addr = a, .val = 0 };
		if (ioctl(fd, SECMMIO_RD, &op) < 0) {
			printf("\n!!! 读 0x%08X 被拒 —— 白名单里没有这个窗口\n", a);
			bad = 1;
			break;
		}
		printf("%08x", op.val);
	}
	printf("\n");

	if (!bad && try_write) {
		struct secmmio_op op = { .addr = DNA_LO, .val = 0xDEADBEEF };
		if (ioctl(fd, SECMMIO_WR, &op) < 0) {
			printf("写 0x%08X 被 EL3 拒绝 —— 只读窗口成立\n", DNA_LO);
		} else {
			printf("!!! 写 0x%08X **成功了** —— 这个窗口不是只读的，白名单有错\n",
			       DNA_LO);
			bad = 1;
		}
	}
	close(fd);
	return bad;
}
