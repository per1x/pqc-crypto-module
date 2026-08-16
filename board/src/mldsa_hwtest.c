// mldsa_hwtest —— 在真硅上跑 PL 里的 ML-DSA 核，逐字节对 NIST ACVP 向量
//
//   aarch64-linux-gnu-gcc -O2 -Wall -Wextra -static -o mldsa_hwtest mldsa_hwtest.c
//
// 板上没有 gcc 也没有 python3，所以这个程序在构建机上交叉编译、静态链接，
// 拷过去直接跑；向量编译期嵌进来（kat_mldsa.h，由 tools/mldsa_kat_to_c.py 生成）。
//
// ============================================================================
// 【它要证明什么】
// ============================================================================
// ML-DSA 的 RTL 已经全部验过（十二格 132 条对 ACVP，Icarus 与 Verilator 各一遍；
// 整片综合 WNS +1.547 MET），但**一次都没在硬件上跑过**。这个程序就是要让
// "ML-DSA 真在 FPGA 上算对 ACVP 向量"这件事第一次有证据：
//
//   ① 三个 op × 三个参数集，逐字节对 ACVP 官方向量；
//   ② Verify 的 **pass 与 fail 两种判定都验** —— 只验通过的一半，一个恒返回
//      "通过"的实现照样全过；
//   ③ **片内金库**：SK_TO_SLOT 生成之后 sk 不出现在 OUT_DATA（OUT_LEN 只到 pk
//      的长度，而且把读游标 seek 过去也一个字节都拿不到），再用 SK_FROM_SLOT
//      按槽签名，签出来与"自送 sk 签"逐字节相同 —— 这一条是整个项目
//      "私钥不出硬件"在板上的证据；
//   ④ **运行时切参数集**：同一次运行里 44 → 65 → 87 → 65 → 44，不重装位流。
//
// 顺带量一件事：每条运算都报**实测耗时**。service/pqchsm_fpgad.c 里的
// mldsa_timeout_ms() 现在给的是一组"保证硬件不吭声时我们会停下来"的上限，
// 注释里写着"真实耗时等从机落地后实测"—— 这个程序就是那次实测。
//
// ============================================================================
// 【它不证明什么】
// ============================================================================
// · **不证明送检形态能用**。送检形态是 SECURE_ONLY=1，每一笔核访问都要经 BL31
//   的 SiP 白名单，而那份白名单里没有 0x8006_0000。这个程序跑的是**开发形态
//   位流**（PQC_DEV_OPEN=1 → SECURE_ONLY=0），普通世界经 /dev/mem 直连寄存器。
//   算法与向量与送检形态一模一样，被绕开的只有访问控制那一层。
// · 不证明侧信道、不证明时序余量在温度/电压角上仍然成立。
//
// ============================================================================
// 【⚠️ 为什么第一件事是读 VERSION，而且读不对就必须退出】
// ============================================================================
// 这块板上写一个会被拒的地址是**丢板子级**的错误：DECERR 是 posted 的，在
// aarch64 上以 SError 回来，内核只能 panic，而这块板的 reboot 不生效、只能断电
// （见 board/scripts/ 里那几个死人开关）。读的错误反过来是同步的，接得住。
//
// 所以本程序的第一笔访问永远是**读** VERSION：
//   · 读回 0x00010000 → 这个地址上有核、而且防火墙放我们过 → 后面写也安全
//     （AXI 防火墙的 ALLOW_READ/ALLOW_WRITE 同受 SECURE_ONLY 一个开关管，
//      非安全读过得去，非安全写就过得去）；
//   · 读回 0 或别的值 → 位流不对/不是开发形态/槽里不是这个核 → **立刻退出**，
//     一个字节都不写。继续跑只会得到一屏没有意义的失败。
//   · 读的时候直接 SIGBUS → 地址根本没被译码，同样立刻退出。
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "kat_mldsa.h"

/* ---- 寄存器（槽内偏移，与 hardware/rtl/bus/mldsa_axi.v 的 A_* 一致）---- */
#define MD_VERSION 0x00
#define MD_CTRL    0x04
#define MD_MODE    0x08
#define MD_STATUS  0x0C
#define MD_INDATA  0x10
#define MD_INPTR   0x14
#define MD_OUTDATA 0x18
#define MD_OUTPTR  0x1C
#define MD_OUTLEN  0x20
#define MD_MSGLEN  0x24
#define MD_CTXLEN  0x28
#define MD_KEYSTAT 0x2C
#define MD_VIOL    0x30

#define CORE_VERSION 0x00010000u

#define C_START   (1u << 0)
#define C_CLEAR   (1u << 1)
#define C_ZEROIZE (1u << 2)
/* CTRL[4] 是 SK_LOCK。**本程序绝不写它** —— 它是一次性闩锁，置上之后
 * KeyGen 再也不把 sk 交出来，而 CTRL 里没有清它的位、ZEROIZE 也不清，
 * 只有重新装载位流能解开。而 ACVP 的 KeyGen 向量要核对 sk，闩上就等于
 * 把这个程序自己的第①条用例永久废掉。闩锁该由交付流程去置，不是由测试程序。 */

