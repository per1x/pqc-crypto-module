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
//   · AES-128/256（FIPS 197 附录 C）、SM4（GB/T 32907 附录 A）、
//     SM3（GB/T 32905 附录 A）同上；
//   · 密钥仓装进去的密钥能被对称核用出正确密文，而**两个从机的整个地址
//     窗口里读不到那把密钥的任何一个字**；
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
#include <string.h>
#include <sys/mman.h>
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

static inline uint32_t rd(unsigned off)
{
    return *(volatile uint32_t *)(pl + off);
}

/* 读一个寄存器；*okp 置 0 表示这次访问被总线拒了（DECERR） */
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
static inline void wr(unsigned off, uint32_t v)
{
    *(volatile uint32_t *)(pl + off) = v;
}

/* ---- 记分板 ---- */
static int n_pass, n_fail;
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

static void vault_load(int slot, const unsigned char *key, int len)
{
    int i;
    unsigned char k32[32];
    memset(k32, 0, sizeof k32);
    memcpy(k32, key, len);
    wr(V_SLOT, slot);
    wr(V_SLOTC, 1);                       /* BEGIN */
    for (i = 0; i < 8; i++)
        wr(V_KEYIN, ((uint32_t)k32[4*i] << 24) | ((uint32_t)k32[4*i+1] << 16)
                    | ((uint32_t)k32[4*i+2] << 8) | k32[4*i+3]);
    wr(V_SLOTC, 2);                       /* COMMIT */
}

static int sym_block(int alg, int slot, const unsigned char *in,
                     unsigned char *out, int decrypt)
{
    int i;
    long spin;
    wr(SY_ALG, (uint32_t)(alg | (decrypt ? 4 : 0)));
    wr(SY_SLOT, slot);
    if (!(rd(SY_STATUS) & SYS_KVOK))
        return -1;
    wr(SY_CMD, 1);                        /* LOAD_KEY */
    for (spin = 0; spin < 100000L; spin++)
        if (rd(SY_STATUS) & SYS_KRDY)
            break;
    if (!(rd(SY_STATUS) & SYS_KRDY))
        return -2;
    for (i = 0; i < 4; i++)
        wr(SY_DIN0 + 4*i, ((uint32_t)in[4*i] << 24) | ((uint32_t)in[4*i+1] << 16)
                          | ((uint32_t)in[4*i+2] << 8) | in[4*i+3]);
    wr(SY_CMD, 2);                        /* BLOCK */
    for (spin = 0; spin < 100000L; spin++)
        if (rd(SY_STATUS) & SYS_DONE)
            break;
    if (!(rd(SY_STATUS) & SYS_DONE))
        return -3;
    for (i = 0; i < 4; i++) {
        uint32_t w = rd(SY_DOUT0 + 4*i);
        out[4*i]   = (unsigned char)(w >> 24);
        out[4*i+1] = (unsigned char)(w >> 16);
        out[4*i+2] = (unsigned char)(w >> 8);
        out[4*i+3] = (unsigned char)w;
    }
    return 0;
}

