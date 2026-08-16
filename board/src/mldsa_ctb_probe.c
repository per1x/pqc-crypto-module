/* mldsa_ctb_probe —— 在真硅上判定"孤立 Verify 被拒"到底是谁干的
 *
 *   aarch64-linux-gnu-gcc -O2 -Wall -Wextra -static -o mldsa_ctb_probe mldsa_ctb_probe.c
 *
 * ============================================================================
 * 【要分开的两个假说】
 * ============================================================================
 * 上板实测（开发形态位流，每套 300 次）：纯 Verify、前面不夹同参数集的
 * Sign/KeyGen 时 ML-DSA-44 与 65 失败 100%，87 失败 0%。
 *
 *   假说 A（运行时参数集没被应用）：孤立 Verify 进来时 pset 是陈旧/默认值，
 *          于是用错参数、把本来正确的 σ 拒掉。
 *   假说 B（c̃ 的高位字节是上一次留下的）：verify.v 的判定是
 *          `ctilde_p == ctilde` 的**整 512 位**比较，而两个寄存器每次只写
 *          低 ctb 字节（44/65/87 → 32/48/64）。于是高位是上一次运算的残留：
 *          上一次**判否**过 ⇒ 高位不等 ⇒ 之后每一条小参数集的合法签名都被拒。
 *
 * 两个假说对 100%/0% 的解释一样漂亮，但它们对下面这几步的预言完全不同 ——
 * 这个程序就是去看哪一个对。
 *
 *   序列①  87 通过 → 44 → 65 → 44        A：44/65 该拒   B：全过
 *   序列②  87 判否 → 44 → 65 → 44        A：44/65 该拒   B：全拒
 *   序列③  87 通过 → 44 → 44 → 44        A：44 该拒      B：全过
 *   序列④  87 通过 → 65 判否 → 65 → 44   A：65/44 都拒
 *                                        B：**65 过、44 拒**  ← 决定性
 *
 * 序列④ 是关键：65 判否只弄脏 [0,48) 的字节，而这些字节下一次 65 运算会
 * 整段重写；44 只写 [0,32)，于是它读到的 [32,48) 正是 65 那次判否留下的
 * 脏字节。**"同一次污染之后 65 过而 44 拒"是假说 B 独有的预言** ——
 * 任何"pset 没被应用"的说法都没法让同一时刻的 65 对、44 错。
 *
 * ⚠️ 本程序只读写槽 6 的寄存器，**不重配 PL、不碰网络**。写之前一律先等
 *    从机 IDLE（写被拒在 aarch64 上是 SError，接不住、只能断电，见
 *    mldsa_hwtest.c 里那一大段）。
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "kat_mldsa.h"

#define MD_VERSION 0x00
#define MD_CTRL    0x04
#define MD_MODE    0x08
#define MD_STATUS  0x0C
#define MD_INDATA  0x10
#define MD_INPTR   0x14
#define MD_OUTPTR  0x1C
#define MD_MSGLEN  0x24
#define MD_CTXLEN  0x28

#define CORE_VERSION 0x00010000u
#define C_START   (1u << 0)
#define C_CLEAR   (1u << 1)
#define ST_BUSY   (1u << 0)
#define ST_DONE   (1u << 1)
#define ST_VOK    (1u << 2)
#define ST_PARERR (1u << 3)
#define ST_LENERR (1u << 4)
#define ST_WIPING (1u << 6)
#define OP_VERIFY 2u

/* 序列⑥ 连做多少次。上一条会话用的是 300 次；这里只要证明"没有中间值"，
 * 20 次就够看，而且每条 Verify 实测只要 1~3 ms。 */
#define REP_N 20

static unsigned long base_addr = 0x80060000UL;
static volatile unsigned char *slot_map;
static FILE *rep;
static int hw_stuck;

static sigjmp_buf busjmp;
static volatile sig_atomic_t bus_armed, bus_hit;

