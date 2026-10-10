// d7probe —— D7 的普通世界反证：演示位流下，普通世界装不进对称密钥
//
//   aarch64-linux-gnu-gcc -O2 -Wall -Wextra -static -o d7probe d7probe.c
//
//   d7probe          反证：普通世界走一遍完整装载序列，必须一笔都不生效
//   d7probe vmap     只读 VALID_MAP（会话结束后应为 0：段末即擦）
//
// ============================================================================
// 【为什么只在演示位流上做】
// ============================================================================
// 送检位流（SECURE_ONLY=1）下整块从机对普通世界 RAZ/WI，装载门根本轮不到它
// 生效 —— 那一形态的反证是 hsm_hwtest 里"EL3 拒槽 1"那条。演示位流
// （SECURE_ONLY=0）防火墙敞开，**只剩装载门**挡着，这里测的正是它。
//
// 闸门：先读 key_vault VERSION。读回 0 说明是送检位流（或 PL 不在），
// 不发任何写。被拒的写本来也是 RAZ/WI（不会 SError），但"只在需要的形态上写"
// 是这块板上的规矩。
//
// 判据（三条都要）：
//   ① 写完 VALID_MAP 不变（槽 6 没被装上）；
//   ② LOAD_REJ 正好涨了本程序发的写数（11）；
//   ③ 收尾发 CTRL.ZEROIZE（普通世界放行：擦除是安全方向），VALID_MAP = 0。
// 若 ① 不成立，多半是位流不是 D7 那一版 —— 照样收尾擦掉，如实报 FAIL。
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define KV_BASE   0x80010000UL
#define KV_VER    0x00
#define KV_CTRL   0x04
#define KV_SEL    0x0C
#define KV_KEYIN  0x10
#define KV_SLOTC  0x14
#define KV_VMAP   0x1C
#define KV_REJ    0x34

static volatile uint8_t *kv;

static uint32_t rd(unsigned o) { return *(volatile uint32_t *)(kv + o); }
static void wr(unsigned o, uint32_t v) { *(volatile uint32_t *)(kv + o) = v; }

int main(int argc, char **argv)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC), i, fail = 0;
    uint32_t vm0, rej0, vm1, rej1, vm2;

    if (fd < 0) { perror("/dev/mem"); return 2; }
    kv = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, KV_BASE);
    if (kv == MAP_FAILED) { perror("mmap"); return 2; }

    if (rd(KV_VER) != 0x00010000u) {
        printf("SKIP  key_vault VERSION 读回 0x%08x —— 不是演示位流（或 PL 不在），不写\n",
               rd(KV_VER));
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "vmap")) {
        vm0 = rd(KV_VMAP);
        printf("%s  VALID_MAP = 0x%02x%s\n", vm0 ? "FAIL" : "PASS", vm0,
               vm0 ? "（会话结束后对称金库没擦干净）" : "（段末即擦）");
        return vm0 ? 1 : 0;
    }

    vm0 = rd(KV_VMAP); rej0 = rd(KV_REJ);
    wr(KV_SEL, 6);
    wr(KV_SLOTC, 1);                         /* BEGIN */
    for (i = 0; i < 8; i++)
        wr(KV_KEYIN, 0xD7D70000u | (uint32_t)i);
    wr(KV_SLOTC, 2);                         /* COMMIT */
    vm1 = rd(KV_VMAP); rej1 = rd(KV_REJ);

    printf("VALID_MAP 0x%02x → 0x%02x，LOAD_REJ %u → %u\n", vm0, vm1, rej0, rej1);
    if (vm1 & 0x40u) {
        printf("FAIL  普通世界把槽 6 装上了 —— 位流不是 D7 那一版，或装载门没生效\n");
        fail = 1;
    } else {
        printf("PASS  普通世界的装载序列一笔都没生效（槽 6 仍空）\n");
    }
    if (rej1 - rej0 != 11u) {
        printf("FAIL  LOAD_REJ 涨了 %u，应为 11（选槽 1 + BEGIN 1 + 8 字 + COMMIT 1）\n",
               rej1 - rej0);
        fail = 1;
    } else {
        printf("PASS  LOAD_REJ 涨了 11 —— 被拒有账\n");
    }

    wr(KV_CTRL, 1);                          /* ZEROIZE：普通世界放行 */
    vm2 = rd(KV_VMAP);
    printf("%s  普通世界 CTRL.ZEROIZE 之后 VALID_MAP = 0x%02x\n",
           vm2 ? "FAIL" : "PASS", vm2);
    if (vm2) fail = 1;
    return fail;
}
