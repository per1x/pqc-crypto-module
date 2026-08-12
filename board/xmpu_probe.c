/* xmpu_probe —— 从普通世界读 ZynqMP 的 XMPU / XPPU 实配，并做一次反证
 *
 * ============================================================================
 * 【这个工具回答什么，不回答什么】
 * ============================================================================
 * 回答：**这块板此刻实际生效的内存保护是什么样子**。XMPU/XPPU 是由 FSBL /
 *       PMUFW 在启动时配好的，配成什么样不能靠读文档猜，只能读硬件。
 *
 * 不回答：**怎么把它配成我想要的样子**。XMPU/XPPU 的配置寄存器本身受
 *       XPPU 保护，普通世界（EL1-NS）改不了 —— 那需要一个安全世界主控
 *       （OP-TEE / ATF）。这一半明确留给那条线，本工具只做"现状 + 反证"。
 *
 * ============================================================================
 * 【为什么每一次读都要 SIGBUS 兜底】
 * ============================================================================
 * 被 XPPU 挡掉的读在 APU 这一侧表现为 **SError / 总线错误**，Linux 转成
 * SIGBUS 打到进程头上。没有兜底的话，这个工具在第一个读不到的寄存器上就
 * 死了 —— 而"读不到"恰恰是我要记录的结果之一，不是失败。
 * 所以每个读都包在 setjmp/longjmp 里，读不到就记一条"被挡"继续往下走。
 */
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static sigjmp_buf jb;
static volatile int trapped;

static void on_bus(int sig)
{
    (void)sig;
    trapped = 1;
    siglongjmp(jb, 1);
}

static int fdmem = -1;

/* 读一个 32 位物理地址；被挡返回 -1 并把 *out 置 0 */
static int rd_phys(unsigned long pa, uint32_t *out)
{
    unsigned long pg = pa & ~0xFFFUL, off = pa & 0xFFF;
    volatile uint8_t *m;
    int rc = 0;

    m = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fdmem, pg);
    if (m == MAP_FAILED) { *out = 0; return -2; }

    trapped = 0;
    if (sigsetjmp(jb, 1) == 0) {
        *out = *(volatile uint32_t *)(m + off);
    } else {
        *out = 0;
        rc = -1;
    }
    munmap((void *)m, 0x1000);
    return rc;
}

static void show(const char *name, unsigned long pa)
{
    uint32_t v;
    int rc = rd_phys(pa, &v);
    if (rc == 0)
        printf("  %-28s @0x%08lx = 0x%08x\n", name, pa, v);
    else if (rc == -1)
        printf("  %-28s @0x%08lx = **总线错误（硬件挡的）**\n", name, pa);
    else
        printf("  %-28s @0x%08lx = **映射失败（内核不给，不是硬件挡的）**\n",
               name, pa);
    /* 这两种必须分清：前者说明 XPPU/XMPU 真的在拦，后者只说明
     * /dev/mem 这条路被内核限制了，跟硬件隔离没关系。 */
    fflush(stdout);
}

/* XMPU 一个区域的四个寄存器 */
static void show_region(const char *inst, unsigned long base, int n)
{
    unsigned long r = base + 0x100 + (unsigned long)n * 0x10;
    uint32_t s, e, m, c;
    int rs = rd_phys(r + 0x0, &s);
    int re = rd_phys(r + 0x4, &e);
    int rm = rd_phys(r + 0x8, &m);
    int rc = rd_phys(r + 0xC, &c);
    if (rs || re || rm || rc) {
        printf("  %s R%02d：%s\n", inst, n,
               (rs == -1 || re == -1 || rm == -1 || rc == -1)
                   ? "总线错误（硬件挡的）" : "映射失败（内核不给）");
        return;
    }
    if (!(c & 1) && s == 0 && e == 0)
        return;                         /* 没启用且没配，跳过不刷屏 */
    printf("  %s R%02d：start=0x%08x end=0x%08x master=0x%08x cfg=0x%08x"
           "  [%s %s%s%s]\n",
           inst, n, s, e, m, c,
           (c & 1) ? "启用" : "关闭",
           (c & 2) ? "读许可 " : "读禁止 ",
           (c & 4) ? "写许可 " : "写禁止 ",
           (c & 8) ? "仅安全" : "非安全可");
    fflush(stdout);
}