static void on_bus(int sig)
{
    static const char m[] = "\n*** SIGBUS：PL 总线拒绝了一次访问。\n";

    (void)sig;
    if (bus_armed) { bus_hit = 1; siglongjmp(busjmp, 1); }
    if (write(2, m, sizeof m - 1) < 0) { /* 没什么可做的了 */ }
    _exit(4);
}

static inline uint32_t rd(unsigned off)
{
    return *(volatile uint32_t *)(slot_map + off);
}

static inline void wr(unsigned off, uint32_t v)
{
    *(volatile uint32_t *)(slot_map + off) = v;
}

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* 写之前必须确认 IDLE —— 理由见文件头。等不到就粘住，再也不写。 */
static int wait_idle(long ms)
{
    long deadline = now_ms() + ms;

    for (;;) {
        uint32_t st = rd(MD_STATUS);

        if (!(st & (ST_BUSY | ST_WIPING)))
            return 0;
        if (now_ms() > deadline) {
            hw_stuck = 1;
            fprintf(rep, "\n*** 从机 %ld ms 之后仍然 BUSY/WIPING"
                    "（STATUS=0x%08x）——**停止一切写操作**。\n", ms, st);
            fflush(rep);
            return -1;
        }
    }
}

static unsigned char inbuf[16384];

/* 跑一条 Verify。返回 1=verify_ok，0=判否，<0=没跑成 */
static int verify_one(const mldsa_sv_vec *v, long *ms)
{
    unsigned in_len = 0, i;
    uint32_t st;
    long t0, deadline;

    if (hw_stuck || wait_idle(3000))
        return -1;

    memcpy(inbuf, v->pk, v->pk_len);            in_len  = v->pk_len;
    memcpy(inbuf + in_len, v->sig, v->sig_len); in_len += v->sig_len;
    if (v->ctx_len) {
        memcpy(inbuf + in_len, v->ctx, v->ctx_len);
        in_len += v->ctx_len;
    }
    memcpy(inbuf + in_len, v->msg, v->msg_len); in_len += v->msg_len;

    /* 顺序照 mldsa_hwtest.c：MODE 在最前（写 MODE 会清 IN_PTR）*/
    wr(MD_MODE, OP_VERIFY | ((unsigned)v->pset << 2));
    wr(MD_CTRL, C_CLEAR);
    wr(MD_INPTR, 0);
    wr(MD_OUTPTR, 0);
    wr(MD_MSGLEN, v->msg_len);
    wr(MD_CTXLEN, v->ctx_len);
    for (i = 0; i < in_len; i++)
        wr(MD_INDATA, inbuf[i]);
    if (rd(MD_INPTR) != in_len) {
        fprintf(rep, "    IN_PTR 对不上（%u ≠ %u）\n", rd(MD_INPTR), in_len);
        return -1;
    }

    t0 = now_ms();
    wr(MD_CTRL, C_START);
    deadline = t0 + 2000L * (v->pset + 1) + (long)in_len / 8;
    for (;;) {
        st = rd(MD_STATUS);
        if (st & (ST_PARERR | ST_LENERR)) {
            fprintf(rep, "    START 被拒（STATUS=0x%08x）\n", st);
            return -1;
        }
        if (st & ST_DONE)
            break;
        if (now_ms() > deadline) {
            hw_stuck = 1;
            fprintf(rep, "    等 done 超时（STATUS=0x%08x）\n", st);
            return -1;
        }
    }
    if (ms)
        *ms = now_ms() - t0;
    return (st & ST_VOK) ? 1 : 0;
}

/* 跑一条 KeyGen / Sign —— 只为"在两条 Verify 之间夹一次同参数集的运算"，
 * 不核对输出。上一条会话记的规避是"Verify 前只要有同参数集的运算就必过"，
 * 这两个函数就是去证伪它：假说 B 说这两个核根本碰不到 verify 的 c̃ 寄存器，
 * 夹进来不该有任何影响。*/