#define ST_BUSY   (1u << 0)
#define ST_DONE   (1u << 1)
#define ST_VOK    (1u << 2)
#define ST_PARERR (1u << 3)
#define ST_LENERR (1u << 4)
#define ST_TAMPER (1u << 5)
#define ST_WIPING (1u << 6)

#define OP_KEYGEN 0u
#define OP_SIGN   1u
#define OP_VERIFY 2u
#define M_SK_TO_SLOT   (1u << 4)
#define M_SK_FROM_SLOT (1u << 5)
#define M_SLOT(s)      (((unsigned)(s) & 15u) << 6)

#define KS_LOCK   (1u << 8)      /* KEYSTAT[8] */

/* FIPS 204 表 2。pk/sk 的长度直接取自向量表（生成脚本已核对过），这里只留
 * σ 的 —— 金库那条用例要签一条 ACVP 给不出期望值的消息，长度得自己算。 */
static const unsigned SIG_LEN[3] = { 2420, 3309, 4627 };

#define IN_MAX   16384           /* 最大输入流：Verify 87 = pk+sig+ctx+msg */
#define OUT_MAX   8192           /* 最大输出：KeyGen 87 = pk+sk = 7488 */

static unsigned long base_addr = 0x80060000UL;
static volatile unsigned char *slot_map;   /* mmap 到的槽首地址 */
static FILE *rep;

/* ---- SIGBUS：只用来把"地址没被译码"变成一句人话，而不是一个 core dump ----
 * 读被防火墙/互连拒了在 aarch64 上是同步外部中止 → SIGBUS，接得住。
 * 探测那一笔要能从里面回来；探测通过之后再 SIGBUS 就是真出事了，直接退。 */
static sigjmp_buf busjmp;
static volatile sig_atomic_t bus_armed, bus_hit;

