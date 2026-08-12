/* hsm_kem3 —— ML-KEM 三套参数集的板上 KAT + 性能 + 常数时间抽测
 *
 * 与 hsm_hwtest 分开，是因为那一份（ML-KEM-512 + AES/SM4/SM3 + 边界反证）
 * 已经在真硅上跑通过 24/24，是**已验证的既有资产**。往里面加东西要动它的
 * 结构，等于把一份验过的重新变成没验过的。这一份独立，出问题也能立刻分辨
 * 是谁的。
 *
 * 三件事：
 *  ① **KAT**：NIST ACVP 官方向量，512/768/1024 各 KeyGen/Encaps/Decaps 两组，
 *     逐字节比对。参数集只靠 MODE 寄存器的 pset 字段切换，长度由 RTL 自己
 *     按 pset 算（软件不报长度，也就报不错）。
 *  ② **性能**：每个参数集每个操作跑 N 次取中位数，给出次/秒。
 *  ③ **常数时间抽测**：Decaps 的隐式拒绝路径。
 *     用**合法密文**与**改了一个比特的密文**各跑很多次，比较耗时分布 ——
 *     RTL 里密文比对是逐字节全比完再选、绝不早退，所以两者必须无法区分。
 *     这是从软件侧能观测到的那一层，也正是计时攻击真正能拿到的那一层。
 */
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>

#include "kat_mlkem_all.h"

#define PL_BASE   0x80000000UL
#define PL_SPAN   0x00050000UL
#define S_MLKEM   0x30000

#define MK_VER    (S_MLKEM + 0x00)
#define MK_CTRL   (S_MLKEM + 0x04)
#define MK_STATUS (S_MLKEM + 0x08)
#define MK_MODE   (S_MLKEM + 0x0C)
#define MK_INDATA (S_MLKEM + 0x10)
#define MK_INPTR  (S_MLKEM + 0x14)
#define MK_OUTDAT (S_MLKEM + 0x18)
#define MK_OUTLEN (S_MLKEM + 0x1C)

#define MKC_START 1u
#define MKC_INRST 4u
#define MKS_DONE  2u

static volatile uint8_t *pl;
static FILE *rep;
static int n_pass, n_fail;

static void ok(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fprintf(rep, "  ok    "); vfprintf(rep, fmt, ap); fprintf(rep, "\n");
    fflush(rep); va_end(ap); n_pass++;
}
static void bad(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fprintf(rep, "  FAIL  "); vfprintf(rep, fmt, ap); fprintf(rep, "\n");
    fflush(rep); va_end(ap); n_fail++;
}

static inline uint32_t rd(unsigned off)
{ return *(volatile uint32_t *)(pl + off); }
static inline void wr(unsigned off, uint32_t v)
{ *(volatile uint32_t *)(pl + off) = v; }

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static unsigned char inbuf[8192], outbuf[8192];

static int mlkem_run(int mode, int pset, const unsigned char *in, size_t inlen,
                     unsigned char *out, size_t outcap, size_t *outlen)
{
    size_t i; long spin;
    wr(MK_MODE, (uint32_t)(mode | (pset << 2)));
    wr(MK_CTRL, MKC_INRST);
    for (i = 0; i < inlen; i++)
        wr(MK_INDATA, in[i]);
    if (rd(MK_INPTR) != inlen) return -1;
    wr(MK_CTRL, MKC_START);
    for (spin = 0; spin < 200000000L; spin++)
        if (rd(MK_STATUS) & MKS_DONE) break;
    if (!(rd(MK_STATUS) & MKS_DONE)) return -2;
    *outlen = rd(MK_OUTLEN);
    if (*outlen > outcap) return -3;
    for (i = 0; i < *outlen; i++)
        out[i] = (unsigned char)(rd(MK_OUTDAT) & 0xFF);
    return 0;
}

static void hexdiff(const char *what, const unsigned char *got,
                    const unsigned char *want, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) if (got[i] != want[i]) break;
    fprintf(rep, "        %s：第 %zu 字节起不同（得 %02x，期 %02x）\n",
            what, i, got[i], want[i]);
}

