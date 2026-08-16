/* sdfe.h —— SDF 风格的调用接口（代表性子集）
 *
 * "SDFE" = SDF Extended：函数命名沿用 GM/T 0018 的习惯（SDF_ 前缀 + 驼峰），
 * 但 ML-KEM 这一族国内标准接口尚未定，所以用 SDFE_ 前缀标明是扩展、
 * 不冒充标准函数名。理由与取舍见 docs/API.md §3.3。
 *
 * ============================================================================
 * 【这个库不做密码运算】
 * ============================================================================
 * 它只把调用翻译成一条到 pqchsm_fpgad 的请求。真正的运算在 FPGA 里，
 * 请求要经过：
 *
 *   本库 → daemon → /dev/secmmio → EL3 的 SiP → AXI 防火墙 → 密码核
 *
 * 库本身无状态，多个进程可以各自链接它、各自开会话，互不可见对方的句柄。
 *
 * ============================================================================
 * 【私钥怎么处理 —— 两种，必须分开看】
 * ============================================================================
 *   · **对称密钥**（SM4/AES）：装进 PL 的 key_vault，**在 RTL 上就没有通往
 *     总线的读路径**。本库的 SDFE_ImportKey 送进去之后，任何人都读不回来，
 *     包括 daemon 自己。之后只能按槽号使唤它。
 *
 *   · **ML-KEM 私钥 dk**：KeyGen 把 dk 写进 **PL 片内的私钥金库**（一块
 *     只有密码核够得到的 BRAM），对外只交出公钥 ek 和一个槽号；Decaps 按
 *     槽号使唤它。**dk 一个字节都不经过 AXI 总线**，daemon 的内存里也没有它。
 *     交付形态还会置上一道一次性闩锁（CTRL[4]），置上之后连 daemon 自己
 *     都无法让硬件把 dk 送出来 —— 只有重新装载位流才解得开。
 *
 *     ⚠️ 这一条以前不是这样：早先 KeyGen 会把 ek‖dk 一起交出来，dk 存在
 *     daemon 的进程内存里。那时文档一律写"私钥不出**接口**"，不写"不出
 *     **硬件**"，因为后者当时是假的。现在两句都成立了 —— 前提是位流本身
 *     没被换掉（没有硬件信任根，见 docs/SECURITY.md）。
 *
 *     出厂验证是个例外：ACVP 的 KeyGen 向量要核对 dk，所以那条路仍然留着，
 *     由 MODE 的 DK_TO_SLOT 位选择，且闩锁一置就永久关闭。
 *
 *   · **ML-DSA 私钥 sk**：口径与 dk 完全一致 —— KeyGen 把 sk 写进片内金库
 *     （8 个槽），对外只交出 pk 和槽号；SDFE_Sign_MLDSA 按槽号使唤它，
 *     sk 不经过 AXI 总线、不经过 daemon 的内存，交付形态同样有一道一次性闩锁。
 *
 *     2026-08-17 起这一条**有硬件了**：mldsa_axi 在槽 6，三个参数集的
 *     KeyGen/Sign/Verify 经 daemon → /dev/secmmio → EL3 白名单 → 核端到端跑通，
 *     演示形态（SECURE_ONLY=0）与送检形态（SECURE_ONLY=1）各验过一遍。
 *     本库仍然一行密码运算都不做：硬件不可达时它如实失败，**不会**偷偷
 *     回落到软件算一个签名回来 —— 那种回落正是本库文件头第一段拒绝的事。
 */
#ifndef PQCHSM_SDFE_H
#define PQCHSM_SDFE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 返回码沿用 SDF 的风格：0 成功，非 0 是错误码 */
#define SDR_OK            0x00000000
#define SDR_BASE          0x01000000
#define SDR_UNKNOWERR     (SDR_BASE + 0x01)
#define SDR_OPENDEVICE    (SDR_BASE + 0x02)
#define SDR_COMMFAIL      (SDR_BASE + 0x03)
#define SDR_INARGERR      (SDR_BASE + 0x04)
#define SDR_KEYNOTEXIST   (SDR_BASE + 0x05)
#define SDR_HARDFAIL      (SDR_BASE + 0x06)
#define SDR_AUTHFAIL      (SDR_BASE + 0x07)   /* 远程连接口令不对 */
/* 验签不通过。**这是结果，不是故障**，但仍然占一个非 SDR_OK 的码：
 * 若用 SDR_OK + 一个"通过与否"的出参，忘了看那个出参就等于验过了。
 * 让"没检查返回值"这个最常见的疏忽落在安全的一侧。 */