static void test_sym(void)
{
    unsigned char out[16], dig[32];
    long spin;
    int i;

    fprintf(rep, "\n[对称与国密  FIPS 197 / GB-T 32907 / GB-T 32905]\n");

    vault_load(0, aes128_key, 16);
    vault_load(1, aes256_key, 32);
    vault_load(2, sm4_key, 16);
    if ((rd(V_VMAP) & 0x7) != 0x7) {
        bad("密钥仓：槽 0/1/2 没都装上（VALID_MAP=0x%02x）", rd(V_VMAP));
        return;
    }
    ok("密钥仓：三把密钥装载并 COMMIT 成功");

    if (sym_block(0, 0, aes_pt, out, 0))            bad("AES-128：核没跑完");
    else if (memcmp(out, aes128_ct, 16))            { bad("AES-128 密文不一致"); hexdiff("ct", out, aes128_ct, 16); }
    else                                            ok("AES-128 加密：FIPS 197 C.1 逐字节一致");

    if (sym_block(0, 0, aes128_ct, out, 1))         bad("AES-128 解密：核没跑完");
    else if (memcmp(out, aes_pt, 16))               bad("AES-128 解密不一致");
    else                                            ok("AES-128 解密：回到原文");

    if (sym_block(1, 1, aes_pt, out, 0))            bad("AES-256：核没跑完");
    else if (memcmp(out, aes256_ct, 16))            { bad("AES-256 密文不一致"); hexdiff("ct", out, aes256_ct, 16); }
    else                                            ok("AES-256 加密：FIPS 197 C.3 逐字节一致");

    if (sym_block(2, 2, sm4_pt, out, 0))            bad("SM4：核没跑完");
    else if (memcmp(out, sm4_ct, 16))               { bad("SM4 密文不一致"); hexdiff("ct", out, sm4_ct, 16); }
    else                                            ok("SM4 加密：GB/T 32907 A.1 逐字节一致");

    if (sym_block(2, 2, sm4_ct, out, 1))            bad("SM4 解密：核没跑完");
    else if (memcmp(out, sm4_pt, 16))               bad("SM4 解密不一致");
    else                                            ok("SM4 解密：回到原文");

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
    unsigned kw[4];
    unsigned off;
    int i, leaked = 0;
    unsigned char out[16];

    fprintf(rep, "\n[密码边界：密钥能用，但读不出来]\n");

    /* 先确认这把密钥确实在被用（密文对得上 FIPS 197） */
    if (sym_block(0, 0, aes_pt, out, 0) || memcmp(out, aes128_ct, 16)) {
        bad("边界反证的前提不成立：AES 没算出正确密文");
        return;
    }

    for (i = 0; i < 4; i++)
        kw[i] = ((uint32_t)aes128_key[4*i] << 24)
              | ((uint32_t)aes128_key[4*i+1] << 16)
              | ((uint32_t)aes128_key[4*i+2] << 8) | aes128_key[4*i+3];

    /* 把密钥仓与对称核两个窗口整个扫一遍。
     * 窗口外的地址会被防火墙拒（DECERR → SIGBUS），rd_safe 接住并记成
     * "读不到" —— 读不到当然也就漏不出去，所以那些地址算通过。 */
    {
        int okv, oks, nread = 0, nrefused = 0;
        for (off = 0; off < 0x100; off += 4) {
            uint32_t a = rd_safe(S_VAULT + off, &okv);
            uint32_t b = rd_safe(S_SYM + off, &oks);
            nread += okv + oks;
            nrefused += (!okv) + (!oks);
            for (i = 0; i < 4; i++) {
                if (okv && a == kw[i]) {
                    bad("密钥第 %d 字出现在密钥仓 +0x%02x", i, off); leaked = 1;
                }
                if (oks && b == kw[i]) {
                    bad("密钥第 %d 字出现在对称核 +0x%02x", i, off); leaked = 1;
                }
            }
        }
        if (!leaked)
            ok("扫完两个从机各 256 字节（%d 个读得到、%d 个被防火墙拒）："
               "密钥的 4 个字一个都没出现，而密文正确", nread, nrefused);
    }
}

/* ================= AxPROT 金丝雀 ================= */
static void test_canary(void)
{
    int okc, okv;
    uint32_t v, r;

    fprintf(rep, "\n[AxPROT 门控：非安全 master 访问 SECURE_ONLY=1 的实例]\n");

    v = rd_safe(S_CANARY + 0x00, &okc);
    if (!okc)
        ok("金丝雀读被总线拒绝（DECERR/SIGBUS）—— AxPROT 门控在真硬件上生效");
    else if (v == 0x00010000u)
        bad("金丝雀读出了正确的 VERSION=0x%08x —— AxPROT 门控没生效", v);
    else
        ok("金丝雀读回 0x%08x（不是有效 VERSION），访问被拦下", v);

    /* 对照组：同一个模块、SECURE_ONLY=0 的实例必须读得出 VERSION。
     * 少了这一条，一个"什么都拒绝"的实现也能通过上面那条。 */
    r = rd_safe(S_VAULT + 0x00, &okv);
    if (okv && r == 0x00010000u)
        ok("对照：SECURE_ONLY=0 的同一模块读回 VERSION=0x%08x", r);
    else
        bad("对照组读不出 VERSION（ok=%d val=0x%08x）—— 说明不了门控", okv, r);
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

int main(void)
{
    int fd;
    uint32_t v;

    struct sigaction sa;

    rep = fopen("/tmp/hsm_hwtest.txt", "w");
    if (!rep)
        rep = stdout;

    /* 全程装着：边界扫描要故意去读被防火墙拒的地址 */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_bus;
    sigaction(SIGBUS, &sa, NULL);

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }
    pl = mmap(NULL, PL_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PL_BASE);
    if (pl == MAP_FAILED) { perror("mmap"); return 1; }

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
    fprintf(rep, "通过 %d，失败 %d\n", n_pass, n_fail);
    fclose(rep);
    return n_fail ? 1 : 0;
}
