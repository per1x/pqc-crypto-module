// rdreg <addr> —— 读一个 32 位物理地址。只做这一件事。
// 用它而不是整套自测，是为了让「AXI 通路活不活」这个问题单独可答。
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    unsigned long a = strtoul(argc > 1 ? argv[1] : "0x80000000", NULL, 0);
    unsigned long pg = a & ~0xFFFUL, off = a & 0xFFF;
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    volatile uint8_t *m;
    if (fd < 0) { perror("open"); return 1; }
    m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, pg);
    if (m == MAP_FAILED) { perror("mmap"); return 1; }
    printf("读 0x%08lx ...\n", a); fflush(stdout); sync();
    printf("0x%08lx = 0x%08x\n", a, *(volatile uint32_t *)(m + off));
    fflush(stdout); sync();
    return 0;
}
