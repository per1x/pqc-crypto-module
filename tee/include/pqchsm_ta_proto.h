/* pqchsm_ta_proto.h —— 普通世界 ↔ pqc-hsm TA 命令协议
 *
 * 本头文件同时被 TA(tee/ta/) 与普通世界客户端(tee/host/) 包含，
 * 是安全边界两侧唯一的协议事实来源。改动必须两侧同步。
 *
 * 设计见 docs/reference/tee-protocol.zh-CN.md。要点：
 *   - 私钥材料不出 TA：KEYGEN 返回 (公钥, 包裹后的私钥 blob)，
 *     SIGN/DECAPS 以 blob 为输入，TA 内解包即用即焚。
 *   - 包裹格式复用 include/pqchsm/wrap.h 的 PWRP（AES-256-GCM），
 *     KEK 由 TA 内 KDR 派生，不出 S-EL1。
 *   - 算法编号与 include/pqchsm/pqc.h 的 pqc_alg_t 一一对应。
 */
#ifndef PQCHSM_TA_PROTO_H
#define PQCHSM_TA_PROTO_H

#include <stdint.h>

/* TA UUID：pqchsm 专用，随机生成后固定，不得与其他 TA 冲突 */
#define TA_PQCHSM_UUID \
	{ 0x4e2d9c1a, 0x7b35, 0x4f68, \
	  { 0x9a, 0x2c, 0xd1, 0x5e, 0x88, 0x40, 0x6f, 0xa3 } }

/* 协议版本（CMD_GET_INFO 返回，host 据此做兼容性检查） */
#define PQCHSM_TA_PROTO_VERSION  0x00010000U  /* 1.0.0 */

/* ---- 算法编号：与 pqc_alg_t（include/pqchsm/pqc.h）数值一致 ---- */
#define TA_ALG_NONE        0
#define TA_ALG_ML_KEM_512  1
#define TA_ALG_ML_KEM_768  2
#define TA_ALG_ML_KEM_1024 3
#define TA_ALG_ML_DSA_44   4
#define TA_ALG_ML_DSA_65   5
#define TA_ALG_ML_DSA_87   6

/* ---- 命令码 ---- */
/*
 * CMD_GET_INFO：握手/自检。
 *   [0] value.out: a = 协议版本(PQCHSM_TA_PROTO_VERSION), b = 特性位
 * 特性位：bit0 = 支持 KDF（**已删除，恒 0**）；
 *        bit1 = 支持 WRAP/UNWRAP（**已删除，恒 0**）；
 *        bit2 = 支持 ML-KEM；bit3 = 支持 ML-DSA
 * bit0/bit1 保留位置而不是让后面的位往前挪：挪了的话旧 host 会把
 * "支持 ML-KEM" 读成 "支持 KDF"。
 */
#define TA_PQCHSM_CMD_GET_INFO        0

/* ============================================================================
 * 【已删除的三条命令：1 / 3 / 4 —— 编号永久保留，不得复用】
 * ============================================================================
 * 它们是登记表 PS-22 / PS-24 那两颗地雷。删掉而不是收紧，是因为**产品路径
 * 一条都不用它们**（KEYGEN 返回包裹好的 blob，SIGN/DECAPS 在 TA 内部自己
 * 解包），而它们各自都是一个完整的谕言机：
 *
 *  · CMD_KDF_DERIVE（1）：拿 TA 内的 KDR 按**调用方给的任意 label + 任意
 *    salt** 派生并把结果交出来。于是任何普通世界进程只要写
 *        label = "pqc-hsm/storage-kek"、salt = keystore 头部那 16 字节明文
 *    就能把**存储 KEK 原样要出来**。KDR 的全部意义是"只进不出"，而这条
 *    命令是一个带域分隔参数的读出口。
 *    ⚠️ 今天泄的还只是由 SHA-256(DNA) 退化根派生出来的 KEK；**等量产接上
 *    真 HUK，泄的就是真 KEK** —— 信任根越真，这个洞越值钱。
 *
 *  · CMD_KEK_SET（2，保留）+ CMD_UNWRAP（4）：前者让调用方指定 salt 来定
 *    KEK，后者拿这个 KEK 解开**任意** blob。两条合起来就是一台通用解包机：
 *    拿到 keystore 文件 → 读出它的明文 salt → KEK_SET → UNWRAP 每一条记录。
 *    KEK_SET 本身单独留着无害（它只在 TA 内缓存一个值，没有出口），
 *    真正要拿掉的是那个出口。
 *
 *  · CMD_WRAP（3）：与 UNWRAP 成对，是同一个 KEK 下的加密谕言机。危害小于
 *    解包，但同样没有产品用途，一起删干净 —— 留着半对更容易被将来补全。
 *
 * **编号 1/3/4 永久保留不复用**：普通世界可能还有旧客户端在发它们，把编号
 * 让给新命令的话，一个旧调用会安静地触发一件完全不同的事。
 * TA 侧对它们不再有 case，落到 default 返回 TEE_ERROR_NOT_SUPPORTED。
 */