#define SDR_VERIFYFAIL    (SDR_BASE + 0x08)

typedef void *SDFE_HANDLE;

/* ML-KEM 参数集 */
#define SDFE_MLKEM_512   0
#define SDFE_MLKEM_768   1
#define SDFE_MLKEM_1024  2

/* ML-DSA 参数集 */
#define SDFE_MLDSA_44    0
#define SDFE_MLDSA_65    1
#define SDFE_MLDSA_87    2

/* 对称算法（与 sym_axi 的 ALG 字段一致） */
#define SDFE_ALG_AES128  0
#define SDFE_ALG_AES256  1
#define SDFE_ALG_SM4     2

/* ---- 设备与会话 ---- */
int SDFE_OpenDevice(SDFE_HANDLE *phDev);
/* 远程打开：连另一台机器上的密码机（内网演示）。
 * token 是预共享口令，与板上 /media/sd-mmcblk1p2/hsm/hsm_token 一致。
 * 之后所有 SDFE_* 调用与本机完全一样 —— 调用方不需要知道自己是远程的。 */
int SDFE_OpenDeviceRemote(SDFE_HANDLE *phDev, const char *host,
                          int port, const char *token);
int SDFE_CloseDevice(SDFE_HANDLE hDev);
int SDFE_OpenSession(SDFE_HANDLE hDev, SDFE_HANDLE *phSession);
int SDFE_CloseSession(SDFE_HANDLE hSession);
/* 设备信息：版本串，含各核在硅上的 VERSION 寄存器值 */
int SDFE_GetDeviceInfo(SDFE_HANDLE hSession, char *buf, size_t cap);

/* ---- 随机数：来自 PL 的环振噪声源（不是软件 PRNG）---- */
int SDFE_GenerateRandom(SDFE_HANDLE hSession, uint32_t len, uint8_t *out);

/* ---- ML-KEM ----
 * 私钥留在 daemon 侧，只回句柄；公钥 ek 给应用。 */
int SDFE_GenerateKeyPair_MLKEM(SDFE_HANDLE hSession, uint32_t pset,
                               uint8_t *ek, uint32_t *ek_len,
                               uint32_t *key_handle);
int SDFE_Encapsulate_MLKEM(SDFE_HANDLE hSession, uint32_t pset,
                           const uint8_t *ek, uint32_t ek_len,
                           uint8_t *ss, uint32_t *ss_len,
                           uint8_t *ct, uint32_t *ct_len);
/* 用句柄解封装 —— 应用从头到尾没见过 dk */
int SDFE_Decapsulate_MLKEM(SDFE_HANDLE hSession, uint32_t key_handle,
                           const uint8_t *ct, uint32_t ct_len,
                           uint8_t *ss, uint32_t *ss_len);

/* ---- ML-DSA ----
 * 私钥 sk 留在 PL 片内的签名金库里，只回公钥 pk 和一个槽号（句柄）。
 * 与 ML-KEM 那组的口径逐字一致，见文件头第三条。
 *
 * ctx（FIPS 204 的 domain separation 上下文串）：**当前只支持空 ctx**。
 * 已定的寄存器约定里输入字节流没有给 ctx 字节留位置，只有一个 CTX_LEN；
 * 与其猜一个位置、猜错了签到另一条 M' 上（合法、验得过、却不是你要签的消息），
 * 不如现在明确拒绝，等从机落地与 RTL 对齐后再放开。ctx_len != 0 回 SDR_INARGERR。 */
int SDFE_GenerateKeyPair_MLDSA(SDFE_HANDLE hSession, uint32_t pset,
                               uint8_t *pk, uint32_t *pk_len,
                               uint32_t *key_handle);
/* 用句柄签名 —— 应用从头到尾没见过 sk。
 * 签名随机数由硬件自取（FIPS 204 的 hedged 模式），接口上没有喂 rnd 的入口：
 * 确定性签名要 rnd=0³²，而寄存器面不提供那条路，所以本接口不假装支持它。 */
int SDFE_Sign_MLDSA(SDFE_HANDLE hSession, uint32_t key_handle,
                    const uint8_t *msg, uint32_t msg_len,
                    const uint8_t *ctx, uint32_t ctx_len,
                    uint8_t *sig, uint32_t *sig_len);
