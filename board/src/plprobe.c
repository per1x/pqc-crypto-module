// plprobe —— 载入 bitstream 之后的最小验证：只读五个 VERSION 寄存器
//
// 单独做一个最小探针，而不是直接跑整套自测，是因为**上一次整套一把梭之后
// 板子硬挂、日志全空，什么都没学到**。这个程序只做一件事，配合逐行 sync
// 的日志，就能把"载入失败"和"访问挂死"分开。
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
    static const char *nm[5] = { "trng", "key_vault", "sym", "mlkem", "canary" };
    volatile uint8_t *pl;
    int fd, i;

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open"); return 1; }
    pl = mmap(NULL, 0x50000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x80000000UL);
    if (pl == MAP_FAILED) { perror("mmap"); return 1; }

    for (i = 0; i < 5; i++) {
        uint32_t v;
        printf("读 slot%d (%s) +0x00 ...\n", i, nm[i]);
        fflush(stdout);
        sync();
        v = *(volatile uint32_t *)(pl + i * 0x10000);
        printf("  slot%d %-10s VERSION = 0x%08x\n", i, nm[i], v);
        fflush(stdout);
        sync();
    }
    printf("五个从机都读到了，AXI 通路是活的\n");
    return 0;
}
