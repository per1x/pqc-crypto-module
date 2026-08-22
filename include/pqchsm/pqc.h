/* pqchsm/pqc.h —— PQC 算法后端抽象
 *
 * 这是整个项目最重要的接口边界：要求上层（槽位管理器 / 密钥库 /
 * 备份恢复 / PKCS#11）**只通过本接口**做密码运算，从而在把算法核搬到 FPGA 时
 * "上层一行不改"。
 *
 * 当前实现：pqc_backend_native()  —— mlkem-native / mldsa-native 软件实现
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

	/* ---- 句柄型操作：私钥根本不经过软件 ----------------------------------
	 *
	 * 上面那几个都是**按字节传私钥**的（decaps 的第二个参数就是 sk）。
	 * 对软件后端这没问题 —— 私钥本来就在进程内存里。但对 FPGA 后端，
	 * 它是个硬冲突：PL 里的 ML-KEM 现在把 dk 留在片内金库，KeyGen 只交出
	 * 公钥和一个槽号，而且有一道一次性闩锁封死 dk 的出口。照上面的签名
	 * 实现，就必须把 dk 拉回软件内存 —— 等于把"私钥不出硬件"这条性质
	 * 亲手抵消掉。
	 *
	 * 所以另开一组按句柄的操作。两条要点：
	 *
	 *   · **句柄由后端定义，上层只当它是个不透明的数。** 现在它就是 PL
	 *     金库的槽号，但上层不该知道，否则换一种硬件就要改 slot 层。
	 *
	 *   · **不支持就填 NULL，别做一个"内部先取出字节再调字节版"的实现。**
	 *     那种实现能让所有测试通过，而它恰好把这组接口存在的唯一理由
	 *     取消了 —— 上层无从分辨自己拿到的是真的硬件保管还是一层包装。
	 *     pqc_backend_has_hw_keys_kind() 就是给上层用来问这件事的。
	 */
	pqc_status_t (*keypair_hw)(pqc_alg_t alg, uint8_t *pk, uint32_t *hw_handle);

	pqc_status_t (*decaps_hw)(pqc_alg_t alg, uint32_t hw_handle,
	                          const uint8_t *ct, uint8_t *ss);

	/* 签名版的句柄操作。与 decaps_hw 同一条纪律：sk 只有一个槽号，
	 * 软件侧连一个能放它的缓冲区都不该存在。
	 *
	 * rnd 的语义比字节版**窄**：硬件核的 rnd 由 PL 自己的 TRNG 供给
	 * （FIPS 204 的 hedged 模式），寄存器面上没有喂 rnd 的入口。所以
	 *   · rnd == NULL  → hedged，正常路径；
	 *   · rnd != NULL  → 后端应当回 PQC_ERR_UNSUPPORTED，**不要默默忽略它**。
	 * 忽略的后果是：调用方以为自己在跑确定性模式（KAT / 复现实验），
	 * 拿到的却是一个合法但每次都不同的签名 —— 一个"验得过、对不上向量"
	 * 的结果最难查，因为两边都不报错。 */
	pqc_status_t (*sign_hw)(pqc_alg_t alg, uint32_t hw_handle,
	                        const uint8_t *msg, size_t msg_len,
	                        const uint8_t *ctx, size_t ctx_len,
	                        const uint8_t *rnd,
	                        uint8_t *sig, size_t *sig_len);
} pqc_backend_t;

/* 软件后端：vendored 的 mlkem-native / mldsa-native
 * （third_party/pqc-native/，与 OP-TEE TA 共用**同一棵源码树**）。
 * ⚠️ 它以前叫 pqc_backend_native()。改名不是洁癖：那个名字会一直告诉读者
 *    "这里连着 liboqs"，而整个依赖已经去掉了 —— 一个说谎的函数名比没有
 *    注释更贵。 */
const pqc_backend_t *pqc_backend_native(void);

/* 经密码机服务（pqchsm_fpgad）打 FPGA 的后端。
 * 本机走 UNIX socket；设了 PQCHSM_SDFE_HOST + PQCHSM_SDFE_PKI 就走 TCP（**mTLS**）。
 * ⚠️ 老的 PQCHSM_SDFE_TOKEN 已经没有任何作用 —— 远程口不再是预共享口令。
 * 连不上时返回 NULL —— 让调用方显式决定，不静默退回软件。 */
const pqc_backend_t *pqc_backend_sdfe(void);

/* 当前后端是否真的把**某一类**算法的私钥留在硬件里。
 * 上层据此决定生成密钥时走哪条路 —— 不要用后端名字去判。
 *
 * ============================================================================
 * 【为什么这里必须按种类问，不能是一个布尔】
 * ============================================================================
 * 一个密钥只有在**生成它**和**用它**两步都有句柄路径时，才谈得上"私钥在硬件里"：
 *   · KEM 要 keypair_hw + decaps_hw；
 *   · 签名要 keypair_hw + sign_hw。
 * 这是两个独立的事实。硬件完全可能只做了一半 —— 本项目就正好处在这个阶段：
 * ML-KEM 的核在板上跑着，ML-DSA 的 AXI 从机还没落地。
 *
 * 把两者合成一个布尔会朝两个方向都出错：
 *   · 合取（都在才算真）：ML-DSA 一天没落地，**ML-KEM 也跟着退回软件**，
 *     一条已经成立的硬件保证被另一条尚未成立的连累掉；
 *   · 析取（有一个就算真）：上层拿它去生成 ML-DSA 密钥，keypair_hw 走通了、
 *     签名却没有句柄路径，于是只能把 sk 拉回软件 —— 那正是这组接口要防的事，
 *     而且要到第一次签名时才暴露。
 *
 * kinds 是**位掩码**（PQC_KIND_* 的值就是为此定的 1/2），语义是**合取**：
 * 问 PQC_KIND_KEM|PQC_KIND_SIG 等于问"这两类是不是都在硬件里"。
 * 常规用法是拿单独一类去问，比如 pqc_alg_info(alg)->kind。 */
int pqc_backend_has_hw_keys_kind(unsigned kinds);

/* 旧名字，等价于 pqc_backend_has_hw_keys_kind(PQC_KIND_KEM)。
 * 加签名之前本项目只有 KEM 一条硬件路径，所有调用点问的其实都是它；
 * 保留是为了不动老调用方，**但新代码请直接写种类** —— 这个名字读起来像
 * "后端有没有硬件密钥"，而那个问题现在已经没有唯一答案了。 */
int pqc_backend_has_hw_keys(void);

pqc_status_t pqc_keypair_hw(pqc_alg_t alg, uint8_t *pk, uint32_t *hw_handle);
pqc_status_t pqc_decaps_hw(pqc_alg_t alg, uint32_t hw_handle,
                           const uint8_t *ct, uint8_t *ss);
/* rnd 必须为 NULL（hedged）；非 NULL 时回 PQC_ERR_UNSUPPORTED，见 vtable 注释 */
pqc_status_t pqc_sign_hw(pqc_alg_t alg, uint32_t hw_handle,
                         const uint8_t *msg, size_t msg_len,
                         const uint8_t *ctx, size_t ctx_len,
                         const uint8_t *rnd,
                         uint8_t *sig, size_t *sig_len);

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
