/* sdfe.h —— SDF 风格的调用接口（代表性子集）
 *
 * "SDFE" = SDF Extended：函数命名沿用 GM/T 0018 的习惯（SDF_ 前缀 + 驼峰），
 * 但 ML-KEM 这一族国内标准接口尚未定，所以用 SDFE_ 前缀标明是扩展、
 * 不冒充标准函数名。理由与取舍见 docs/服务层设计.md §3.3。
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
 *   · **ML-KEM 私钥 dk**：当前硬件的 KeyGen 会把 ek‖dk 一起交出来
 *     （ACVP 核对的需要）。本库**不把 dk 交给应用** —— daemon 留着它，
 *     只回一个句柄。所以从应用视角看"私钥不出接口"，但它确实出了硬件。
 *     这个区别在 docs/服务层设计.md §4 里写明了，不能含糊成一句
 *     "私钥不出硬件"。
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

typedef void *SDFE_HANDLE;

/* ML-KEM 参数集 */
#define SDFE_MLKEM_512   0
#define SDFE_MLKEM_768   1
#define SDFE_MLKEM_1024  2

/* 对称算法（与 sym_axi 的 ALG 字段一致） */
#define SDFE_ALG_AES128  0
#define SDFE_ALG_AES256  1
#define SDFE_ALG_SM4     2

/* ---- 设备与会话 ---- */
int SDFE_OpenDevice(SDFE_HANDLE *phDev);
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

/* ---- 对称：密钥进 key_vault，之后只按槽号使唤 ---- */
int SDFE_ImportKey(SDFE_HANDLE hSession, uint32_t slot,
                   const uint8_t *key, uint32_t key_len);
int SDFE_Encrypt(SDFE_HANDLE hSession, uint32_t alg, uint32_t slot,
                 const uint8_t *in, uint8_t *out);
int SDFE_Decrypt(SDFE_HANDLE hSession, uint32_t alg, uint32_t slot,
                 const uint8_t *in, uint8_t *out);

const char *SDFE_StrError(int rv);

#ifdef __cplusplus
}
#endif
#endif