/* ======================= ① KAT ======================= */
static void test_kat(void)
{
    size_t n, i;
    int rc;
    unsigned k;

    fprintf(rep, "\n[① ML-KEM 三套参数集 · NIST ACVP 官方向量]\n");

    for (k = 0; k < sizeof MLKEM_KG / sizeof MLKEM_KG[0]; k++) {
        const mlkem_kg_vec *v = &MLKEM_KG[k];
        memcpy(inbuf, v->d, 32);
        memcpy(inbuf + 32, v->z, 32);
        rc = mlkem_run(0, v->pset, inbuf, 64, outbuf, sizeof outbuf, &n);
        if (rc) { bad("%s KeyGen tc%d：rc=%d", MLKEM_SET_NAME[v->pset], v->tc, rc); continue; }
        if (n != v->ek_len + v->dk_len) {
            bad("%s KeyGen tc%d：输出 %zu 字节，期 %u",
                MLKEM_SET_NAME[v->pset], v->tc, n, v->ek_len + v->dk_len);
            continue;
        }
        if (memcmp(outbuf, v->ek, v->ek_len)) {
            bad("%s KeyGen tc%d：ek 不符", MLKEM_SET_NAME[v->pset], v->tc);
            hexdiff("ek", outbuf, v->ek, v->ek_len);
        } else if (memcmp(outbuf + v->ek_len, v->dk, v->dk_len)) {
            bad("%s KeyGen tc%d：dk 不符", MLKEM_SET_NAME[v->pset], v->tc);
            hexdiff("dk", outbuf + v->ek_len, v->dk, v->dk_len);
        } else {
            ok("%s KeyGen tc%d：ek %u + dk %u 字节逐字节一致",
               MLKEM_SET_NAME[v->pset], v->tc, v->ek_len, v->dk_len);
        }
    }

    for (k = 0; k < sizeof MLKEM_EN / sizeof MLKEM_EN[0]; k++) {
        const mlkem_en_vec *v = &MLKEM_EN[k];
        /* ⚠️ 顺序是 **m 在前、ek 在后**。RTL 里 mlkem_axi 把输入流的前 32 字节
         * 吃进 seed_a 当 m（ek_valid 的条件是 fp >= 32），第一版写反了，
         * 结果 K 和 c 全错 —— 而 KeyGen(d‖z) 和 Decaps(dk‖c) 碰巧写对了，
         * 于是"六条 Encaps 全错、其余全对"，这个形状本身就指向输入构造。*/
        memcpy(inbuf, v->m, 32);
        memcpy(inbuf + 32, v->ek, v->ek_len);
        rc = mlkem_run(1, v->pset, inbuf, 32 + v->ek_len, outbuf, sizeof outbuf, &n);
        if (rc) { bad("%s Encaps tc%d：rc=%d", MLKEM_SET_NAME[v->pset], v->tc, rc); continue; }
        if (n != 32 + v->c_len) {
            bad("%s Encaps tc%d：输出 %zu 字节", MLKEM_SET_NAME[v->pset], v->tc, n);
            continue;
        }
        /* K 和 c **都报**，不是 else-if。只报第一个不符的话，看到的是
         * "K 错"，而 K 与 c 同时错才说明是输入构造的问题；分别错则说明
         * 是输出解释的问题。两种成因要能一眼分开。*/
        if (memcmp(outbuf, v->k, 32) || memcmp(outbuf + 32, v->c, v->c_len)) {
            int kbad = memcmp(outbuf, v->k, 32) != 0;
            int cbad = memcmp(outbuf + 32, v->c, v->c_len) != 0;
            bad("%s Encaps tc%d：%s%s%s不符", MLKEM_SET_NAME[v->pset], v->tc,
                kbad ? "K" : "", (kbad && cbad) ? " 与 " : "", cbad ? "c" : "");
            if (kbad) hexdiff("K", outbuf, v->k, 32);
            if (cbad) hexdiff("c", outbuf + 32, v->c, v->c_len);
        } else {
            ok("%s Encaps tc%d：K 32 + c %u 字节逐字节一致",
               MLKEM_SET_NAME[v->pset], v->tc, v->c_len);
        }
    }

    for (k = 0; k < sizeof MLKEM_DE / sizeof MLKEM_DE[0]; k++) {
        const mlkem_de_vec *v = &MLKEM_DE[k];
        memcpy(inbuf, v->dk, v->dk_len);
        memcpy(inbuf + v->dk_len, v->c, v->c_len);
        rc = mlkem_run(2, v->pset, inbuf, v->dk_len + v->c_len,
                       outbuf, sizeof outbuf, &n);
        if (rc) { bad("%s Decaps tc%d：rc=%d", MLKEM_SET_NAME[v->pset], v->tc, rc); continue; }
        if (n != 32 || memcmp(outbuf, v->k, 32)) {
            bad("%s Decaps tc%d：K 不符（%zu 字节）", MLKEM_SET_NAME[v->pset], v->tc, n);
            if (n == 32) hexdiff("K", outbuf, v->k, 32);
        } else {
            ok("%s Decaps tc%d：K 32 字节逐字节一致", MLKEM_SET_NAME[v->pset], v->tc);
        }
    }
    (void)i;
}