static int run_op_nocheck(unsigned op, unsigned pset, const unsigned char *in,
                          unsigned in_len, unsigned msg_len, unsigned ctx_len)
{
    unsigned i;
    uint32_t st;
    long deadline;

    if (hw_stuck || wait_idle(3000))
        return -1;
    wr(MD_MODE, op | (pset << 2));
    wr(MD_CTRL, C_CLEAR);
    wr(MD_INPTR, 0);
    wr(MD_OUTPTR, 0);
    wr(MD_MSGLEN, msg_len);
    wr(MD_CTXLEN, ctx_len);
    for (i = 0; i < in_len; i++)
        wr(MD_INDATA, in[i]);
    if (rd(MD_INPTR) != in_len)
        return -1;
    wr(MD_CTRL, C_START);
    deadline = now_ms() + 20000L;
    for (;;) {
        st = rd(MD_STATUS);
        if (st & (ST_PARERR | ST_LENERR))
            return -1;
        if (st & ST_DONE)
            return 0;
        if (now_ms() > deadline) { hw_stuck = 1; return -1; }
    }
}

/* 夹一次同参数集的 KeyGen + Sign（照上一条会话记的规避原样做）*/
static void interpose_kg_sign(int pset)
{
    static const unsigned char rnd0[32] = { 0 };
    const mldsa_kg_vec *kg = NULL;
    const mldsa_sg_vec *sg = NULL;
    unsigned i, n = 0;

    for (i = 0; i < sizeof MLDSA_KG / sizeof MLDSA_KG[0]; i++)
        if (MLDSA_KG[i].pset == pset) { kg = &MLDSA_KG[i]; break; }
    for (i = 0; i < sizeof MLDSA_SG / sizeof MLDSA_SG[0]; i++)
        if (MLDSA_SG[i].pset == pset) { sg = &MLDSA_SG[i]; break; }
    if (!kg || !sg) return;

    fprintf(rep, "      （夹一次 %s 的 KeyGen：%s）\n", MLDSA_SET_NAME[pset],
            run_op_nocheck(0u, (unsigned)pset, kg->xi, 32, 0, 0) ? "没跑成"
                                                                 : "跑完");
    memcpy(inbuf, sg->sk, sg->sk_len);          n  = sg->sk_len;
    memcpy(inbuf + n, rnd0, 32);                n += 32;
    if (sg->ctx_len) { memcpy(inbuf + n, sg->ctx, sg->ctx_len); n += sg->ctx_len; }
    memcpy(inbuf + n, sg->msg, sg->msg_len);    n += sg->msg_len;
    fprintf(rep, "      （夹一次 %s 的 Sign：%s）\n", MLDSA_SET_NAME[pset],
            run_op_nocheck(1u, (unsigned)pset, inbuf, n, sg->msg_len,
                           sg->ctx_len) ? "没跑成" : "跑完");
    fflush(rep);
}

/* 按 (pset, 是否 ACVP 判通过, 第几条) 找向量 */
static const mldsa_sv_vec *pick(int pset, int expect_ok, int nth)
{
    unsigned i;
    int seen = 0;

    for (i = 0; i < sizeof MLDSA_SV / sizeof MLDSA_SV[0]; i++)
        if (MLDSA_SV[i].pset == pset && MLDSA_SV[i].expect_ok == expect_ok)
            if (seen++ == nth)
                return &MLDSA_SV[i];
    return NULL;
}

/* 两套记分：
 *   · 与**假说 B 的预言**是否相符 —— 修之前用它认病因；
 *   · 与 **ACVP 的判定**是否相符 —— 修之后用它验对错。
 * 同一个程序在修前修后各跑一次，两套数正好对调：修之前 B 全中、ACVP 有错；
 * 修之后 ACVP 全中，而 B 预言的那些"该被误拒"的步骤全部不再成立。 */
static int n_as_predicted_B, n_against_B;
static int n_acvp_ok, n_acvp_bad;

