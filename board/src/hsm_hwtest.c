// hsm_hwtest —— 在真硬件上跑 PL 里那几个密码核
//
//   aarch64-linux-gnu-gcc -O2 -static -o hsm_hwtest hsm_hwtest.c
//
// 板上没有 gcc 也没有 python3，所以这个程序在构建机上交叉编译、静态链接，
// 拷过去直接跑。输出**只有一个小文本文件** —— 板子的 TX 方向退化，
// 往回拉大文件很慢。
//
// ============================================================================
// 【它证明什么，不证明什么】
// ============================================================================
// 证明：
//   · ML-KEM-512 的 KeyGen / Encaps / Decaps 在真硅上跑出的字节与 **NIST
//     ACVP 向量**逐字节相同（不是与我自己的模型比）；
//   · SM3（GB/T 32905 附录 A）同上；
//   · **D7：普通世界（经 EL3 的通用 PL_RD/PL_WR）够不到密钥仓** ——
//     装载口整槽排除，读写都被 EL3 白名单拒；
//
// ⚠️ D7 之后**本工具不再跑 AES/SM4 的 KAT 与密钥边界扫描**。那两项都要先往
//    密钥仓装一把已知密钥，而装载口现在只认安全世界（TA → PTA → EL3 SiP
//    0x8200ff16）—— 本工具在普通世界，装不进去，**这正是 D7 要的性质**。
//    AES/SM4 的 KAT 改由 sdf_demo 的 [5]/[7] 经 TA 的 CMD_SYMKEY_KAT 跑
//    （公开向量密钥、只进槽 7），见 docs/reference/SYMKEY-TEE-REPORT.zh-CN.md。
//    这里把它们记成 SKIP（**不计入 PASS**）—— 跳过被打成通过比没有报告更危险。
//   · TRNG 出的是真随机（健康检测通过 + 基本统计），
//   · **AxPROT 门控在真硬件上确实生效**：金丝雀那个 SECURE_ONLY=1 的实例，
//     从非安全世界的 Linux 访问必然 DECERR（表现为 SIGBUS）。
//
// 不证明：
//   · **最小熵**。这里只做通过/不通过的健康检测与几项粗统计。真正的最小熵
//     要把原始比特导出来跑 SP 800-90B 的 EntropyAssessment，那是另一件事。
//   · 功耗/电磁侧信道。没做，也不假装做了。
#define _GNU_SOURCE
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "kmod/secmmio_uapi.h"
#include <unistd.h>

#include "kat_vectors.h"

#define PL_BASE   0x80000000UL
#define PL_SPAN   0x00050000UL

#define S_TRNG    0x00000
#define S_VAULT   0x10000
#define S_SYM     0x20000
#define S_MLKEM   0x30000
#define S_CANARY  0x40000

static volatile uint8_t *pl;

/* ---- SIGBUS 安全读 ----
 * 防火墙对窗口外的地址回 DECERR，在 aarch64 上表现为同步外部中止 → SIGBUS。
 * 边界扫描本来就要去读那些地址（"读不到"正是要证明的事），所以这里必须
 * 能从 SIGBUS 里回来，而不是让整个程序死掉 —— 第一版就是这么崩的，
 * 崩在密钥仓窗口 0x40 处，而且因为没 flush，连已经跑过的结果都没留下。 */
static sigjmp_buf busjmp;
static volatile int bus_armed, bus_hit;

static void on_bus(int sig)
{
    (void)sig;
    if (bus_armed) { bus_hit = 1; siglongjmp(busjmp, 1); }
    _exit(135);
}


/* ============================================================================
 * 【transport：直接 mmap  还是  经 EL3】
 * ============================================================================
 * 这一版 bitstream 的四个功能核是 SECURE_ONLY=1，普通世界（本进程，AxPROT[1]
 * 恒为 1）直接访问一律 DECERR。所以核访问要经 /dev/secmmio 转给 EL3 发。
 *
 * 算法与向量一行不改 —— 只把 rd()/wr() 底下换掉。这是有意的：
 * **两种 transport 跑的必须是同一套 KAT**，否则"安全世界能跑通"就成了
 * 一句没法与普通世界那次比较的话。
 *
 * ⚠️ 直接 mmap 模式（-d）**只读不写**。普通世界写一个被拒的地址，DECERR 是
 *    posted 的，在 aarch64 上以 SError 回来，内核只能 panic —— 那不是一次
 *    失败的测试，那是丢一块板子。反证只需要读，读的错误是同步的、接得住。
 */
