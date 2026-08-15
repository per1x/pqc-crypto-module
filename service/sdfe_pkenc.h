/* sdfe_pkenc —— 后量子公钥加解密（KEM-DEM）
 *
 * ============================================================================
 * 【它是什么，边界在哪】
 * ============================================================================
 * 公钥加密任意长度的数据。ML-KEM 本身只是密钥封装（给一个 32 字节共享秘密），
 * 加不了任意数据 —— 所以按标准的 KEM-DEM 组合来拼：
 *
 *   PKEncrypt(ek, data):
 *       (ss, ct) = ML-KEM.Encaps(ek)          KEM：只用公钥
 *       (iv, ciphertext, tag) = AES-256-GCM(ss, data)   DEM：带认证
 *       → 输出 ct ‖ iv ‖ tag ‖ ciphertext
 *
 *   PKDecrypt(handle, blob):
 *       ss = ML-KEM.Decaps(handle, ct)         KEM：私钥在硬件片内金库，按句柄
 *       data = AES-256-GCM_open(ss, iv, tag, ciphertext)   认证失败即拒绝
 *
 * ============================================================================
 * 【诚实口径：哪一半在硬件、哪一半在软件】
 * ============================================================================
 *   · **KEM 那一半在硬件**：Encaps/Decaps 走 FPGA 的 ML-KEM 核，
 *     私钥 dk 从生成到使用一步不出片内金库（PKDecrypt 只传句柄）。
 *   · **DEM 那一半在软件**：ss 从 Decaps 出来后就是一个对称密钥，
 *     用它做 AES-256-GCM 是在本进程里用 OpenSSL 完成的。
 *
 * 这不是妥协，是 KEM-DEM 的本来形态 —— 共享秘密必然要变成对称密钥被用掉。
 * 真正要守住的「私钥不出硬件」守住了（dk 从不出片内）。文档里不把这句
 * 含糊成「整个加密都在硬件」。
 *
 * DEM 用 **带认证的 AES-256-GCM**，不是裸 ECB：密文被篡改（包括 tag、iv、
 * ct 任一位）时 PKDecrypt 返回失败，而不是给出一段错误明文。
 */
#ifndef PQCHSM_SDFE_PKENC_H
#define PQCHSM_SDFE_PKENC_H

#include <stddef.h>
#include <stdint.h>

#include "sdfe.h"

/* blob 布局（全部大端长度前缀，避免歧义）：
 *   magic(4) ‖ ct_len(4) ‖ ct ‖ iv(12) ‖ tag(16) ‖ ciphertext
 * ct_len 由 pset 决定，冗余存一份是为了解密方不必先知道 pset 就能切分。 */
#define SDFE_PKENC_MAGIC   0x504B4531u   /* "PKE1" */
#define SDFE_PKENC_IVLEN   12
#define SDFE_PKENC_TAGLEN  16
#define SDFE_PKENC_OVERHEAD(ct_len) (4 + 4 + (ct_len) + 12 + 16)

/* 公钥加密。ek 是 ML-KEM 公钥，pset 是参数集（SDFE_MLKEM_*）。
 * out 需容纳 SDFE_PKENC_OVERHEAD(ct_len)+data_len 字节。 */
int SDFE_PKEncrypt(SDFE_HANDLE hSession, uint32_t pset,
                   const uint8_t *ek, uint32_t ek_len,
                   const uint8_t *data, uint32_t data_len,
                   uint8_t *out, uint32_t *out_len);

/* 公钥解密。key_handle 指向硬件片内金库里的 ML-KEM 私钥。
 * 认证失败（密文被改）返回非 0，且不产生任何明文。 */
int SDFE_PKDecrypt(SDFE_HANDLE hSession, uint32_t key_handle,
                   const uint8_t *blob, uint32_t blob_len,
                   uint8_t *data, uint32_t *data_len);

#endif