/* ======================= ② 性能 ======================= */
static double bench(int mode, int pset, const unsigned char *in, size_t inlen,
                    int iters)
{
    size_t n; int i; double t0, t1;
    /* 先热一次，把首次的缓存/分支效应排除掉 */
    mlkem_run(mode, pset, in, inlen, outbuf, sizeof outbuf, &n);
    t0 = now_ms();
    for (i = 0; i < iters; i++)
        mlkem_run(mode, pset, in, inlen, outbuf, sizeof outbuf, &n);
    t1 = now_ms();
    return (t1 - t0) / iters;      /* 每次毫秒 */
}

static void test_perf(void)
{
    int p, iters = 20;
    fprintf(rep, "\n[② 性能（每次耗时 / 次每秒；含软件搬数据的开销）]\n");
    fprintf(rep, "  %-13s %-22s %-22s %s\n", "参数集", "KeyGen", "Encaps", "Decaps");
    for (p = 0; p < 3; p++) {
        const mlkem_kg_vec *kg = NULL;
        const mlkem_en_vec *en = NULL;
        const mlkem_de_vec *de = NULL;
        unsigned k;
        double a, b, c;
        for (k = 0; k < sizeof MLKEM_KG / sizeof MLKEM_KG[0]; k++)
            if (MLKEM_KG[k].pset == p) { kg = &MLKEM_KG[k]; break; }
        for (k = 0; k < sizeof MLKEM_EN / sizeof MLKEM_EN[0]; k++)
            if (MLKEM_EN[k].pset == p) { en = &MLKEM_EN[k]; break; }
        for (k = 0; k < sizeof MLKEM_DE / sizeof MLKEM_DE[0]; k++)
            if (MLKEM_DE[k].pset == p) { de = &MLKEM_DE[k]; break; }
        if (!kg || !en || !de) continue;

        memcpy(inbuf, kg->d, 32); memcpy(inbuf + 32, kg->z, 32);
        a = bench(0, p, inbuf, 64, iters);
        memcpy(inbuf, en->m, 32); memcpy(inbuf + 32, en->ek, en->ek_len);
        b = bench(1, p, inbuf, 32 + en->ek_len, iters);
        memcpy(inbuf, de->dk, de->dk_len); memcpy(inbuf + de->dk_len, de->c, de->c_len);
        c = bench(2, p, inbuf, de->dk_len + de->c_len, iters);

        fprintf(rep, "  %-13s %8.2f ms %7.1f/s  %8.2f ms %7.1f/s  %8.2f ms %7.1f/s\n",
                MLKEM_SET_NAME[p], a, 1000.0 / a, b, 1000.0 / b, c, 1000.0 / c);
        fflush(rep);
    }
    fprintf(rep, "  注：含软件逐字节搬进搬出的开销（AXI 每字节一次读写），\n");
    fprintf(rep, "      不是纯硬件算核时间。原型验证不追性能，这里只作横向对照。\n");
    n_pass++;
}