static int sec_fd = -1;          /* >=0 表示走 EL3 */
static int direct_wr_ok;         /* -D 才置 1，见 main 的用法说明 */



static void sec_open(void)
{
    char st[64] = {0};
    int f;

    /* 保险的判据放在这里而不是内核里：先确认 PL 已 programmed 再发第一笔 SMC。
     * 这一步**不碰 PL 总线** —— 若 PL 不在，碰一下就是 EL3 上的取数错误，
     * 而 BL31 里没有处理它的东西。 */
    f = open("/sys/class/fpga_manager/fpga0/state", O_RDONLY);
    if (f < 0) { perror("fpga_manager state"); exit(2); }
    if (read(f, st, sizeof st - 1) <= 0) { perror("read state"); exit(2); }
    close(f);
    if (!strstr(st, "operating")) {
        fprintf(stderr, "PL 不是 operating（是 \"%s\"），拒绝发 SMC\n", st);
        exit(2);
    }

    sec_fd = open("/dev/secmmio", O_RDWR);
    if (sec_fd < 0) { perror("/dev/secmmio"); exit(2); }
    if (ioctl(sec_fd, SECMMIO_ARM) < 0) { perror("SECMMIO_ARM"); exit(2); }
}

/* 上一次 sec 访问是否被 **EL3 白名单** 拒。
 *
 * 这个标志必须和"被 **PL 防火墙** 拒"分开记：两者都表现为"读不到"，
 * 但含义完全不同 —— 前者是这笔访问根本没上总线，后者是上了总线被门控挡回。
 * 边界扫描要证明的是"密钥字一个都不出现"，两种拒绝都满足；可报告里必须
 * 说清哪一层拒的，否则就成了含糊其辞。 */
static int sec_refused;

static uint32_t sec_rd_raw(unsigned off)
{
    struct secmmio_op op = { .addr = (uint32_t)(PL_BASE + off), .val = 0 };

    sec_refused = 0;
    if (ioctl(sec_fd, SECMMIO_RD, &op) < 0) { sec_refused = 1; return 0; }
    return op.val;
}

/* 普通寄存器访问用这个：合法寄存器被拒说明白名单与程序对不上，是硬错误 */
static uint32_t sec_rd(unsigned off)
{
    uint32_t v = sec_rd_raw(off);

    if (sec_refused) {
        fprintf(stderr, "EL3 读 0x%08lx 被拒（白名单）—— "
                        "合法寄存器不该被拒，检查 patch_atf_secmmio.py 的上界\n",
                PL_BASE + off);
        exit(3);
    }
    return v;
}

static void sec_wr_(unsigned off, uint32_t v)
{
    struct secmmio_op op = { .addr = (uint32_t)(PL_BASE + off), .val = v };

    if (ioctl(sec_fd, SECMMIO_WR, &op) < 0) {
        fprintf(stderr, "EL3 写 0x%08x 被拒（白名单）\n", op.addr);
        exit(3);
    }
}
static inline uint32_t rd(unsigned off)
{
    if (sec_fd >= 0) return sec_rd(off);
    return *(volatile uint32_t *)(pl + off);
}

/* 读一个寄存器；*okp 置 0 表示这次访问被总线拒了（DECERR） */
static int n_ref_el3, n_ref_fw;   /* 两种拒绝分别计数 */