static void on_bus(int sig)
{
    static const char m[] =
        "\n*** SIGBUS：PL 总线拒绝了一次访问。位流没装/不是开发形态/地址不对。\n";

    (void)sig;
    if (bus_armed) {
        bus_hit = 1;
        siglongjmp(busjmp, 1);
    }
    /* 信号处理里只能用 async-signal-safe 的东西，所以是 write 不是 fprintf */
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

/* ---- 记分板 ---- */
static int n_pass, n_fail;

static void res(int good, int pset, const char *op, int tc, const char *fmt, ...)
{
    va_list ap;

    fprintf(rep, "%s %-10s %-7s tcId=%-4d ", good ? "✅" : "❌",
            MLDSA_SET_NAME[pset], op, tc);
    va_start(ap, fmt);
    vfprintf(rep, fmt, ap);
    va_end(ap);
    fputc('\n', rep);
    fflush(rep);
    if (good)
        n_pass++;
    else
        n_fail++;
}

/* 逐字节比较；相同返回 NULL，不同把"第一个不同在哪"写进 buf。
 *
 * "整段不一致"这句话在这里几乎没有用：段边界差一个字节和整段错位是完全不同
 * 的两个 bug，而下标一报就分得开（RTL 那边正是靠这个把"pk 最后一个字节"
 * 那条定位出来的，见 mldsa_axi.v 里 out_addr 那段）。 */
static const char *cmp_bytes(const unsigned char *got, const unsigned char *want,
                             unsigned n, char *buf, size_t bufsz)
{
    unsigned i, first = 0, ndiff = 0;

    for (i = 0; i < n; i++) {
        if (got[i] != want[i]) {
            if (!ndiff)
                first = i;
            ndiff++;
        }
    }
    if (!ndiff)
        return NULL;
    snprintf(buf, bufsz, "%u/%u 字节不一致，第一个在 [%u]（读回 0x%02x，"
             "应当是 0x%02x）", ndiff, n, first, got[first], want[first]);
    return buf;
}

/* ============================================================================
 * 【⚠️ 写之前必须先确认从机是 IDLE —— 这一条是丢板子/不丢板子的分界】
 * ============================================================================
 * mldsa_axi 对下面这几种写**明确回 SLVERR**（见 mldsa_axi.v 的 wr_busy_reject
 * 与 f_bresp 那段）：
 *   · 运行途中写 MODE / MSG_LEN / CTX_LEN / IN_DATA；
 *   · 擦除期间（WIPING）写**任何**寄存器，CTRL 也不例外；
 *   · 往 IN_PTR 写非零、输入缓冲写满之后再写 IN_DATA。
 * RTL 那么设计是对的 —— 静默丢弃比报错危险得多。但在**这一侧**要付代价：
 * 写的错误响应在 aarch64 上是 SError，接不住，内核只能 panic，而这块板
 * reboot 不生效、只能断电。
 *
 * 于是有一条真实的路会撞上去：某一次运算超时（BUSY 一直不落）之后，测试循环
 * 若无其事地去跑下一条，第一笔 `wr(MODE)` 就落在 state != S_IDLE 上 —— 一次
 * 失败的测试当场升级成一块下线的板子。
 *
 * 所以照 service/pqchsm_fpgad.c 里 hw_fault 那条纪律办：**失败要粘住，并且
 * 立刻停下。** 卡住一次之后，后面每一次调用都直接返回，一个字节都不再写。
 */
static int hw_stuck;

static int wait_idle(long ms)
{
    long deadline = now_ms() + ms;
    uint32_t st;

    for (;;) {
        st = rd(MD_STATUS);
        if (!(st & (ST_BUSY | ST_WIPING)))
            return 0;
        if (now_ms() > deadline) {
            hw_stuck = 1;
            fprintf(rep, "\n*** 从机在 %ld ms 之后仍然 BUSY/WIPING"
                    "（STATUS=0x%08x）——**停止一切写操作**。\n"
                    "    这时候写 MODE 会拿到 SLVERR，而写的错误在 aarch64 上是"
                    " SError，接不住、内核 panic、只能断电。\n"
                    "    宁可不测，不可丢板子。\n", ms, st);
            fflush(rep);
            return -1;
        }
    }
}

/* ---- 等 done 等多久 ----
 * 照抄 service/pqchsm_fpgad.c 的 mldsa_timeout_ms()：那是一组**上限**，不是
 * 耗时估计，唯一的作用是硬件不吭声时我们会停下来。这里不敢收紧 —— Sign 有
 * 拒绝采样循环，期望轮数是个位数但分布有长尾，偶尔十几轮是正常的；拿一个
 * 拍脑袋的紧上限去卡它，症状是"平时好好的、偶尔报一次硬件失败"。
 * 真实耗时由本程序每条打印出来，收紧那个数是**看过实测之后**的事。 */
static long timeout_ms(unsigned op, unsigned pset, unsigned in_len)
{
    long b = (op == OP_SIGN) ? 10000L : 2000L;

    return b * (long)(pset + 1) + (long)in_len / 8;
}

/* ============================================================================
 * 一次完整调用
 * ============================================================================
 * 顺序照 service/pqchsm_fpgad.c 的 mldsa_run()，一步不改（那是与 RTL 对齐过的
 * 一份时序，自创顺序会踩到下面这条）：
 *
 *   ⚠️ **必须先写 MODE 再灌数据。** 软件写的字节落在 engine 输入存储的哪个
 *      偏移取决于 MODE（SK_FROM_SLOT 那趟前面要让开 sk_len 个字节），所以
 *      写 MODE 会把 IN_PTR 清零。先灌后改 MODE 的话，指针归零 → START 处报
 *      欠填（LEN_ERR），而不是安静地按另一份排布解读旧字节。
 *
 * 返回 0 成功；<0 失败并把原因写进 why。verify_ok 只在 op == OP_VERIFY 有意义。
 */
static int md_run(unsigned op, unsigned pset, unsigned mode_extra,
                  const unsigned char *in, unsigned in_len,
                  unsigned msg_len, unsigned ctx_len,
                  unsigned char *out, unsigned out_cap, unsigned *out_len,
                  int *verify_ok, long *elapsed_ms, char *why, size_t whysz)
{
    unsigned i, n, p;
    uint32_t st = 0;
    long t0, deadline;

    if (verify_ok)
        *verify_ok = 0;
    if (out_len)
        *out_len = 0;

    /* ⚠️ 第一笔写之前先确认 IDLE，理由见 wait_idle 上面那段。粘住之后
     *    直接返回，连 STATUS 都不再读。 */
    if (hw_stuck) {
        snprintf(why, whysz, "从机已经卡住，本条跳过（不再碰总线）");
        return -5;
    }
    if (wait_idle(2000)) {
        snprintf(why, whysz, "启动前从机不是 IDLE —— 后面全部跳过");
        return -5;
    }

    wr(MD_MODE, op | (pset << 2) | mode_extra);
    wr(MD_CTRL, C_CLEAR);
    wr(MD_INPTR, 0);          /* ⚠️ 复位写指针是写 IN_PTR=0，不是 CTRL 的某个位 */
    wr(MD_OUTPTR, 0);
    wr(MD_MSGLEN, msg_len);
    wr(MD_CTXLEN, ctx_len);
    for (i = 0; i < in_len; i++)
        wr(MD_INDATA, in[i]);

    p = rd(MD_INPTR);
    if (p != in_len) {
        snprintf(why, whysz, "IN_PTR 对不上：%u，应当是 %u（字节没全灌进去）",
                 p, in_len);
        return -1;
    }

    t0 = now_ms();
    wr(MD_CTRL, C_START);
    deadline = t0 + timeout_ms(op, pset, in_len);
    for (;;) {
        st = rd(MD_STATUS);
        if (st & (ST_PARERR | ST_LENERR)) {
            snprintf(why, whysz, "硬件拒绝了这次 START（STATUS=0x%08x，"
                     "param_err=%d len_err=%d）—— 参数或长度不对，**没有启动**",
                     st, !!(st & ST_PARERR), !!(st & ST_LENERR));
            return -2;
        }
        if (st & ST_DONE)
            break;
        if (now_ms() > deadline) {
            snprintf(why, whysz, "等 done 超时（%ld ms，STATUS=0x%08x）",
                     timeout_ms(op, pset, in_len), st);
            return -3;
        }
    }
    if (elapsed_ms)
        *elapsed_ms = now_ms() - t0;
    if (verify_ok)
        *verify_ok = (st & ST_VOK) ? 1 : 0;

    n = rd(MD_OUTLEN);
    if (out_len)
        *out_len = n;
    if (!out)
        return 0;
    if (n > out_cap) {
        snprintf(why, whysz, "OUT_LEN=%u 超过缓冲 %u", n, out_cap);
        return -4;
    }
    /* 读游标：S_FIN 已经把它清了，这里再显式写一次 —— 读出这一段就变成
     * 自足的，不依赖"上一步刚好把它清过"。RTL 允许任意写 OUT_PTR（它只是个
     * 读游标，读的时候还要被 OUT_LEN 卡一道）。 */
    wr(MD_OUTPTR, 0);
    for (i = 0; i < n; i++)
        out[i] = (unsigned char)(rd(MD_OUTDATA) & 0xFFu);
    return 0;
}

static unsigned char inbuf[IN_MAX], outbuf[OUT_MAX], outbuf2[OUT_MAX];
static const unsigned char RND0[32] = { 0 };   /* 确定性签名：rnd = 0³² */

/* ============================================================================
 * ① KeyGen —— pk‖sk 逐字节对 ACVP
 * ============================================================================
 * 这一条**不走金库**（SK_TO_SLOT=0），因为判据就是 sk 本身。能这么做的前提是
 * sk_lock 没被置上；本程序不置它，理由见 C_ZEROIZE 上面那段。
 */
static void test_keygen(void)
{
    char why[192], diff[160];
    unsigned i, n, want;
    long ms = 0;

    for (i = 0; i < sizeof MLDSA_KG / sizeof MLDSA_KG[0]; i++) {
        const mldsa_kg_vec *v = &MLDSA_KG[i];
        const char *d;

        if (md_run(OP_KEYGEN, (unsigned)v->pset, 0, v->xi, 32, 0, 0,
                   outbuf, sizeof outbuf, &n, NULL, &ms, why, sizeof why)) {
            res(0, v->pset, "KeyGen", v->tc, "%s", why);
            continue;
        }
        want = v->pk_len + v->sk_len;
        if (n != want) {
            res(0, v->pset, "KeyGen", v->tc,
                "OUT_LEN=%u，应当是 pk+sk 的 %u", n, want);
            continue;
        }
        d = cmp_bytes(outbuf, v->pk, v->pk_len, diff, sizeof diff);
        if (d) {
            res(0, v->pset, "KeyGen", v->tc, "pk：%s", d);
            continue;
        }
        d = cmp_bytes(outbuf + v->pk_len, v->sk, v->sk_len, diff, sizeof diff);
        if (d) {
            res(0, v->pset, "KeyGen", v->tc, "sk：%s", d);
            continue;
        }
        res(1, v->pset, "KeyGen", v->tc,
            "pk(%u)+sk(%u) 逐字节对上 ACVP，%ld ms",
            v->pk_len, v->sk_len, ms);
    }
}

/* ============================================================================
 * ② Sign —— σ 逐字节对 ACVP（确定性条目）
 * ============================================================================
 * 输入流是四段拼起来的：sk ‖ rnd(0³²) ‖ ctx ‖ msg。段边界算错一个字节，
 * σ 就完全不同 —— 这也正是这条用例的价值：它同时钉住了排布与算法。
 */
static void test_sign(void)
{
    char why[192], diff[160];
    unsigned i, n, in_len;
    long ms = 0;

    for (i = 0; i < sizeof MLDSA_SG / sizeof MLDSA_SG[0]; i++) {
        const mldsa_sg_vec *v = &MLDSA_SG[i];
        const char *d;

        in_len = 0;
        memcpy(inbuf + in_len, v->sk, v->sk_len);       in_len += v->sk_len;
        memcpy(inbuf + in_len, RND0, sizeof RND0);      in_len += sizeof RND0;
        if (v->ctx_len) {
            memcpy(inbuf + in_len, v->ctx, v->ctx_len); in_len += v->ctx_len;
        }
        memcpy(inbuf + in_len, v->msg, v->msg_len);     in_len += v->msg_len;

        if (md_run(OP_SIGN, (unsigned)v->pset, 0, inbuf, in_len,
                   v->msg_len, v->ctx_len,
                   outbuf, sizeof outbuf, &n, NULL, &ms, why, sizeof why)) {
            res(0, v->pset, "Sign", v->tc, "%s", why);
            continue;
        }
        if (n != v->sig_len) {
            res(0, v->pset, "Sign", v->tc, "OUT_LEN=%u，应当是 σ 的 %u",
                n, v->sig_len);
            continue;
        }
        d = cmp_bytes(outbuf, v->sig, v->sig_len, diff, sizeof diff);
        if (d) {
            res(0, v->pset, "Sign", v->tc, "σ：%s", d);
            continue;
        }
        res(1, v->pset, "Sign", v->tc,
            "σ(%u) 逐字节对上 ACVP（|msg|=%u |ctx|=%u），%ld ms",
            v->sig_len, v->msg_len, v->ctx_len, ms);
    }
}

/* ============================================================================
 * ③ Verify —— pass 与 fail 两种判定都对 ACVP
 * ============================================================================
 * ⚠️ 判据是 **verify_ok 与 ACVP 的 result 相符**，不是"跑完了"。fail 的那些
 *    条目一样会正常 done，只是 STATUS[2] 落着 —— 只挑 pass 的验，一个恒返回
 *    "通过"的实现能把整组过掉。
 */
static void test_verify(void)
{
    char why[192];
    unsigned i, n, in_len;
    int vok;
    long ms = 0;

    for (i = 0; i < sizeof MLDSA_SV / sizeof MLDSA_SV[0]; i++) {
        const mldsa_sv_vec *v = &MLDSA_SV[i];

        in_len = 0;
        memcpy(inbuf + in_len, v->pk, v->pk_len);       in_len += v->pk_len;
        memcpy(inbuf + in_len, v->sig, v->sig_len);     in_len += v->sig_len;
        if (v->ctx_len) {
            memcpy(inbuf + in_len, v->ctx, v->ctx_len); in_len += v->ctx_len;
        }
        memcpy(inbuf + in_len, v->msg, v->msg_len);     in_len += v->msg_len;

        if (md_run(OP_VERIFY, (unsigned)v->pset, 0, inbuf, in_len,
                   v->msg_len, v->ctx_len,
                   NULL, 0, &n, &vok, &ms, why, sizeof why)) {
            res(0, v->pset, "Verify", v->tc, "%s", why);
            continue;
        }
        if (vok != v->expect_ok) {
            res(0, v->pset, "Verify", v->tc,
                "判定为\"%s\"，ACVP 说应当\"%s\"",
                vok ? "通过" : "不通过", v->expect_ok ? "通过" : "不通过");
            continue;
        }
        res(1, v->pset, "Verify", v->tc,
            "判定\"%s\"与 ACVP 一致（|msg|=%u |ctx|=%u），%ld ms",
            vok ? "通过" : "不通过", v->msg_len, v->ctx_len, ms);
    }
}

/* ============================================================================
 * ④ 片内金库 —— "私钥不出硬件"在板上的证据
 * ============================================================================
 * 四半都要成立，缺一半就有一个能糊弄过去的实现：
 *   · **出不来**：OUT_LEN 恰好是 pk 的长度，一个字节不多；而且把读游标 seek
 *     到 sk 那一段也一个字节都拿不到（只查 OUT_LEN 是不够的 —— 读指针要是
 *     没被卡住，软件照样能把 sk 捞出来），越界读还不能推动读游标；
 *   · **进去了**：KEYSTAT 里那个槽被标成有效，而且记着的参数集是对的；
 *   · **还能用**：拿槽里的 sk 去签，σ 与"软件自己送 sk"那一路逐字节相同。
 *     少了这一半，一个"把 sk 直接丢掉"的实现也能过掉上面两条；
 *   · **而且是对的**：那条 σ 还要能被**片内 Verify 用 ACVP 的 pk** 验过。
 *     少了这一半，两条路一起错成同一个样子也能过 —— 相等只证明一致，
 *     不证明正确。
 *
 * ⚠️ 这里的判据为什么不是 ACVP 的 σ：金库里的 sk 只能来自一次真 KeyGen，
 *    而 ACVP 的 siggen 条目自带另一把 sk（两边对不上号）。cocotb 那边用
 *    mldsa_oracle.py 当第三方判据，板上没有 python，所以换成"用 ACVP 的 pk
 *    在片内验一次"——Verify 是与 Sign 不同的一条硬件路径，它点头就说明
 *    这条 σ 与那把公钥是自洽的，而那把公钥是官方值。
 *
 * 自送 sk 那一路用的是 **ACVP 期望的 sk**，不是硬件刚吐出来的 sk：拿硬件的
 * 输出去喂硬件，错了也能自洽。
 */
static void test_vault(void)
{
    static const unsigned char ctx[2] = { 0xAA, 0xBB };
    static const unsigned char msg[7] = { 's', 'i', 'g', 'n', '-', 'm', 'e' };
    char why[192], diff[160];
    unsigned i, n, in_len, slot;
    uint32_t ks, p;
    int vok;
    long ms = 0;

    for (i = 0; i < sizeof MLDSA_KG / sizeof MLDSA_KG[0]; i++) {
        const mldsa_kg_vec *v = &MLDSA_KG[i];
        const char *d;
        unsigned leak = 0, j;

        /* 每个参数集只做一遍（表里每个参数集有两条，取第一条） */
        if (i && MLDSA_KG[i - 1].pset == v->pset)
            continue;
        slot = (unsigned)v->pset;      /* 顺带用上三个不同的槽 */

        /* ---- ① KeyGen → 槽：sk 不出总线 ---- */
        if (md_run(OP_KEYGEN, (unsigned)v->pset,
                   M_SK_TO_SLOT | M_SLOT(slot), v->xi, 32, 0, 0,
                   outbuf, sizeof outbuf, &n, NULL, &ms, why, sizeof why)) {
            res(0, v->pset, "Vault", v->tc, "存槽 KeyGen 失败：%s", why);
            continue;
        }
        if (n != v->pk_len) {
            res(0, v->pset, "Vault", v->tc,
                "存槽后 OUT_LEN=%u，应当**恰好**是 pk 的 %u —— "
                "多出来的就是 sk 出了总线", n, v->pk_len);
            continue;
        }
        d = cmp_bytes(outbuf, v->pk, v->pk_len, diff, sizeof diff);
        if (d) {
            res(0, v->pset, "Vault", v->tc, "存槽这一趟的 pk：%s", d);
            continue;
        }

        /* ---- ② 把读游标推到 sk 那一段：一个字节都不该给 ---- */
        wr(MD_OUTPTR, v->pk_len);
        for (j = 0; j < 64; j++)
            if ((rd(MD_OUTDATA) & 0xFFu) != 0)
                leak++;
        p = rd(MD_OUTPTR);
        if (leak || p != v->pk_len) {
            res(0, v->pset, "Vault", v->tc,
                "读游标 seek 到 pk 之后：%u/64 个字节非零，OUT_PTR=%u"
                "（应当 %u 不动）—— sk 从读游标那条路漏出来了",
                leak, p, v->pk_len);
            continue;
        }

        /* ---- ③ KEYSTAT：槽有效 + 槽里记的参数集对 ---- */
        ks = rd(MD_KEYSTAT);
        if (!(ks & (1u << slot))) {
            res(0, v->pset, "Vault", v->tc,
                "槽 %u 没被标成有效（KEYSTAT=0x%08x）", slot, ks);
            continue;
        }
        if ((int)(((ks >> 16) >> (2 * slot)) & 3u) != v->pset) {
            res(0, v->pset, "Vault", v->tc,
                "槽 %u 里记的参数集不对（KEYSTAT=0x%08x）", slot, ks);
            continue;
        }

        /* ---- ④ 按槽签：软件只送 rnd‖ctx‖msg，一个 sk 字节都不送 ---- */
        in_len = 0;
        memcpy(inbuf + in_len, RND0, sizeof RND0);  in_len += sizeof RND0;
        memcpy(inbuf + in_len, ctx, sizeof ctx);    in_len += sizeof ctx;
        memcpy(inbuf + in_len, msg, sizeof msg);    in_len += sizeof msg;
        if (md_run(OP_SIGN, (unsigned)v->pset,
                   M_SK_FROM_SLOT | M_SLOT(slot), inbuf, in_len,
                   sizeof msg, sizeof ctx,
                   outbuf, sizeof outbuf, &n, NULL, &ms, why, sizeof why)) {
            res(0, v->pset, "Vault", v->tc, "按槽签失败：%s", why);
            continue;
        }
        if (n != SIG_LEN[v->pset]) {
            res(0, v->pset, "Vault", v->tc, "按槽签的 OUT_LEN=%u，应当是 %u",
                n, SIG_LEN[v->pset]);
            continue;
        }

        /* ---- ⑤ 自送 ACVP 的 sk 再签一遍，两条必须逐字节相同 ---- */
        in_len = 0;
        memcpy(inbuf + in_len, v->sk, v->sk_len);   in_len += v->sk_len;
        memcpy(inbuf + in_len, RND0, sizeof RND0);  in_len += sizeof RND0;
        memcpy(inbuf + in_len, ctx, sizeof ctx);    in_len += sizeof ctx;
        memcpy(inbuf + in_len, msg, sizeof msg);    in_len += sizeof msg;
        if (md_run(OP_SIGN, (unsigned)v->pset, 0, inbuf, in_len,
                   sizeof msg, sizeof ctx,
                   outbuf2, sizeof outbuf2, &n, NULL, NULL, why, sizeof why)) {
            res(0, v->pset, "Vault", v->tc, "自送 sk 签失败：%s", why);
            continue;
        }
        d = cmp_bytes(outbuf, outbuf2, SIG_LEN[v->pset], diff, sizeof diff);
        if (d) {
            res(0, v->pset, "Vault", v->tc,
                "按槽签与自送 sk 签不一样（%s）—— 金库里的 sk、或者它进 "
                "engine 的位置是错的", d);
            continue;
        }

        /* ---- ⑥ 用 ACVP 的 pk 在片内验一次：一致 ≠ 正确 ---- */
        in_len = 0;
        memcpy(inbuf + in_len, v->pk, v->pk_len);            in_len += v->pk_len;
        memcpy(inbuf + in_len, outbuf, SIG_LEN[v->pset]);
        in_len += SIG_LEN[v->pset];
        memcpy(inbuf + in_len, ctx, sizeof ctx);             in_len += sizeof ctx;
        memcpy(inbuf + in_len, msg, sizeof msg);             in_len += sizeof msg;
        if (md_run(OP_VERIFY, (unsigned)v->pset, 0, inbuf, in_len,
                   sizeof msg, sizeof ctx,
                   NULL, 0, NULL, &vok, NULL, why, sizeof why)) {
            res(0, v->pset, "Vault", v->tc, "回验失败：%s", why);
            continue;
        }
        if (!vok) {
            res(0, v->pset, "Vault", v->tc,
                "按槽签出来的 σ 用 ACVP 的 pk 验不过 —— 两条路一致但都是错的");
            continue;
        }
        res(1, v->pset, "Vault", v->tc,
            "sk 全程在片内（OUT_LEN=%u 正好 pk，seek 到 sk 段读回全 0）；"
            "槽 %u 按槽签的 σ 与自送 sk 逐字节相同，且用 ACVP 的 pk 验得过",
            v->pk_len, slot);
    }
}

/* ============================================================================
 * ⑤ 运行时切参数集 —— 同一份位流，44 → 65 → 87 → 65 → 44
 * ============================================================================
 * 上面那几组本来就是按参数集顺序跑的，但那是**单调递增**的一遍。这里特意来回
 * 跳一次：如果哪个配置寄存器是"只在第一次生效"或者"降不回去"，单调那一遍看不
 * 出来，来回跳立刻就现形。RTL 那边刚踩过一次同源的坑 —— mldsa_axi 的 pset_ok
 * 曾经拿运行时 pset 去比综合期参数集，bitstream 声称支持三套、实际只有一套能
 * 用（见 mldsa_axi.v 里 pset_ok 那段）。这条用例就是它在板上的对照。
 */
static void test_pset_hop(void)
{
    static const int order[5] = { 0, 1, 2, 1, 0 };
    char why[192], diff[160];
    unsigned k, n;
    int hops = 0, bad = 0;
    long ms = 0;

    for (k = 0; k < sizeof order / sizeof order[0]; k++) {
        const mldsa_kg_vec *v = NULL;
        const char *d;
        unsigned i;

        for (i = 0; i < sizeof MLDSA_KG / sizeof MLDSA_KG[0]; i++)
            if (MLDSA_KG[i].pset == order[k]) {
                v = &MLDSA_KG[i];
                break;
            }
        if (!v)
            continue;

        if (md_run(OP_KEYGEN, (unsigned)v->pset, 0, v->xi, 32, 0, 0,
                   outbuf, sizeof outbuf, &n, NULL, &ms, why, sizeof why)) {
            res(0, v->pset, "PsetSw", v->tc, "第 %u 跳：%s", k + 1, why);
            bad++;
            continue;
        }
        if (n != v->pk_len + v->sk_len) {
            res(0, v->pset, "PsetSw", v->tc,
                "第 %u 跳：OUT_LEN=%u，应当是 %u —— 长度没跟着参数集变",
                k + 1, n, v->pk_len + v->sk_len);
            bad++;
            continue;
        }
        d = cmp_bytes(outbuf, v->pk, v->pk_len, diff, sizeof diff);
        if (d) {
            res(0, v->pset, "PsetSw", v->tc, "第 %u 跳：pk %s", k + 1, d);
            bad++;
            continue;
        }
        res(1, v->pset, "PsetSw", v->tc,
            "第 %u 跳：pk 对上 ACVP，%ld ms", k + 1, ms);
        if (k)
            hops++;
    }
    fprintf(rep, "   —— 同一份位流里切了 %d 次参数集（44→65→87→65→44），"
            "%s\n", hops, bad ? "有失败" : "没有重装位流");
    fflush(rep);
}

/* ============================================================================
 * 探测：读 VERSION，读不对就一个字节都不写
 * ============================================================================ */
static void probe(int readonly)
{
    /* ver 声明成 volatile 是因为它跨 sigsetjmp —— 非 volatile 的局部量在
     * longjmp 回来之后值是未定义的（-Wclobbered 说的正是这个）。 */
    volatile uint32_t ver = 0;
    uint32_t st, ks, viol;

    bus_hit = 0;
    bus_armed = 1;
    if (sigsetjmp(busjmp, 1) == 0)
        ver = rd(MD_VERSION);
    bus_armed = 0;

    if (bus_hit) {
        fprintf(stderr,
                "读 0x%08lx 直接 SIGBUS —— 这个地址根本没被译码。\n"
                "多半是位流没装、或者装的不是含 ML-DSA 的那一版。\n",
                base_addr + MD_VERSION);
        exit(2);
    }
    if (ver != CORE_VERSION) {
        fprintf(stderr,
                "VERSION 读回 0x%08x，应当是 0x%08x —— **不继续**，"
                "一个字节都不写。\n"
                "  · 读回 0：多半是核被防火墙拒了（送检形态 SECURE_ONLY=1，"
                "普通世界访问一律 RAZ/WI 或 DECERR），\n"
                "    要的是开发形态位流（PQC_DEV_OPEN=1 → SECURE_ONLY=0，"
                "产物 zu3eg_hsm_dev.bit）；也可能位流压根没装。\n"
                "  · 读回别的非零值：0x%08lx 上是另一个核或另一个版本。\n"
                "继续跑只会得到一屏没有意义的失败，所以到此为止。\n",
                (unsigned)ver, CORE_VERSION, base_addr);
        exit(2);
    }

    /* VERSION 对上 ⇒ 这个地址上有核、而且防火墙放我们过。AXI 防火墙的
     * ALLOW_READ / ALLOW_WRITE 同受 SECURE_ONLY 一个开关管，所以"非安全读
     * 过得去"就等于"非安全写过得去" —— 后面的写不会变成 SError。
     * 这一句是本程序全部写操作的安全前提，别把 probe 挪到后面去。 */
    st = rd(MD_STATUS);
    ks = rd(MD_KEYSTAT);
    viol = rd(MD_VIOL);

    fprintf(rep, "基址 0x%08lx  VERSION=0x%08x  STATUS=0x%08x"
            "（busy=%d wiping=%d tamper=%d）\n",
            base_addr, (unsigned)ver, st, !!(st & ST_BUSY),
            !!(st & ST_WIPING), !!(st & ST_TAMPER));
    fprintf(rep, "KEYSTAT=0x%08x（槽有效位=0x%02x sk_lock=%d）  "
            "VIOL=0x%08x（写违规=%u 读违规=%u）\n",
            ks, ks & 0xFFu, !!(ks & KS_LOCK), viol,
            viol & 0xFFFFu, viol >> 16);
    fflush(rep);

    if (readonly)
        return;               /* -n：到此为止，一个字节都不写 */

    /* 本程序的**第一笔写**在下面。上一次运行崩在半路、或者从机正在擦除的话，
     * 这一笔就会落在 state != S_IDLE / WIPING 上 → SLVERR → SError → panic。
     * 所以先等它回到 IDLE；等不到就干脆不写。 */
    if (wait_idle(3000)) {
        fprintf(stderr, "从机不是 IDLE，拒绝写任何寄存器。"
                "先把上一次的运算等完，或者重装位流。\n");
        exit(3);
    }

    /* VERSION 是所有核共用的同一个常量（0x00010000），单靠它分不出槽里是
     * 哪个核。MSG_LEN(0x24) 是 mldsa_axi 独有的 RW 寄存器 —— mlkem_axi 在这个
     * 偏移上没有东西（它的 KEYSTAT 在 0x30），读回是 0。写一个值再读回来，
     * 就把"这确实是 ML-DSA 那个从机"钉死了。写完还原成 0。 */
    wr(MD_MSGLEN, 0x1234);
    if (rd(MD_MSGLEN) != 0x1234) {
        fprintf(stderr, "MSG_LEN 写 0x1234 读回 0x%08x —— "
                "0x%08lx 上有东西，但不是 mldsa_axi。停。\n",
                rd(MD_MSGLEN), base_addr);
        exit(2);
    }
    wr(MD_MSGLEN, 0);
}

static void usage(const char *me)
{
    fprintf(stderr,
        "用法：%s [-b 基址] [-o 报告文件] [-n] [-z]\n"
        "  -b HEX   mldsa_axi 的基址（默认 0x80060000）\n"
        "  -o FILE  报告写到文件（默认写 stdout —— 这块板的 TX 方向退化，\n"
        "           整屏往回拉很慢，落文件再取更稳）\n"
        "  -n       **只读探测**：读 VERSION/STATUS/KEYSTAT/VIOL 就退出，\n"
        "           一个字节都不写。第一次上板建议先跑这个。\n"
        "  -z       跑完发一次 ZEROIZE 把 64 KB 金库擦掉（默认不擦）\n", me);
}

int main(int argc, char **argv)
{
    int fd, c, probe_only = 0, do_zero = 0;
    const char *outfile = NULL;
    unsigned long page, off;
    void *m;

    rep = stdout;
    while ((c = getopt(argc, argv, "b:o:nzh")) != -1) {
        switch (c) {
        case 'b': base_addr = strtoul(optarg, NULL, 0); break;
        case 'o': outfile = optarg; break;
        case 'n': probe_only = 1; break;
        case 'z': do_zero = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }
    if (outfile) {
        rep = fopen(outfile, "w");
        if (!rep) { perror(outfile); return 1; }
    }

    signal(SIGBUS, on_bus);

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("/dev/mem");
        fprintf(stderr, "（要 root；而且这条路只在**开发形态位流**下走得通）\n");
        return 2;
    }
    page = (unsigned long)sysconf(_SC_PAGESIZE);
    off  = base_addr & ~(page - 1);
    m = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)off);
    if (m == MAP_FAILED) { perror("mmap"); return 2; }
    slot_map = (volatile unsigned char *)m + (base_addr - off);

    fprintf(rep, "ML-DSA 上板验证 —— 逐字节对 NIST ACVP（FIPS 204）\n");
    fprintf(rep, "经 /dev/mem 直连 mldsa_axi（开发形态位流，不经 daemon、"
            "不经 BL31）\n");
    probe(probe_only);
    if (probe_only) {
        fprintf(rep, "只读探测完成 —— 核在那儿。去掉 -n 跑完整用例。\n");
        return 0;
    }

    fprintf(rep, "\n");
    test_keygen();
    test_sign();
    test_verify();
    test_vault();
    test_pset_hop();

    if (do_zero && !hw_stuck && !wait_idle(3000)) {
        long t0 = now_ms();
        uint32_t ks;

        /* 擦除期间**任何**写都回 SLVERR（CTRL 也不例外），所以这一笔要在
         * IDLE 时发出，发完就只读。 */
        wr(MD_CTRL, C_ZEROIZE);
        while ((rd(MD_STATUS) & ST_WIPING) && now_ms() - t0 < 5000)
            ;
        ks = rd(MD_KEYSTAT);
        fprintf(rep, "\nZEROIZE：%ld ms 擦完，KEYSTAT=0x%08x（槽有效位=0x%02x）"
                "\n", now_ms() - t0, ks, ks & 0xFFu);
        if (ks & 0xFFu)
            fprintf(rep, "⚠️ 擦完之后还有槽被标成有效 —— 擦除没做干净。\n");
    }

    fprintf(rep, "\n总计：%d 通过 / %d 失败\n", n_pass, n_fail);
    if (n_fail)
        fprintf(rep, "有失败 —— **不许说 ML-DSA 已在硬件上验证通过**。\n");
    fflush(rep);
    return n_fail ? 1 : 0;
}