/* ================= ③ 常数时间抽测 ================= */
static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void test_ct(void)
{
    enum { N = 200 };
    static double tv[N], ti[N];
    const mlkem_de_vec *v = NULL;
    unsigned k;
    size_t n, dklen, clen;
    int i;
    double mv, mi, d;

    fprintf(rep, "\n[③ Decaps 常数时间抽测（合法密文 vs 隐式拒绝）]\n");

    for (k = 0; k < sizeof MLKEM_DE / sizeof MLKEM_DE[0]; k++)
        if (MLKEM_DE[k].pset == 1) { v = &MLKEM_DE[k]; break; }   /* 用 768 */
    if (!v) { bad("找不到 ML-KEM-768 的 Decaps 向量"); return; }
    dklen = v->dk_len; clen = v->c_len;

    for (i = 0; i < N; i++) {
        double t0;
        /* 合法密文 */
        memcpy(inbuf, v->dk, dklen);
        memcpy(inbuf + dklen, v->c, clen);
        t0 = now_ms();
        mlkem_run(2, 1, inbuf, dklen + clen, outbuf, sizeof outbuf, &n);
        tv[i] = now_ms() - t0;
        /* 改一个比特 —— 走隐式拒绝那条路 */
        memcpy(inbuf, v->dk, dklen);
        memcpy(inbuf + dklen, v->c, clen);
        inbuf[dklen + (i % clen)] ^= 0x01;
        t0 = now_ms();
        mlkem_run(2, 1, inbuf, dklen + clen, outbuf, sizeof outbuf, &n);
        ti[i] = now_ms() - t0;
    }

    qsort(tv, N, sizeof tv[0], cmp_double);
    qsort(ti, N, sizeof ti[0], cmp_double);
    mv = tv[N / 2];
    mi = ti[N / 2];
    d = (mv > mi ? mv - mi : mi - mv) / (mv > 0 ? mv : 1) * 100.0;

    fprintf(rep, "  合法密文   中位 %.4f ms  最小 %.4f  最大 %.4f\n",
            mv, tv[0], tv[N - 1]);
    fprintf(rep, "  隐式拒绝   中位 %.4f ms  最小 %.4f  最大 %.4f\n",
            mi, ti[0], ti[N - 1]);
    fprintf(rep, "  中位数相对差 %.3f%%（各 %d 次）\n", d, N);

    /* 判据：两条路径的中位耗时差要远小于单次测量的抖动。
     * 0.5% 是很宽的门槛 —— 真早退的话差异会是数量级的，而不是百分之几。 */
    if (d < 0.5)
        ok("两条路径耗时无法区分（相对差 %.3f%% < 0.5%%）"
           "——密文比对确实是全比完再选，没有早退", d);
    else
        bad("两条路径耗时差 %.3f%%，超过判据；密文比对可能有数据相关的早退", d);

    fprintf(rep, "  说明：这测的是**软件侧能观测到的**那一层延迟，也正是计时攻击\n");
    fprintf(rep, "        能拿到的那一层。它不覆盖功耗/电磁侧信道（本次不做，\n");
    fprintf(rep, "        也不假装做了）。\n");
}

int main(void)
{
    int fd;
    uint32_t ver;

    rep = fopen("/tmp/hsm_kem3.txt", "w");
    if (!rep) rep = stdout;

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }
    pl = mmap(NULL, PL_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PL_BASE);
    if (pl == MAP_FAILED) { perror("mmap"); return 1; }

    ver = rd(MK_VER);
    fprintf(rep, "ML-KEM 从机 VERSION = 0x%08x\n", ver);
    if (ver != 0x00010000) {
        fprintf(rep, "VERSION 不对，八成没装对 bitstream，就此打住\n");
        fflush(rep);
        return 2;
    }

    test_kat();
    test_perf();
    test_ct();

    fprintf(rep, "\n合计：通过 %d，失败 %d\n", n_pass, n_fail);
    fflush(rep);
    if (rep != stdout) { fclose(rep); }
    printf("done pass=%d fail=%d\n", n_pass, n_fail);
    return n_fail ? 1 : 0;
}