int main(void)
{
    struct sigaction sa;
    uint32_t v;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_bus;
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);

    fdmem = open("/dev/mem", O_RDWR | O_SYNC);
    if (fdmem < 0) { perror("open /dev/mem"); return 1; }

    printf("=== ZynqMP 内存保护实配（普通世界视角） ===\n\n");

    printf("[XPPU（LPD 从机的许可表）base 0xFF980000]\n");
    show("XPPU_CTRL",        0xFF980000);
    show("XPPU_ERR_STATUS1", 0xFF980004);
    show("XPPU_ERR_STATUS2", 0xFF980008);
    show("XPPU_POISON",      0xFF98000C);
    show("XPPU_ISR",         0xFF980010);
    show("XPPU_IMR",         0xFF980014);
    show("XPPU_LOCK",        0xFF98001C);

    printf("\n[XMPU_DDR0..5（DDR 的区域保护）]\n");
    {
        int i, n;
        static const unsigned long ddr[6] = {
            0xFD000000, 0xFD010000, 0xFD020000,
            0xFD030000, 0xFD040000, 0xFD050000
        };
        for (i = 0; i < 6; i++) {
            char nm[32];
            snprintf(nm, sizeof nm, "XMPU_DDR%d CTRL", i);
            show(nm, ddr[i] + 0x00);
            for (n = 0; n < 16; n++)
                show_region("  DDR", ddr[i], n);
        }
    }

    printf("\n[XMPU_FPD base 0xFD5D0000]\n");
    show("XMPU_FPD CTRL",   0xFD5D0000);
    show("XMPU_FPD ISR",    0xFD5D0010);
    show("XMPU_FPD LOCK",   0xFD5D001C);
    { int n; for (n = 0; n < 16; n++) show_region("  FPD", 0xFD5D0000, n); }

    printf("\n[XMPU_OCM base 0xFFA70000]\n");
    show("XMPU_OCM CTRL",   0xFFA70000);
    show("XMPU_OCM ISR",    0xFFA70010);
    { int n; for (n = 0; n < 16; n++) show_region("  OCM", 0xFFA70000, n); }

    printf("\n[反证：普通世界够不到的东西]\n");
    /* CSU —— 安全世界的配置控制器。普通世界读到它就说明隔离是坏的。 */
    if (rd_phys(0xFFCA0000, &v) == 0)
        printf("  ⚠️ CSU_STATUS 竟然读到了 = 0x%08x —— 隔离没生效\n", v);
    else
        printf("  ok  CSU_STATUS(0xFFCA0000) 读不到 —— 普通世界确实够不到 CSU\n");
    if (rd_phys(0xFFCA0010, &v) == 0)
        printf("  ⚠️ CSU_MULTI_BOOT 竟然读到了 = 0x%08x\n", v);
    else
        printf("  ok  CSU_MULTI_BOOT(0xFFCA0010) 读不到\n");
    /* eFuse 控制器 */
    if (rd_phys(0xFFCC0000, &v) == 0)
        printf("  注  eFUSE 控制器(0xFFCC0000) 读到 = 0x%08x\n", v);
    else
        printf("  ok  eFUSE 控制器(0xFFCC0000) 读不到\n");

    printf("\n[对照：同一条 /dev/mem 路径读 PL 是通的]\n");
    /* 这一条是必须的：不然"全都读不到"没法区分是硬件在挡，
     * 还是这条访问路径本身就不通。PL 那段是同一个 /dev/mem、
     * 同样的 mmap+读，能读到就说明路径没问题，挡是硬件在挡。 */
    if (rd_phys(0x80030000, &v) == 0)
        printf("  ok  PL ML-KEM VERSION(0x80030000) = 0x%08x —— 路径本身是通的\n", v);
    else
        printf("  注  PL 也读不到（可能没装 bitstream），本次对照不成立\n");

    printf("\n=== 说明 ===\n");
    printf("  · 以上是**读**的结果。改这些寄存器需要安全世界主控，\n");
    printf("    普通世界改不了 —— 那一半留给 OP-TEE/ATF 那条线。\n");
    printf("  · PL 里那些密码核的边界不靠 XMPU，靠的是我在 PL 内做的\n");
    printf("    AxPROT 门控防火墙（金丝雀实例已在真硅上验过 DECERR）。\n");
    printf("    XMPU 能加的是「哪些主控允许发到 PL 那段地址」，\n");
    printf("    与 PL 内的门控是两层，互相不替代。\n");
    return 0;
}
