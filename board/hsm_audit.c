// hsm_audit —— 在真硅上复验这一轮审计修掉的五个 RTL 硬伤
//
//   aarch64-linux-gnu-gcc -O2 -static -o hsm_audit hsm_audit.c
//
// ============================================================================
// 【哪几条能在板上证，哪几条证不了 —— 先说清楚】
// ============================================================================
// 五项修复里，能从板上的 Linux 观测到的只有三项半。把能证的证扎实，
// 证不了的**明说是仿真覆盖的**，不含糊过去：
//
//   ① 地址别名     → **能证**。镜像地址必须 DECERR（表现为 SIGBUS），
//                     而唯一那个正确地址照常读得到。
//                     ⚠️ 只有"未对齐"那一档证不了：mmap /dev/mem 拿到的是
//                     Device 内存，未对齐的 32 位访问在 **CPU 上**就报对齐
//                     异常了，总线根本没看见 —— SIGBUS 来自哪一层分不开，
//                     所以这一档只在仿真里判（test_xbar.test_unaligned）。
//
//   ② BRAM 真擦除  → **半能证**。WIPING 位从 1 落到 0、耗时与 8192 拍相符、
//                     擦完照常能跑 —— 这些能证。但"两块 BRAM 逐字节是 0"
//                     **在板上证不了，而这恰恰是设计对的表现**：软件本来就
//                     没有任何一条路能读到那些字节（有的话才是漏洞）。
//                     全零的证据来自仿真里对 8192×2 个存储单元的读回
//                     （test_mlkem_axi.test_zeroize_really_wipes_bram）。
//
//   ③ tamper 同拍  → **证不了**，两个原因，都是硬的：
//                     · 这一版 bitstream 里 tamper 恒接 0（zu3eg_hsm_top.v），
//                       板上没有任何东西能把它拉起来；
//                     · 就算能，"与总线握手同一拍"是拍级的巧合，
//                       用户态程序没有办法安排。
//                     这一项只能由仿真判（test_firewall / test_key_vault_core）。
//
//   ④ TRNG 不丢样  → **能证**。DROPS 寄存器（0x34）必须恒为 0。
//                     ⚠️ 它只是个必要条件：DROPS=0 不等于"两条流逐比特相同"，
//                     那个由仿真逐比特对拍（test_trng_nodrop）。
//                     板上这一条防的是另一件事 —— 真硅上的时序与仿真不同，
//                     万一忙的时间比估计的长，FIFO 就真的会溢。
//
//   ⑤ 非法参数     → **能证**。mode=3 / pset=3 必须置 PARAM_ERR 且 BUSY 不动。
//
// ============================================================================
// 【一条用一次断电换来的：读的错误能接住，写的错误接不住】
// ============================================================================
// 第一版这里有一条"擦除期间写 IN_DATA 应当被拒（SLVERR）"的检查，装了
// SIGBUS 处理之后就当它和读一样安全。**跑下去板子当场冻住，只能断电。**
//
// 两者根本不是一回事：
//   · 读的 DECERR 是**同步**外部中止 —— 精确落在那条读指令上，
//     SIGBUS 送到进程，siglongjmp 回得来。这条路早就验证过很多次。
//   · 写是 **posted** 的：指令早就退休了，错误响应过一会儿才回来，
//     在 aarch64 上以 **SError** 的形式到达。SError 不属于任何一条指令，
//     内核只能 panic。而这时密码 bitstream 正载着 —— eth0 的 MAC 在厂家
//     PL 里，此刻根本不存在 —— 于是网络没了、harness 的恢复步骤没跑到、
//     sysrq 看门狗也随内核一起死了。表现就是彻底失联。
//
// 所以：**这个程序一笔"预期会失败的写"都不发。** 写侧的拒绝语义
// （擦除期间回 SLVERR）由仿真判（test_mlkem_axi.test_firewall_and_zeroize）。
//
// 输出直接落在 SD 卡上并逐行 fsync —— /tmp 是 tmpfs，冻住就什么都不剩。
// 每一节开头先落一个进度标记，万一再冻住能定位到具体哪一项。
#define _GNU_SOURCE
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define PL_BASE   0x80000000UL
/* 映到 2 MB：要去读 aperture 之外的地址（0x8010_0004），
 * 只映 6 个槽的话那笔访问连 mmap 都过不去，测的就成了 mmap 而不是译码。 */
#define PL_SPAN   0x00200000UL

