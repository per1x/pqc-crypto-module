// d7bench —— D7 上板的最后两条：密钥边界扫描（板上版）+ 吞吐
//
//   编：与 sdf_demo 同一套（链 libsdfe + 动态 OpenSSL 1.1，RUNPATH 指 hsm/lib）
//   跑：演示形态（SECURE_ONLY=0），daemon 在、没有别的客户端
//
// ① 边界：经 TA 把 FIPS-197 C.3 的 AES-256 公开密钥装进 KAT 槽 7，先用 C.3 向量
//    证明"密钥确实在核里"，然后**在同一会话里**经 /dev/mem 逐字读完 key_vault
//    （槽 1）与 sym_axi（槽 2）的整个 256 字节窗口，断言 8 个密钥字一个都不出现。
//    只读不写；演示位流下窗口外是 RAZ（读回 0），不会总线错误。
// ② 吞吐：SYMKEY_LOAD 的单次延迟（11 次 PTA→SMC）；AES-256-CBC 16 KB × N 的速率
//    （分组路径 D7 没动，只是记一笔绝对值）。
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "../../service/sdfe.h"

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(void)
{
    static const uint8_t pt[16] = { 0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                                    0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff };
    static const uint8_t ct[16] = { 0x8e,0xa2,0xb7,0xca,0x51,0x67,0x45,0xbf,
                                    0xea,0xfc,0x49,0x90,0x4b,0x49,0x60,0x89 };
    static uint8_t buf[16368], out[16368]   /* MAXPAY 16384 减去 16 字节 IV */, iv[16], blob[SDFE_SYMKEY_BLOB_MAX];
    SDFE_HANDLE dev, ses;
    uint8_t got[16];
    uint32_t kw[8], blen;
    int rv, i, fd, fail = 0, leaked = 0, nread = 0;
    volatile uint8_t *pl;
    double t0, t1;

    if (SDFE_OpenDevice(&dev) || SDFE_OpenSession(dev, &ses)) {
        printf("FAIL  连不上 daemon\n"); return 2;
    }

    /* ---- ① 边界 ---- */
    if ((rv = SDFE_LoadKatKey(ses, SDFE_KAT_AES256_FIPS197)) != SDR_OK) {
        printf("FAIL  装 KAT 密钥：%s\n", SDFE_StrError(rv)); return 2;
    }
    if (SDFE_Encrypt(ses, SDFE_ALG_AES256, SDFE_SYM_KAT_SLOT, pt, got) ||
        memcmp(got, ct, 16)) {
        printf("FAIL  边界扫描的前提不成立：AES-256 没对上 FIPS-197 C.3\n"); return 2;
    }
    printf("PASS  槽 7 的 AES-256 对上 FIPS-197 C.3 —— 密钥确实在核里\n");
    for (i = 0; i < 8; i++)
        kw[i] = (uint32_t)(4*i) << 24 | (uint32_t)(4*i+1) << 16
              | (uint32_t)(4*i+2) << 8 | (uint32_t)(4*i+3);
    fd = open("/dev/mem", O_RDONLY | O_SYNC);
    pl = fd < 0 ? MAP_FAILED : mmap(NULL, 0x30000, PROT_READ, MAP_SHARED, fd, 0x80000000UL);
    if (pl == MAP_FAILED) { printf("FAIL  mmap /dev/mem\n"); return 2; }
    if (*(volatile uint32_t *)(pl + 0x10000) != 0x00010000u) {
        printf("SKIP  key_vault VERSION 读不到 —— 不是演示位流，边界扫描不做\n");
    } else {
        unsigned base, off;
        for (base = 0x10000; base <= 0x20000; base += 0x10000)
            for (off = 0; off < 0x100; off += 4) {
                uint32_t v = *(volatile uint32_t *)(pl + base + off);
                nread++;
                for (i = 0; i < 8; i++)
                    if (v == kw[i]) {
                        printf("FAIL  密钥第 %d 字出现在 0x%08x\n", i,
                               0x80000000u + base + off);
                        leaked = 1;
                    }
            }
        if (leaked) fail = 1;
        else printf("PASS  key_vault + sym_axi 两个窗口 %d 个字逐个读完，"
                    "密钥的 8 个字一个都没出现（而密文正确）\n", nread);
    }

    /* ---- ② 吞吐 ---- */
    blen = sizeof blob;
    if ((rv = SDFE_GenerateKey(ses, 3, 32, blob, &blen)) != SDR_OK) {
        printf("FAIL  GenerateKey：%s\n", SDFE_StrError(rv)); return 2;
    }
    t0 = now();
    for (i = 0; i < 50; i++)
        if ((rv = SDFE_LoadKey(ses, 3, blob, blen)) != SDR_OK) {
            printf("FAIL  LoadKey：%s\n", SDFE_StrError(rv)); return 2;
        }
    t1 = now();
    printf("INFO  SYMKEY_LOAD 单次 %.2f ms（50 次平均，含 socket 往返）\n",
           (t1 - t0) * 1000 / 50);
    memset(buf, 0x5a, sizeof buf);
    t0 = now();
    for (i = 0; i < 16; i++)
        if ((rv = SDFE_EncryptMode(ses, SDFE_ALG_AES256, SDFE_MODE_CBC, 3, iv,
                                   buf, sizeof buf, out)) != SDR_OK) {
            printf("FAIL  EncryptMode：%s\n", SDFE_StrError(rv)); return 2;
        }
    t1 = now();
    printf("INFO  AES-256-CBC %u KB 用时 %.3f s → %.2f MB/s（分组路径 D7 未改）\n", (unsigned)(16 * sizeof buf / 1024),
           t1 - t0, 16.0 * sizeof buf / 1048576.0 / (t1 - t0));

    SDFE_CloseSession(ses);
    SDFE_CloseDevice(dev);
    printf("%s\n", fail ? "d7bench: 有失败" : "d7bench: 通过");
    return fail;
}