static uint32_t rd_safe(unsigned off, int *okp)
{
    uint32_t v = 0;

    if (sec_fd >= 0) {
        v = sec_rd_raw(off);
        if (okp) *okp = !sec_refused;
        if (sec_refused) n_ref_el3++;
        return sec_refused ? 0 : v;
    }
    bus_hit = 0;
    bus_armed = 1;
    if (sigsetjmp(busjmp, 1) == 0)
        v = rd(off);
    bus_armed = 0;
    if (okp) *okp = !bus_hit;
    if (bus_hit) n_ref_fw++;
    return bus_hit ? 0 : v;
}
static inline void wr(unsigned off, uint32_t v)
{
    if (sec_fd >= 0) { sec_wr_(off, v); return; }
    /* 闸门对准的是"写一个会拒绝我的核"，不是"直接模式"本身：
     * 对 SECURE_ONLY=0 的旧 bitstream，普通世界写是正常且必要的。
     * 所以危险的那条路要显式打开（-D），默认关着。 */
    if (!direct_wr_ok) {
        fprintf(stderr, "拒绝：未加 -D 就在直接模式下写 PL。\n"
                        "若核是 SECURE_ONLY=1，这一笔会 DECERR->SError->panic，丢板子。\n");
        exit(4);
    }
    /* ⚠️ 这一行在 352f5e1 加 -D 闸门时被一并删掉了：此后直接模式（-D）下的
     * **每一笔写都是空操作**，而读照常 —— 于是 KAT 表现为"核没跑完"或结果
     * 不对，像是 RTL 坏了。查 board/logs：那之后的结果都是默认的经 EL3 模式
     * （pay_seckat.sh 等），没有一份 -D 结果受影响。 */
    *(volatile uint32_t *)(pl + off) = v;
}

/* ---- 记分板 ---- */
static int n_pass, n_fail, n_skip;
static FILE *rep;

static void ok(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(rep, "  PASS  ");
    vfprintf(rep, fmt, ap);
    fprintf(rep, "\n");
    fflush(rep);
    va_end(ap);
    n_pass++;
}
static void bad(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(rep, "  FAIL  ");
    vfprintf(rep, fmt, ap);
    fprintf(rep, "\n");
    fflush(rep);
    va_end(ap);
    n_fail++;
}

/* 跳过**不是**通过：单独计数、单独一行，绝不走 ok() */
static void skip(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(rep, "  SKIP  ");
    vfprintf(rep, fmt, ap);
    fprintf(rep, "\n");
    fflush(rep);
    va_end(ap);
    n_skip++;
}

static void hexdiff(const char *what, const unsigned char *got,
                    const unsigned char *want, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (got[i] != want[i])
            break;
    fprintf(rep, "        %s：第 %zu 字节起不同（得 %02x，期 %02x）\n",
            what, i, got[i], want[i]);
}

/* ================= ML-KEM ================= */
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

static int mlkem_run(int mode, int pset, const unsigned char *in, size_t inlen,
                     unsigned char *out, size_t outcap, size_t *outlen)
{
    size_t i;
    long spin;

    wr(MK_MODE, (uint32_t)(mode | (pset << 2)));
    wr(MK_CTRL, MKC_INRST);
    for (i = 0; i < inlen; i++)
        wr(MK_INDATA, in[i]);
    if (rd(MK_INPTR) != inlen)
        return -1;

    wr(MK_CTRL, MKC_START);
    for (spin = 0; spin < 20000000L; spin++)
        if (rd(MK_STATUS) & MKS_DONE)
            break;
    if (!(rd(MK_STATUS) & MKS_DONE))
        return -2;

    *outlen = rd(MK_OUTLEN);
    if (*outlen > outcap)
        return -3;
    for (i = 0; i < *outlen; i++)
        out[i] = (unsigned char)(rd(MK_OUTDAT) & 0xFF);
    return 0;
}

static unsigned char buf[8192], expect[8192];