#define S_TRNG    0x00000
#define S_VAULT   0x10000
#define S_SYM     0x20000
#define S_MLKEM   0x30000
#define S_CANARY  0x40000
#define S_FAN     0x50000

static volatile uint8_t *pl;
static FILE *rep;
static int n_pass, n_fail, n_skip;

/* ---- SIGBUS 安全访问 ----
 * 被译码拒掉的地址回 DECERR，在 aarch64 上是同步外部中止 → SIGBUS。
 * 这个程序的大半工作就是去读那些**应该读不到**的地址，所以必须能从
 * SIGBUS 里回来。 */
static sigjmp_buf busjmp;
static volatile int bus_armed, bus_hit;

static void on_bus(int sig)
{
    (void)sig;
    if (bus_armed) { bus_hit = 1; siglongjmp(busjmp, 1); }
    _exit(135);
}

static inline uint32_t rd(unsigned off)
{
    return *(volatile uint32_t *)(pl + off);
}
static inline void wr(unsigned off, uint32_t v)
{
    *(volatile uint32_t *)(pl + off) = v;
}

static uint32_t rd_safe(unsigned off, int *okp)
{
    uint32_t v = 0;
    bus_hit = 0;
    bus_armed = 1;
    if (sigsetjmp(busjmp, 1) == 0)
        v = rd(off);
    bus_armed = 0;
    if (okp) *okp = !bus_hit;
    return bus_hit ? 0 : v;
}

static void ok(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fprintf(rep, "  PASS  "); vfprintf(rep, fmt, ap); fprintf(rep, "\n");
    fflush(rep); va_end(ap); n_pass++;
}
static void bad(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fprintf(rep, "  FAIL  "); vfprintf(rep, fmt, ap); fprintf(rep, "\n");
    fflush(rep); va_end(ap); n_fail++;
}
static void skip(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fprintf(rep, "  SKIP  "); vfprintf(rep, fmt, ap); fprintf(rep, "\n");
    fflush(rep); va_end(ap); n_skip++;
}

/* 进度标记：每一节开头落一条并 fsync 到 SD 卡。
 * 上一次冻住的时候报告在 /tmp（tmpfs），断电之后什么都没剩下 ——
 * 于是"卡在哪一项"只能靠猜。这一条是为了下次不用猜。 */
static void mark(const char *what)
{
    fprintf(rep, "\n%s\n", what);
    fflush(rep);
    fsync(fileno(rep));
}

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/* ================= ① 地址别名 ================= */
#define MK_CTRL   (S_MLKEM + 0x04)
#define MK_STATUS (S_MLKEM + 0x08)
#define MK_MODE   (S_MLKEM + 0x0C)
#define MK_INDATA (S_MLKEM + 0x10)
#define MK_INPTR  (S_MLKEM + 0x14)
#define MK_OUTDAT (S_MLKEM + 0x18)
#define MK_OUTLEN (S_MLKEM + 0x1C)

#define MKC_START   1u
#define MKC_ZEROIZE 2u
#define MKC_IN_RST  4u
#define MKC_OUT_RST 8u

#define MKS_BUSY    (1u << 0)
#define MKS_DONE    (1u << 1)
#define MKS_WIPING  (1u << 4)
#define MKS_PARAMER (1u << 5)

static void test_aliases(void)
{
    struct { unsigned off; const char *what; } mirror[] = {
        { S_VAULT + 0x0110, "槽内偏移高位非零（审计点名的那个）" },
        { S_VAULT + 0x0100, "槽内偏移 +0x100" },
        { S_VAULT + 0x1100, "槽内偏移 +0x1100" },
        { S_MLKEM + 0xFF00, "槽内偏移 +0xFF00" },
        { S_TRNG  + 0x8000, "槽内偏移 +0x8000" },
        { 0x00100004,       "aperture 之外（旧译码会落到槽 0）" },
        { 0x00060000,       "槽号 6 ≥ NS" },
        { 0x00070000,       "槽号 7 ≥ NS" },
    };
    size_t i;
    int good;

    mark("① 地址别名：唯一那个地址通，镜像一律 DECERR");

    /* 正例先立住 —— 否则下面"全都读不到"可能只是因为整条路是死的 */
    struct { unsigned off; const char *name; } real[] = {
        { S_TRNG   + 0x20, "trng VERSION" },
        { S_VAULT  + 0x00, "key_vault VERSION" },
        { S_SYM    + 0x00, "sym VERSION" },
        { S_MLKEM  + 0x00, "mlkem VERSION" },
        { S_FAN    + 0x00, "fan VERSION" },
    };
    for (i = 0; i < sizeof(real) / sizeof(real[0]); i++) {
        uint32_t v = rd_safe(real[i].off, &good);
        if (good)
            ok("正例 %s @+0x%05x = 0x%08x", real[i].name, real[i].off, v);
        else
            bad("正例 %s @+0x%05x 竟然被拒 —— 译码把该通的也堵了",
                real[i].name, real[i].off);
    }

    for (i = 0; i < sizeof(mirror) / sizeof(mirror[0]); i++) {
        (void)rd_safe(mirror[i].off, &good);
        if (!good)
            ok("镜像 @+0x%05x 被拒（%s）", mirror[i].off, mirror[i].what);
        else
            bad("镜像 @+0x%05x **读得到** —— 别名还在（%s）",
                mirror[i].off, mirror[i].what);
    }

    skip("未对齐地址：Device 内存的未对齐访问在 CPU 上就报异常，"
         "与总线 DECERR 分不开 —— 只在仿真里判");
}