/* 跑一步并与**假说 B 的预言**对账。pred_B：1=该过 0=该拒 */
static void step(const char *tag, int pset, int expect_ok, int nth, int pred_B)
{
    const mldsa_sv_vec *v = pick(pset, expect_ok, nth);
    long ms = 0;
    int got;

    if (!v) { fprintf(rep, "  %s：找不到向量\n", tag); return; }
    got = verify_one(v, &ms);
    if (got < 0) {
        fprintf(rep, "  %-26s %-10s tcId=%-3d 没跑成\n", tag,
                MLDSA_SET_NAME[pset], v->tc);
        return;
    }
    fprintf(rep, "  %-26s %-10s tcId=%-3d ACVP=%s  硬件 verify_ok=%d  "
            "(%ld ms)  ACVP:%s  假说B预言=%d:%s\n",
            tag, MLDSA_SET_NAME[pset], v->tc, expect_ok ? "pass" : "fail",
            got, ms, got == expect_ok ? "✅" : "❌ 判错",
            pred_B, got == pred_B ? "相符" : "不符");
    if (got == pred_B)
        n_as_predicted_B++;
    else
        n_against_B++;
    if (got == expect_ok)
        n_acvp_ok++;
    else
        n_acvp_bad++;
    fflush(rep);
}

static void probe(void)
{
    volatile uint32_t ver = 0;

    bus_hit = 0;
    bus_armed = 1;
    if (sigsetjmp(busjmp, 1) == 0)
        ver = rd(MD_VERSION);
    bus_armed = 0;
    if (bus_hit || ver != CORE_VERSION) {
        fprintf(stderr, "VERSION 读回 0x%08x（应当 0x%08x）——**不继续**，"
                "一个字节都不写。要的是开发形态位流。\n",
                (unsigned)ver, CORE_VERSION);
        exit(2);
    }
    fprintf(rep, "基址 0x%08lx  VERSION=0x%08x  STATUS=0x%08x\n",
            base_addr, (unsigned)ver, rd(MD_STATUS));
}