static void test_mlkem(void)
{
    size_t n;
    int i, rc;
    const unsigned char *ds[N_KG] = { kg0_d, kg1_d, kg2_d };
    const unsigned char *zs[N_KG] = { kg0_z, kg1_z, kg2_z };
    const unsigned char *eks[N_KG] = { kg0_ek, kg1_ek, kg2_ek };
    const unsigned char *dks[N_KG] = { kg0_dk, kg1_dk, kg2_dk };

    fprintf(rep, "\n[ML-KEM-512  NIST ACVP 向量]\n");

    for (i = 0; i < N_KG; i++) {
        memcpy(buf, ds[i], 32);
        memcpy(buf + 32, zs[i], 32);
        rc = mlkem_run(0, 0, buf, 64, expect, sizeof expect, &n);
        if (rc) { bad("KeyGen tc%d：核没跑完（rc=%d）", i, rc); continue; }
        if (n != 800 + 1632) { bad("KeyGen tc%d：输出 %zu 字节", i, n); continue; }
        if (memcmp(expect, eks[i], 800)) {
            bad("KeyGen tc%d：ek 不一致", i);
            hexdiff("ek", expect, eks[i], 800);
        } else if (memcmp(expect + 800, dks[i], 1632)) {
            bad("KeyGen tc%d：dk 不一致", i);
            hexdiff("dk", expect + 800, dks[i], 1632);
        } else {
            ok("KeyGen tc%d：ek 800 + dk 1632 字节逐字节一致", i);
        }
    }

    {
        const unsigned char *eek[N_EN] = { en0_ek, en1_ek, en2_ek };
        const unsigned char *em[N_EN]  = { en0_m,  en1_m,  en2_m  };
        const unsigned char *ec[N_EN]  = { en0_c,  en1_c,  en2_c  };
        const unsigned char *ek_[N_EN] = { en0_k,  en1_k,  en2_k  };
        for (i = 0; i < N_EN; i++) {
            memcpy(buf, em[i], 32);
            memcpy(buf + 32, eek[i], 800);
            rc = mlkem_run(1, 0, buf, 32 + 800, expect, sizeof expect, &n);
            if (rc) { bad("Encaps tc%d：核没跑完（rc=%d）", i, rc); continue; }
            if (n != 32 + 768) { bad("Encaps tc%d：输出 %zu 字节", i, n); continue; }
            if (memcmp(expect, ek_[i], 32)) {
                bad("Encaps tc%d：K 不一致", i);
                hexdiff("K", expect, ek_[i], 32);
            } else if (memcmp(expect + 32, ec[i], 768)) {
                bad("Encaps tc%d：c 不一致", i);
                hexdiff("c", expect + 32, ec[i], 768);
            } else {
                ok("Encaps tc%d：K 32 + c 768 字节逐字节一致", i);
            }
        }
    }

    {
        const unsigned char *ddk[N_DE] = { de0_dk, de1_dk, de2_dk };
        const unsigned char *dc[N_DE]  = { de0_c,  de1_c,  de2_c  };
        const unsigned char *dk_[N_DE] = { de0_k,  de1_k,  de2_k  };
        for (i = 0; i < N_DE; i++) {
            memcpy(buf, ddk[i], 1632);
            memcpy(buf + 1632, dc[i], 768);
            rc = mlkem_run(2, 0, buf, 1632 + 768, expect, sizeof expect, &n);
            if (rc) { bad("Decaps tc%d：核没跑完（rc=%d）", i, rc); continue; }
            if (n != 32) { bad("Decaps tc%d：输出 %zu 字节", i, n); continue; }
            if (memcmp(expect, dk_[i], 32)) {
                bad("Decaps tc%d：K 不一致", i);
                hexdiff("K", expect, dk_[i], 32);
            } else {
                ok("Decaps tc%d：K 32 字节逐字节一致", i);
            }
        }
    }
}

/* ================= 密钥仓 + 对称核 ================= */
#define V_CTRL   (S_VAULT + 0x04)
#define V_SLOT   (S_VAULT + 0x0C)
#define V_KEYIN  (S_VAULT + 0x10)
#define V_SLOTC  (S_VAULT + 0x14)
#define V_VMAP   (S_VAULT + 0x1C)

#define SY_STATUS (S_SYM + 0x08)
#define SY_ALG    (S_SYM + 0x0C)
#define SY_SLOT   (S_SYM + 0x10)
#define SY_CMD    (S_SYM + 0x14)
#define SY_HASHIN (S_SYM + 0x18)
#define SY_DIN0   (S_SYM + 0x20)
#define SY_DOUT0  (S_SYM + 0x30)
#define SY_DIG0   (S_SYM + 0x40)

#define SYS_DONE 2u
#define SYS_KRDY 4u
#define SYS_KVOK 8u