/* ================= ② zeroize 的可观测部分 ================= */
static int mk_run_keygen(unsigned pset, const unsigned char *dz,
                         unsigned char *out, unsigned cap, unsigned *outlen)
{
    unsigned i, n;
    long guard;

    wr(MK_MODE, 0u | (pset << 2));
    wr(MK_CTRL, MKC_IN_RST);
    for (i = 0; i < 64; i++)
        wr(MK_INDATA, dz[i]);
    if (rd(MK_INPTR) != 64) return -1;

    wr(MK_CTRL, MKC_START);
    for (guard = 0; guard < 2000000; guard++)
        if (rd(MK_STATUS) & MKS_DONE) break;
    if (guard >= 2000000) return -2;

    n = rd(MK_OUTLEN);
    if (n > cap) return -3;
    for (i = 0; i < n; i++)
        out[i] = (unsigned char)rd(MK_OUTDAT);
    *outlen = n;
    return 0;
}

static void test_zeroize(void)
{
    static unsigned char a[8192], b[8192];
    unsigned char dz[64];
    unsigned na = 0, nb = 0;
    unsigned i;
    uint32_t st;
    double t0, t_wipe;
    long guard;

    mark("② zeroize：WIPING 的行为（BRAM 全零由仿真判，板上无路可读）");

    for (i = 0; i < 32; i++) { dz[i] = (unsigned char)(0xA0 + i); }
    for (i = 32; i < 64; i++) { dz[i] = (unsigned char)(0x50 + i); }

    if (mk_run_keygen(0, dz, a, sizeof(a), &na) != 0) {
        bad("zeroize 前的 KeyGen 没跑成");
        return;
    }
    ok("zeroize 前 KeyGen-512 出 %u 字节", na);

    /* 擦除：写下去之后 WIPING 必须立刻是 1 */
    t0 = now_us();
    wr(MK_CTRL, MKC_ZEROIZE);
    st = rd(MK_STATUS);
    if (st & MKS_WIPING)
        ok("写 ZEROIZE 之后 STATUS.WIPING = 1（擦除机真的启动了）");
    else
        bad("写 ZEROIZE 之后 STATUS.WIPING = 0（STATUS=0x%08x）—— "
            "多半只清了指针", st);

    /* 擦除期间的写会回 SLVERR。**这条在板上不测** —— 写是 posted 的，
     * 错误以 SError 回来，内核只能 panic（见文件头，为此断过一次电）。 */
    skip("擦除期间写 IN_DATA 回 SLVERR：写错误在 aarch64 上是 SError，"
         "接不住、会 panic —— 由仿真判");

    for (guard = 0; guard < 20000000; guard++) {
        if (!(rd(MK_STATUS) & MKS_WIPING)) break;
    }
    t_wipe = now_us() - t0;
    if (guard >= 20000000) {
        bad("WIPING 一直没落下来（可能是用电平而不是上升沿触发）");
        return;
    }
    /* 8192 拍 @75 MHz ≈ 109 µs。轮询本身有开销，所以只判量级：
     * 太短说明根本没擦，太长说明擦除机在打转。 */
    if (t_wipe > 30.0 && t_wipe < 50000.0)
        ok("WIPING 持续 %.1f µs（8192 拍 @75 MHz ≈ 109 µs，含轮询开销）",
           t_wipe);
    else
        bad("WIPING 持续 %.1f µs —— 与 8192 拍对不上", t_wipe);

    if (rd(MK_OUTLEN) == 0 && rd(MK_INPTR) == 0)
        ok("擦除后 OUT_LEN = IN_PTR = 0");
    else
        bad("擦除后 OUT_LEN=%u IN_PTR=%u", rd(MK_OUTLEN), rd(MK_INPTR));

    if (mk_run_keygen(0, dz, b, sizeof(b), &nb) != 0) {
        bad("擦除之后再跑 KeyGen 没跑成");
        return;
    }
    if (nb == na && memcmp(a, b, na) == 0)
        ok("擦除后同一输入重跑，%u 字节逐字节相同（擦除没有损坏数据通路）", nb);
    else
        bad("擦除后重跑结果不同（%u vs %u 字节）", nb, na);

    skip("两块 8 KB BRAM 逐字节为 0：软件没有任何路径能读到那些字节"
         "（这正是设计对的表现），全零证据来自仿真的存储读回");
}