#define TA_PQCHSM_CMD_RETIRED_KDF_DERIVE  1
#define TA_PQCHSM_CMD_RETIRED_WRAP        3
#define TA_PQCHSM_CMD_RETIRED_UNWRAP      4

/*
 * CMD_KEK_SET：指定密钥库 salt，派生并缓存本实例 KEK（KEK 不出 TA）。
 *   [0] memref.in: salt（16B，密钥库头部所存）
 * 之后 KEYGEN 系列 / SIGN / DECAPS 均使用该 KEK 包裹、解包私钥 blob。
 * 它没有任何把 KEK 或明文交出去的路径 —— 那条路径（CMD_UNWRAP）已经删掉。
 */
#define TA_PQCHSM_CMD_KEK_SET         2

/*
 * CMD_KEYGEN：生成密钥对。私钥以 PWRP blob 返回，公钥明文返回。
 *   [0] value.in: a = 算法编号
 *   [1] memref.output: 公钥
 *   [2] memref.inout:  包裹私钥 blob（SHORT_BUFFER 语义同 WRAP）
 */
#define TA_PQCHSM_CMD_KEYGEN          5

/*
 * CMD_KEYGEN_FROM_SEED：从种子展开密钥对（种子存储优化路径）。
 *   [0] value.in: a = 算法编号
 *   [1] memref.in: 种子（ML-KEM: 64B d‖z；ML-DSA: 32B ξ）
 *   [2] memref.output: 公钥
 *   [3] memref.inout: 包裹私钥 blob
 */
#define TA_PQCHSM_CMD_KEYGEN_FROM_SEED 6

/*
 * CMD_DECAPS：ML-KEM 解封装。
 *   [0] value.in: a = 算法编号（限 ML-KEM）
 *   [1] memref.in: 包裹私钥 blob
 *   [2] memref.in: 密文 ct
 *   [3] memref.output: 共享秘密 ss
 * 隐式重加密校验不通过时返回 TEE_ERROR_GENERIC（FIPS 203 的静默失败
 * 在库内已完成，此错误仅在参数/解包层面失败时出现）。
 */
#define TA_PQCHSM_CMD_DECAPS          7

/*
 * CMD_SIGN：ML-DSA 签名（hedged，rnd 由 TA 内 TRNG 现取）。
 *   [0] value.in: a = 算法编号（限 ML-DSA）
 *   [1] memref.in: 包裹私钥 blob
 *   [2] memref.in: 消息帧 = ctx_len(1B) ‖ ctx ‖ msg（ctx_len 可为 0）
 *   [3] memref.inout: 签名（SHORT_BUFFER 语义）
 * 注意：v1 不提供显式 rnd 的确定性签名（那是 KAT 驱动路径，留在普通
 * 世界的软件后端里）；协议保留扩展位，需要时加 CMD_SIGN_DERAND。
 */
#define TA_PQCHSM_CMD_SIGN            8

/* 单条命令各 memref 的尺寸上限（host 与 TA 共同遵守） */
#define TA_PQCHSM_MAX_LABEL    64
#define TA_PQCHSM_MAX_SALT     64
#define TA_PQCHSM_MAX_AAD      256
#define TA_PQCHSM_MAX_PT       8192     /* 私钥明文最大 ~4.9KB（ML-DSA-87 sk） */
#define TA_PQCHSM_MAX_BLOB     (TA_PQCHSM_MAX_PT + 64)
#define TA_PQCHSM_MAX_MSG      (64 * 1024)
#define TA_PQCHSM_MAX_PK       2592     /* ML-DSA-87 pk */
#define TA_PQCHSM_MAX_SIG      4627     /* ML-DSA-87 sig */
#define TA_PQCHSM_MAX_CT       1568     /* ML-KEM-1024 ct */
#define TA_PQCHSM_MAX_SS       32
#define TA_PQCHSM_MAX_SEED     64

#endif /* PQCHSM_TA_PROTO_H */
