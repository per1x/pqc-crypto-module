/* pqchsm/pqc.h —— PQC 算法后端抽象
 *
 * 这是整个项目最重要的接口边界：要求上层（槽位管理器 / 密钥库 /
 * 备份恢复 / PKCS#11）**只通过本接口**做密码运算，从而在把算法核搬到 FPGA 时
 * "上层一行不改"。
 *
 * 当前实现：pqc_backend_liboqs()  —— liboqs 0.16 软件实现
 * 已备接口：pqc_backend_accel()   —— AXI-Lite 寄存器驱动 PL 硬件核
 *
 * 注意「硬件核」不是设想：ML-KEM 512/768/1024 已在 PL 里跑通并对 NIST ACVP
 * 向量逐字节一致（见 docs/SECURITY.md）。板上驱动它们的是 board/
 * 下的独立程序；把 src/ 这条 transport 接到同一批核上是尚未做的一步。
 *
 * 设计约束（为硬件后端预留）：
 *   1. 所有缓冲由调用方按 pqc_alg_info() 给出的长度预分配 —— 硬件后端不做动态分配；
 *   2. 无隐式全局随机源：签名的 rnd 显式传入（NULL = 后端自取 TRNG），
 *      对应 FIPS 204 的 hedged / deterministic 两种模式，也让 KAT 可驱动；
 *   3. keypair_from_seed 是一等公民而非测试后门 —— 的种子存储优化
 *      （每槽只存 32/64 B 种子，用时片内重展开）在生产路径上就依赖它。
 */
#ifndef PQCHSM_PQC_H
#define PQCHSM_PQC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	PQC_OK              =  0,
	PQC_ERR_BAD_ARG     = -1,
	PQC_ERR_UNSUPPORTED = -2,  /* 后端不具备该能力（如硬件核未实现该参数集） */
	PQC_ERR_BACKEND     = -3,  /* 后端内部失败 */
	PQC_ERR_VERIFY      = -4,  /* 验签/解封装校验不通过 —— 非错误，是结果 */
	PQC_ERR_SELF_TEST   = -5,  /* 模块处于错误状态：上电自测未通过，拒绝服务 */
} pqc_status_t;

typedef enum {
	PQC_ALG_NONE = 0,
	PQC_ALG_ML_KEM_512,
	PQC_ALG_ML_KEM_768,
	PQC_ALG_ML_KEM_1024,
	PQC_ALG_ML_DSA_44,
	PQC_ALG_ML_DSA_65,
	PQC_ALG_ML_DSA_87,
	PQC_ALG__COUNT
} pqc_alg_t;

typedef enum {
	PQC_KIND_KEM = 1,
	PQC_KIND_SIG = 2,
} pqc_kind_t;

/* FIPS 204 的 rnd 长度；deterministic 模式即 rnd = 0^32 */
#define PQC_SIG_RND_LEN 32

typedef struct {
	pqc_alg_t  alg;
	pqc_kind_t kind;
	const char *name;    /* 与 ACVP parameterSet 字符串一致，如 "ML-KEM-768" */
	size_t pk_len;
	size_t sk_len;
	size_t ct_len;       /* KEM 密文 */
	size_t ss_len;       /* KEM 共享秘密 */
	size_t sig_len;      /* 签名最大长度 */
	size_t seed_len;     /* ML-KEM: 64 (d‖z)；ML-DSA: 32 (ξ) */
} pqc_alg_info_t;

/* alg 非法时返回 NULL */
const pqc_alg_info_t *pqc_alg_info(pqc_alg_t alg);
/* 名字未知时返回 PQC_ALG_NONE */
pqc_alg_t pqc_alg_by_name(const char *name);

/* ---- 后端 vtable ---------------------------------------------------------
 * 未实现的能力置 NULL，上层通过 pqc_* 包装函数会得到 PQC_ERR_UNSUPPORTED。
 */
typedef struct pqc_backend {
	const char *name;

	pqc_status_t (*keypair)(pqc_alg_t alg, uint8_t *pk, uint8_t *sk);

	pqc_status_t (*keypair_from_seed)(pqc_alg_t alg,
	                                  const uint8_t *seed, size_t seed_len,
	                                  uint8_t *pk, uint8_t *sk);

	pqc_status_t (*encaps)(pqc_alg_t alg, const uint8_t *pk,
	                       uint8_t *ct, uint8_t *ss);

	/* m = 32 B 明文随机数（FIPS 203 Encaps 的输入），供 ACVP AFT 驱动 */
	pqc_status_t (*encaps_derand)(pqc_alg_t alg, const uint8_t *pk,
	                              const uint8_t *m, size_t m_len,
	                              uint8_t *ct, uint8_t *ss);

	pqc_status_t (*decaps)(pqc_alg_t alg, const uint8_t *sk,
	                       const uint8_t *ct, uint8_t *ss);

	/* rnd == NULL → 后端自取 TRNG（hedged）；否则须为 PQC_SIG_RND_LEN 字节 */
	pqc_status_t (*sign)(pqc_alg_t alg, const uint8_t *sk,
	                     const uint8_t *msg, size_t msg_len,
	                     const uint8_t *ctx, size_t ctx_len,
	                     const uint8_t *rnd,
	                     uint8_t *sig, size_t *sig_len);

	pqc_status_t (*verify)(pqc_alg_t alg, const uint8_t *pk,
	                       const uint8_t *msg, size_t msg_len,
	                       const uint8_t *ctx, size_t ctx_len,
	                       const uint8_t *sig, size_t sig_len);
} pqc_backend_t;

const pqc_backend_t *pqc_backend_liboqs(void);

/* 进程级当前后端；未设置时默认 liboqs。be == NULL 恢复默认。 */
void                 pqc_set_backend(const pqc_backend_t *be);
const pqc_backend_t *pqc_get_backend(void);

/* ---- 上层调用的包装函数（做参数校验后转发到当前后端）------------------- */
pqc_status_t pqc_keypair(pqc_alg_t alg, uint8_t *pk, uint8_t *sk);
pqc_status_t pqc_keypair_from_seed(pqc_alg_t alg, const uint8_t *seed, size_t seed_len,
                                   uint8_t *pk, uint8_t *sk);
pqc_status_t pqc_encaps(pqc_alg_t alg, const uint8_t *pk, uint8_t *ct, uint8_t *ss);
pqc_status_t pqc_encaps_derand(pqc_alg_t alg, const uint8_t *pk,
                               const uint8_t *m, size_t m_len,
                               uint8_t *ct, uint8_t *ss);
pqc_status_t pqc_decaps(pqc_alg_t alg, const uint8_t *sk, const uint8_t *ct, uint8_t *ss);
pqc_status_t pqc_sign(pqc_alg_t alg, const uint8_t *sk,
                      const uint8_t *msg, size_t msg_len,
                      const uint8_t *ctx, size_t ctx_len,
                      const uint8_t *rnd, uint8_t *sig, size_t *sig_len);
pqc_status_t pqc_verify(pqc_alg_t alg, const uint8_t *pk,
                        const uint8_t *msg, size_t msg_len,
                        const uint8_t *ctx, size_t ctx_len,
                        const uint8_t *sig, size_t sig_len);

const char *pqc_strerror(pqc_status_t st);

#ifdef __cplusplus
}
#endif
#endif /* PQCHSM_PQC_H */