/* ================= ④ TRNG 取样 FIFO 不溢出 ================= */
#define TR_CTRL   (S_TRNG + 0x00)
#define TR_STATUS (S_TRNG + 0x04)
#define TR_RDATA  (S_TRNG + 0x08)
#define TR_BLOCKS (S_TRNG + 0x18)
#define TR_DROPS  (S_TRNG + 0x34)

#define TRS_READY  (1u << 0)
#define TRS_DVALID (1u << 1)
#define TRS_ALARM  (1u << 2)

static void test_trng_drops(void)
{
    uint32_t d0, d1, blk0, blk1, st;
    long i, got = 0;
    int good;

    mark("④ TRNG：取样 FIFO 一次都没溢出");

    d0 = rd_safe(TR_DROPS, &good);
    if (!good) {
        bad("DROPS 寄存器（+0x34）读不到 —— 这一版 bitstream 里没有它？");
        return;
    }
    blk0 = rd(TR_BLOCKS);
    st = rd(TR_STATUS);
    ok("起点：DROPS=%u BLOCKS=%u STATUS=0x%08x", d0, blk0, st);
    if (d0 != 0)
        bad("上电到现在已经溢出过 %u 次（没有人读随机字的那段时间）—— "
            "调理器被出口卡住了，丢样从'出口丢无害的输出'变成了'入口丢样本'",
            d0);

    /* 取一批随机字，逼调理器完整跑很多轮"吸收—置换—挤出"，
     * 也就是最容易丢样的那些拍。 */
    wr(TR_CTRL, 1u);                   /* ENABLE */
    {
        /* 用挂钟兜底而不是迭代次数：真硅上一次 MMIO 读多久是未知的，
         * 迭代上界折算成时间可能远超 harness 那条 480 秒看门狗 ——
         * 看门狗一响就是一次重启，而重启时密码 bitstream 还载着。 */
        double t_end = now_us() + 60e6;
        for (i = 0; got < 4096; i++) {
            if (rd(TR_STATUS) & TRS_DVALID) { (void)rd(TR_RDATA); got++; }
            if ((i & 0xFFFF) == 0 && now_us() > t_end) break;
        }
    }
    d1 = rd(TR_DROPS);
    blk1 = rd(TR_BLOCKS);

    if (got < 4096) {
        bad("只取到 %ld/4096 个随机字（STATUS=0x%08x）", got, rd(TR_STATUS));
        return;
    }
    ok("取了 %ld 个随机字，调理器吸收块数 %u → %u（+%u）",
       got, blk0, blk1, blk1 - blk0);

    if (d1 == 0)
        ok("DROPS 仍为 0 —— 取样 FIFO 一次都没溢出");
    else
        bad("DROPS = %u —— 真硅上的时序让 FIFO 溢了，"
            "SAMPLE_FIFO_DEPTH 要加大", d1);

    skip("两条流逐比特相同：板上取不到调理器入口的握手，由仿真逐比特对拍");
}