int main(int argc, char **argv)
{
    int fd, c;
    const char *outfile = NULL;
    unsigned long page, off;
    void *m;

    rep = stdout;
    while ((c = getopt(argc, argv, "b:o:")) != -1) {
        switch (c) {
        case 'b': base_addr = strtoul(optarg, NULL, 0); break;
        case 'o': outfile = optarg; break;
        default:  fprintf(stderr, "用法：%s [-b 基址] [-o 报告]\n", argv[0]);
                  return 1;
        }
    }
    if (outfile) {
        rep = fopen(outfile, "w");
        if (!rep) { perror(outfile); return 1; }
    }

    signal(SIGBUS, on_bus);
    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return 2; }
    page = (unsigned long)sysconf(_SC_PAGESIZE);
    off  = base_addr & ~(page - 1);
    m = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)off);
    if (m == MAP_FAILED) { perror("mmap"); return 2; }
    slot_map = (volatile unsigned char *)m + (base_addr - off);

    fprintf(rep, "ML-DSA 孤立 Verify 归因探针（开发形态位流，经 /dev/mem）\n");
    fprintf(rep, "假说 A = 运行时参数集没被应用；假说 B = c̃ 高位字节是上一次"
            "残留\n\n");
    probe();

    fprintf(rep, "\n【序列①】先用一条**通过**的 87 把 64 个字节全写一遍，"
            "再连做小参数集\n");
    fprintf(rep, "         假说 A 预言 44/65 仍被拒；假说 B 预言全过\n");
    step("①-1 87 通过（洗干净）", 2, 1, 0, 1);
    step("①-2 44 合法签名",       0, 1, 0, 1);
    step("①-3 65 合法签名",       1, 1, 0, 1);
    step("①-4 44 合法签名",       0, 1, 0, 1);

    fprintf(rep, "\n【序列②】改成用一条**判否**的 87 起头，别的一模一样\n");
    fprintf(rep, "         假说 A 预言与序列①相同；假说 B 预言 44/65 全被拒\n");
    step("②-1 87 判否（弄脏）",   2, 0, 0, 0);
    step("②-2 44 合法签名",       0, 1, 0, 0);
    step("②-3 65 合法签名",       1, 1, 0, 0);
    step("②-4 44 合法签名",       0, 1, 0, 0);

    fprintf(rep, "\n【序列③】再洗一次 —— 说明它可逆，不是一次性闩死\n");
    step("③-1 87 通过（洗干净）", 2, 1, 0, 1);
    step("③-2 44 合法签名",       0, 1, 0, 1);
    step("③-3 44 合法签名（连做）", 0, 1, 1, 1);

    fprintf(rep, "\n【序列④ —— 决定性】改用 **65** 判否来弄脏\n");
    fprintf(rep, "         65 判否只弄脏 [0,48)，而 65 自己下一次会整段重写；\n");
    fprintf(rep, "         44 只写 [0,32)，于是它读到的 [32,48) 是脏的。\n");
    fprintf(rep, "         假说 B 预言：**同一次污染之后 65 过、44 拒**。\n");
    fprintf(rep, "         任何'pset 没被应用'的说法都给不出这个分裂。\n");
    fprintf(rep, "         ⚠️ 次序要紧：44 必须在 65 之前问 —— 一条**通过**的\n"
            "         65 会把 [0,48) 整段重写成相等，等于顺手替 44 洗干净了。\n");
    step("④-1 87 通过（洗干净）", 2, 1, 0, 1);
    step("④-2 65 判否（弄脏低48）", 1, 0, 0, 0);
    step("④-3 44 合法签名",       0, 1, 0, 0);   /* B：拒（读到脏的 [32,48)）*/
    step("④-4 65 合法签名",       1, 1, 0, 1);   /* B：过（自己整段重写）  */
    step("④-5 44 合法签名",       0, 1, 0, 1);   /* B：过（上一步顺手洗了）*/

    fprintf(rep, "\n【序列⑤】直接考上一条会话记下的那条规避：\n");
    fprintf(rep, "         \"Verify 前只要有同参数集的运算就必过\"\n");
    fprintf(rep, "         假说 B 说 KeyGen/Sign 根本碰不到 verify 的 c̃ "
            "寄存器，夹进来**没有任何用**\n");
    step("⑤-1 87 判否（弄脏）",   2, 0, 0, 0);
    step("⑤-2 44 合法签名",       0, 1, 0, 0);
    interpose_kg_sign(0);
    step("⑤-3 44 同一条再来一次", 0, 1, 0, 0);   /* B：夹了也还是拒 */

    fprintf(rep, "\n【序列⑥】把上一条会话那个 100%%/0%% 直接复现出来\n");
    fprintf(rep, "         同一条 44 合法签名连做 %d 次，中间什么都不夹 ——\n"
            "         脏字节没人碰，于是这 %d 次要么全败要么全过，"
            "不会有中间值。\n", REP_N, REP_N);
    {
        int k;

        step("⑥-1 87 判否（弄脏）", 2, 0, 0, 0);
        for (k = 0; k < REP_N; k++) {
            char t[48];

            snprintf(t, sizeof t, "⑥-2.%-2d 44 同一条", k + 1);
            step(t, 0, 1, 0, 0);            /* B：全败 */
        }
        step("⑥-3 87 通过（洗干净）", 2, 1, 0, 1);
        for (k = 0; k < REP_N; k++) {
            char t[48];

            snprintf(t, sizeof t, "⑥-4.%-2d 44 同一条", k + 1);
            step(t, 0, 1, 0, 1);            /* B：全过 */
        }
    }

    fprintf(rep, "\n与 ACVP 的判定相符 %d 步 / 判错 %d 步\n",
            n_acvp_ok, n_acvp_bad);
    fprintf(rep, "与假说 B 的预言相符 %d 步 / 不符 %d 步\n",
            n_as_predicted_B, n_against_B);
    if (!n_against_B && n_acvp_bad)
        fprintf(rep, "→ **修之前**的样子：病因确认为"
                "「c̃ 比较看了不属于本次运算的高位字节」。\n");
    else if (!n_acvp_bad)
        fprintf(rep, "→ **修之后**的样子：无论前一次判通过还是判否、"
                "无论跨不跨参数集，判定全部与 ACVP 一致。\n");
    else
        fprintf(rep, "→ 两套预言都没全中 —— 还有别的东西，别急着收工。\n");
    fflush(rep);
    return n_acvp_bad ? 1 : 0;
}