/* D7：vault_load（普通世界逐字写 KEY_IN）已删除 —— 装载口只认安全世界。 */

static void test_sym(void)
{
    unsigned char dig[32];
    long spin;
    int i;

    fprintf(rep, "\n[对称与国密  FIPS 197 / GB-T 32907 / GB-T 32905]\n");

    /* D7：普通世界够不到密钥仓。经 EL3 时这是一条**正面**判据：
     * 通用 PL_RD/PL_WR 对槽 1 读写都拒（白名单整槽排除）。
     * 直接 mmap 模式下不写（写被拒是 SError，丢板子），只记 SKIP。 */
    if (sec_fd >= 0) {
        struct secmmio_op op = { .addr = (uint32_t)(PL_BASE + V_SLOT), .val = 0 };
        int wr_refused = ioctl(sec_fd, SECMMIO_WR, &op) < 0;
        int rd_ok;

        (void)rd_safe(V_VMAP, &rd_ok);
        if (wr_refused && !rd_ok)
            ok("密钥仓（槽 1）：EL3 通用读写都拒 —— 普通世界经 SiP 转一手也装不进对称密钥");
        else
            bad("密钥仓（槽 1）没被 EL3 白名单排除（写%s、读%s）—— "
                "BL31 不是 D7 那一版？", wr_refused ? "被拒" : "放行",
                rd_ok ? "放行" : "被拒");
    } else {
        skip("密钥仓装载门：直接模式不写 PL（D7 的普通世界反证由 hsm_secneg/sdf_demo 做）");
    }
    skip("AES-128/256、SM4 KAT：D7 之后装载只走 TA，改由 sdf_demo [5]/[7] 经 CMD_SYMKEY_KAT 跑");

    /* SM3("abc") */
    wr(SY_ALG, 3);
    wr(SY_CMD, 4);                                  /* HASH_START */
    wr(SY_HASHIN, 'a'); wr(SY_HASHIN, 'b'); wr(SY_HASHIN, 'c');
    wr(SY_CMD, 8);                                  /* HASH_FINAL */
    for (spin = 0; spin < 1000000L; spin++)
        if (rd(SY_STATUS) & SYS_DONE)
            break;
    if (!(rd(SY_STATUS) & SYS_DONE)) {
        bad("SM3：核没跑完");
    } else {
        for (i = 0; i < 8; i++) {
            uint32_t w = rd(SY_DIG0 + 4*i);
            dig[4*i]   = (unsigned char)(w >> 24);
            dig[4*i+1] = (unsigned char)(w >> 16);
            dig[4*i+2] = (unsigned char)(w >> 8);
            dig[4*i+3] = (unsigned char)w;
        }
        if (memcmp(dig, sm3_abc, 32)) { bad("SM3(\"abc\") 不一致"); hexdiff("digest", dig, sm3_abc, 32); }
        else                            ok("SM3(\"abc\")：GB/T 32905 A.1 逐字节一致");
    }
}

/* ================= 边界反证：密钥读不出来 ================= */
static void test_key_boundary(void)
{
    fprintf(rep, "\n[密码边界：密钥能用，但读不出来]\n");
    /* 这一项要先往密钥仓装一把已知密钥再扫两个窗口。D7 之后本工具装不进去，
     * 所以记 SKIP。RTL 层面的同一条性质由 test_key_vault / test_sym_vault 的
     * 逐地址扫描钉着；板上版本待 TA 装载路径上板后，在 sdf_demo [5] 装完
     * 槽 7 之后补一次扫描（见 SYMKEY-TEE-REPORT 的上板清单）。 */
    skip("密钥边界扫描：需要先装已知密钥，D7 之后改到 TA 装载路径上做");
}