/* ================= ⑤ 非法 mode / pset ================= */
static void test_illegal_params(void)
{
    struct { unsigned mode, pset; const char *why; } bad_cfg[] = {
        { 3, 1, "mode=3" }, { 0, 3, "pset=3" }, { 3, 3, "两个都=3" },
    };
    size_t k;
    unsigned i;
    long guard;
    uint32_t st;
    int busy_seen;

    mark("⑤ 非法 mode / pset：START 那一刻被拒，核不启动");

    for (k = 0; k < sizeof(bad_cfg) / sizeof(bad_cfg[0]); k++) {
        wr(MK_MODE, bad_cfg[k].mode | (bad_cfg[k].pset << 2));
        if (rd(MK_MODE) != (bad_cfg[k].mode | (bad_cfg[k].pset << 2))) {
            bad("%s：MODE 回读不对，下面测的就不是这个配置了", bad_cfg[k].why);
            continue;
        }
        wr(MK_CTRL, MKC_IN_RST);
        for (i = 0; i < 64; i++) wr(MK_INDATA, 0x77);

        wr(MK_CTRL, MKC_START);
        busy_seen = 0;
        for (guard = 0; guard < 2000; guard++)
            if (rd(MK_STATUS) & MKS_BUSY) { busy_seen = 1; break; }
        st = rd(MK_STATUS);

        /* 判据三条。第三条不是"核有没有跑"（BUSY 已经答了），而是
         * **上一次的结果有没有被作废** —— 板上是连着跑的，拒绝之后若
         * DONE 与 OUT_LEN 还留着上一次的值，软件就会拿上一次的输出
         * 当成这一次的结果。这比不报错更糟，因为它看起来成功了。 */
        if (!(st & MKS_PARAMER))
            bad("%s：PARAM_ERR 没置位（STATUS=0x%08x）", bad_cfg[k].why, st);
        else if (busy_seen)
            bad("%s：报了 PARAM_ERR，但核**还是被启动了**", bad_cfg[k].why);
        else if (st & MKS_DONE)
            bad("%s：拒绝之后 DONE 仍是 1 —— 软件会读到上一次的结果",
                bad_cfg[k].why);
        else if (rd(MK_OUTLEN) != 0)
            bad("%s：拒绝之后 OUT_LEN=%u —— 上一次的输出还摆着",
                bad_cfg[k].why, rd(MK_OUTLEN));
        else
            ok("%s：PARAM_ERR 置位、BUSY 从未拉起、上一次的 DONE/OUT_LEN 已作废",
               bad_cfg[k].why);
    }

    /* 换回合法参数照常能跑，且错误位清掉 */
    {
        static unsigned char out[8192];
        unsigned char dz[64];
        unsigned n = 0;
        for (i = 0; i < 64; i++) dz[i] = (unsigned char)(0x30 + i);
        /* ML-KEM-768：ek 1184 + dk 2400 = 3584。
         * 上一版这里写的是 2400（只算了 dk），于是 RTL 给出正确的 3584
         * 反而被判成失败 —— 长度必须按标准算，不能凭印象。 */
        if (mk_run_keygen(1, dz, out, sizeof(out), &n) == 0 && n == 3584) {
            st = rd(MK_STATUS);
            if (st & MKS_PARAMER)
                bad("合法参数跑完之后 PARAM_ERR 还挂着（0x%08x）", st);
            else
                ok("换回合法参数（768）照常跑出 %u 字节，PARAM_ERR 已清", n);
        } else {
            bad("换回合法参数之后跑不动了（n=%u）", n);
        }
    }
}

int main(void)
{
    int fd;

    /* 直接落 SD 卡：/tmp 是 tmpfs，一旦冻住就什么都不剩 */
    rep = fopen("/media/sd-mmcblk1p2/hsm/RESULT_audit.txt", "w");
    if (!rep) return 1;

    fprintf(rep, "==== 审计修复上板复验 ====\n");
    fprintf(rep, "五项修复里能从板上观测的三项半在下面；\n");
    fprintf(rep, "证不了的都标成 SKIP 并写明由哪条仿真用例覆盖。\n");

    signal(SIGBUS, on_bus);

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { fprintf(rep, "打不开 /dev/mem\n"); fclose(rep); return 1; }
    pl = mmap(NULL, PL_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PL_BASE);
    if (pl == MAP_FAILED) { fprintf(rep, "mmap 失败\n"); fclose(rep); return 1; }

    test_aliases();
    test_zeroize();
    test_trng_drops();
    test_illegal_params();

    mark("③ tamper 同拍：板上无法复验");
    skip("这一版 bitstream 里 tamper 恒接 0（zu3eg_hsm_top.v），"
         "板上没有任何东西能拉起它");
    skip("即便能拉起，\"与总线握手同一拍\"是拍级巧合，用户态安排不了 —— "
         "由 test_firewall / test_key_vault_core 判");

    fprintf(rep, "\n==== 合计 PASS=%d FAIL=%d SKIP=%d ====\n",
            n_pass, n_fail, n_skip);
    fclose(rep);
    return n_fail ? 1 : 0;
}