/* 用公钥验签。验不过回 SDR_VERIFYFAIL（是结果不是故障，但仍是非 OK 码）。 */
int SDFE_Verify_MLDSA(SDFE_HANDLE hSession, uint32_t pset,
                      const uint8_t *pk, uint32_t pk_len,
                      const uint8_t *msg, uint32_t msg_len,
                      const uint8_t *ctx, uint32_t ctx_len,
                      const uint8_t *sig, uint32_t sig_len);

/* ---- 对称：密钥进 key_vault，之后只按槽号使唤 ---- */
int SDFE_ImportKey(SDFE_HANDLE hSession, uint32_t slot,
                   const uint8_t *key, uint32_t key_len);
/* ---- 工作模式（CBC / CTR / CFB / OFB）--------------------------------------
 * 与上面两个单分组函数的关系：**分组变换是同一个硬件核**，模式的链接与异或在
 * daemon 里做。所以"密钥不出金库"这条性质在模式下原样成立，而**不能**把它说成
 * "硬件 CBC" —— RTL 里没有模式状态机。
 *
 * `iv` 恒为 16 字节：CBC 是初始向量，CTR 是初始计数块，CFB/OFB 是初始反馈。
 *
 * ⚠️ **IV 由调用方给，本库不替你生成。**
 *    · CBC 的 IV 要不可预测（否则有选择明文攻击）；
 *    · **CTR/OFB/CFB 的 (密钥, IV) 绝不能重复** —— 重复即密钥流复用，
 *      两条密文异或就把两条明文的异或暴露出来。
 *    要一串好随机数就用 SDFE_GenerateRandom：它取自 PL 里的环振噪声源。
 *
 * ⚠️ **不做填充。** ECB/CBC 要求 `len` 是 16 的非零整数倍；CTR/CFB/OFB 任意
 *    长度。填充放在这一层会引来 padding oracle，那是调用方的协议该决定的事。
 *
 * 长度上限见 wire.h 的 PQCS_MAXPAY（当前 16384，减去 16 字节 IV）。 */
#define SDFE_MODE_ECB    0
#define SDFE_MODE_CBC    1
#define SDFE_MODE_CTR    2
#define SDFE_MODE_CFB    3
#define SDFE_MODE_OFB    4

int SDFE_EncryptMode(SDFE_HANDLE hSession, uint32_t alg, uint32_t mode,
                     uint32_t slot, const uint8_t iv[16],
                     const uint8_t *in, uint32_t len, uint8_t *out);
int SDFE_DecryptMode(SDFE_HANDLE hSession, uint32_t alg, uint32_t mode,
                     uint32_t slot, const uint8_t iv[16],
                     const uint8_t *in, uint32_t len, uint8_t *out);

/* ---- 杂凑：SM3（硬件核，无密钥）----------------------------------------
 * out 恒 32 字节。一次一段。
 *
 * ⚠️ **不提供 Init/Update/Final 三段式。** sym_axi 里只有一份 SM3 上下文，
 *    两个会话交错 Update 会把状态搅在一起 —— 算出来的摘要合法但错，
 *    而那是最坏的一类失败。要流式得先在 RTL 里给上下文分槽。 */
int SDFE_Hash_SM3(SDFE_HANDLE hSession, const uint8_t *in, uint32_t len,
                  uint8_t out[32]);

int SDFE_Encrypt(SDFE_HANDLE hSession, uint32_t alg, uint32_t slot,
                 const uint8_t *in, uint8_t *out);
int SDFE_Decrypt(SDFE_HANDLE hSession, uint32_t alg, uint32_t slot,
                 const uint8_t *in, uint8_t *out);

/* ---- 后量子公钥加解密（KEM-DEM）----
 * 用公钥加密任意长度数据。ML-KEM 只封装 32 字节共享秘密，加不了任意数据，
 * 所以按标准 KEM-DEM 组合：Encaps 出共享秘密（KEM，硬件，dk 留片内），
 * 再用它做带认证的对称加密（DEM）。原型、blob 线格式与「哪一半在硬件、
 * 哪一半在软件」的诚实边界见 sdfe_pkenc.h。 */
#include "sdfe_pkenc.h"

const char *SDFE_StrError(int rv);

#ifdef __cplusplus
}
#endif
#endif