/* ================= AxPROT 金丝雀 ================= */
static void test_canary(void)
{
    int okc, okv;
    uint32_t v, r;

    /* ⚠️ 这一条的**期望值随 transport 反号**，不是 bug 是语义：
     *
     *   普通世界（-d/-D）读金丝雀 → 必须被拒   门控挡住了非安全 master
     *   安全世界（默认，经 EL3）读 → 必须读到   门控是按 AxPROT 判的，
     *                                          不是把这个地址一律堵死
     *
     * 同一个地址、两种 transport、相反的期望 —— 这一对比单独任何一条都强：
     * 只测"读不到"的话，一个把地址写死堵掉的实现也能通过。 */
    fprintf(rep, "\n[AxPROT 门控：同一个金丝雀地址，两种 transport 期望相反]\n");

    v = rd_safe(S_CANARY + 0x00, &okc);
    if (sec_fd >= 0) {
        if (okc && v == 0x00010000u)
            ok("安全世界（EL3）读金丝雀 = 0x%08x —— "
               "SECURE_ONLY=1 的核对安全 master 是**开的**，"
               "门控确实按 AxPROT 判", v);
        else
            bad("安全世界读金丝雀失败（ok=%d val=0x%08x）—— "
                "要么 EL3 没发出安全事务，要么门控把地址一律堵死了", okc, v);
    } else {
        if (!okc)
            ok("普通世界读金丝雀被总线拒绝（DECERR/SIGBUS）—— "
               "AxPROT 门控在真硬件上生效");
        else if (v == 0x00010000u)
            bad("普通世界读出了正确的 VERSION=0x%08x —— 门控没生效", v);
        else
            ok("金丝雀读回 0x%08x（不是有效 VERSION），访问被拦下", v);
    }

    /* 对照组：同一个模块的另一个实例。
     * 这一版四个功能核都是 SECURE_ONLY=1，所以对照组的期望也随 transport 变：
     * 安全世界读得到（说明核是活的），普通世界读不到（那是 hsm_secneg 的活）。 */
    /* ⚠️ D7 之后对照组从槽 1（key_vault）换成槽 3（mlkem）：EL3 已把槽 1
     * 整槽排除，经 EL3 读它必然被拒 —— 那是 D7 的正面性质，不再能当"核是活的"
     * 的对照。mlkem 同样是 SECURE_ONLY_FUNCTIONAL、VERSION 同值。 */
    r = rd_safe(sec_fd >= 0 ? S_MLKEM + 0x00 : S_VAULT + 0x00, &okv);
    if (sec_fd >= 0) {
        if (okv && r == 0x00010000u)
            ok("对照：mlkem 经 EL3 读回 VERSION=0x%08x —— "
               "核是活的，上面那条不是\"什么都读不到\"", r);
        else
            bad("对照组经 EL3 读不出 VERSION（ok=%d val=0x%08x）", okv, r);
    } else {
        if (!okv)
            ok("对照：普通世界读 key_vault 也被拒 —— 这一版它也是 SECURE_ONLY=1");
        else
            bad("普通世界读到了 key_vault VERSION=0x%08x —— 门没关上", r);
    }
}

/* ================= TRNG ================= */
#define T_CTRL   (S_TRNG + 0x00)
#define T_STATUS (S_TRNG + 0x04)
#define T_RDATA  (S_TRNG + 0x08)
#define T_HEALTH (S_TRNG + 0x0C)
#define T_STARTUP (S_TRNG + 0x14)
#define T_WORDS  (S_TRNG + 0x1C)

#define TS_READY 1u
#define TS_DVAL  2u
#define TS_ALARM 4u
#define TS_SUDONE 0x20u

static void test_trng(void)
{
    long spin;
    int i, bit;
    uint32_t st;
    unsigned long ones = 0, total = 0;
    uint32_t prev = 0;
    int stuck = 0, ngot = 0;
    uint32_t words[256];

    fprintf(rep, "\n[TRNG]\n");

    wr(T_CTRL, 1);                        /* ENABLE */
    for (spin = 0; spin < 50000000L; spin++) {
        st = rd(T_STATUS);
        if (st & TS_SUDONE)
            break;
    }
    st = rd(T_STATUS);
    if (!(st & TS_SUDONE)) {
        bad("启动健康检测没通过（STATUS=0x%08x，已过 %u 个样本）",
            st, rd(T_STARTUP));
        return;
    }
    ok("启动健康检测通过（SP 800-90B §4.3，%u 个样本）", rd(T_STARTUP));

    if (st & TS_ALARM) {
        bad("健康检测告警：STATUS=0x%08x HEALTH=0x%08x", st, rd(T_HEALTH));
    } else {
        ok("无 RCT / APT 告警");
    }

    for (i = 0; i < 256; i++) {
        for (spin = 0; spin < 5000000L; spin++)
            if (rd(T_STATUS) & TS_DVAL)
                break;
        if (!(rd(T_STATUS) & TS_DVAL))
            break;
        words[ngot] = rd(T_RDATA);
        if (ngot > 0 && words[ngot] == prev)
            stuck++;
        prev = words[ngot];
        ngot++;
    }
    if (ngot < 64) {
        bad("只取到 %d 个随机字（要 256）", ngot);
        return;
    }
    for (i = 0; i < ngot; i++)
        for (bit = 0; bit < 32; bit++) {
            total++;
            if (words[i] & (1u << bit))
                ones++;
        }
    ok("取到 %d 个随机字，交付计数 %u", ngot, rd(T_WORDS));

    /* 粗统计：只是"明显坏了"的筛子，不是熵评估 */
    {
        double frac = (double)ones / (double)total;
        if (frac < 0.45 || frac > 0.55)
            bad("一比特占比 %.4f，明显偏离 0.5", frac);
        else
            ok("一比特占比 %.4f（%lu/%lu），无明显偏置", frac, ones, total);
        if (stuck)
            bad("有 %d 次相邻字完全相同 —— 疑似卡住", stuck);
        else
            ok("256 个字里没有相邻重复");
    }
    fprintf(rep, "        注：以上**不是**最小熵评估。真实最小熵要导出原始比特\n");
    fprintf(rep, "        跑 SP 800-90B 的 EntropyAssessment，见 ring_osc.v 的说明。\n");
}

/* 用法：
 *   hsm_hwtest            默认走 EL3（/dev/secmmio）—— 这一版核是 SECURE_ONLY=1
 *   hsm_hwtest -d         直接 mmap，**只读** —— 反证用（读被拒是同步 SIGBUS，接得住）
 *   hsm_hwtest -D         直接 mmap，**可写** —— 只对 SECURE_ONLY=0 的旧 bitstream 用。
 *                    对 SECURE_ONLY=1 的核写一笔就是 DECERR->SError->内核 panic。
 */
int main(int argc, char **argv)
{
    int fd;
    uint32_t v;
    int direct = (argc > 1 && (strcmp(argv[1], "-d") == 0 ||
                              strcmp(argv[1], "-D") == 0));
    direct_wr_ok = (argc > 1 && strcmp(argv[1], "-D") == 0);

    struct sigaction sa;

    rep = fopen("/tmp/hsm_hwtest.txt", "w");
    if (!rep)
        rep = stdout;

    /* 全程装着：边界扫描要故意去读被防火墙拒的地址 */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_bus;
    sigaction(SIGBUS, &sa, NULL);

    if (direct) {
        fd = open("/dev/mem", O_RDWR | O_SYNC);
        if (fd < 0) { perror("open /dev/mem"); return 1; }
        pl = mmap(NULL, PL_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PL_BASE);
        if (pl == MAP_FAILED) { perror("mmap"); return 1; }
    } else {
        sec_open();
    }

    fprintf(rep, "PL 密码机硬件自测  @ 0x%08lx\n", PL_BASE);
    v = rd(S_MLKEM + 0x00);
    fprintf(rep, "mlkem_axi VERSION = 0x%08x\n", v);
    if (v != 0x00010000u) {
        fprintf(rep, "\n*** 读不到 mlkem_axi 的 VERSION —— bitstream 没载入，"
                     "或地址不对。后面的结果都不作数。***\n");
        fclose(rep);
        return 2;
    }

    test_mlkem();
    test_sym();
    test_key_boundary();
    test_canary();
    test_trng();

    fprintf(rep, "\n================================\n");
    fprintf(rep, "通过 %d，失败 %d，跳过 %d（跳过不计入通过）\n", n_pass, n_fail, n_skip);
    fclose(rep);
    return n_fail ? 1 : 0;
}
