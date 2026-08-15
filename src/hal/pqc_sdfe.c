// pqc_sdfe —— 经密码机服务打 FPGA 的 pqc_backend_t 实现
//
// ============================================================================
// 【它和 pqc_accel.c 的区别】
// ============================================================================
// pqc_accel.c 也是一个硬件后端，但它打的是 pqc_accel_axi（带操作码的
// AXI-Stream 加速器）—— 那个从机**不在板上的位流里**（它的批量数据要一路
// DMA，是独立的一块工作，见 zu3eg_hsm_top.v 的说明）。
//
// 板上真正跑着的 ML-KEM 是 mlkem_axi，而它对普通世界是关死的
// （SECURE_ONLY=1）。唯一的通路是 pqchsm_fpgad → /dev/secmmio → EL3 的 SiP。
// 本文件就是把 pqc_backend_t 架在那条通路上。
//
// ML-DSA 走的是同一条路上的另一个从机 mldsa_axi（0x8006_0000）。
// **那个从机现在还不存在。** 本文件里的 sign_hw 与 keypair_hw 的 ML-DSA 分支
// 是照已定的寄存器约定写好的驱动：接口、线格式、长度断言都就位了，
// 但运算没有地方跑。从机落地之前，它们在板上会如实失败 ——
// 这一层一行密码运算都不做，失败就是失败，不会替硬件把签名算出来。
//
// ============================================================================
// 【为什么 keypair / decaps / sign 这几个字节版故意不实现】
// ============================================================================
// 它们的签名要求私钥以字节形式进出（keypair 要填 sk，decaps/sign 要收 sk）。
// 而 PL 里的 dk / ML-DSA 的 sk 都留在片内金库、并且有一道一次性闩锁封死出口 ——
// 交付形态下硬件**根本不会**把私钥交出来。
//
// 这里有两种写法，差别很大：
//   ① 填 NULL 或拒绝，让上层看见"这个后端做不了字节版"，于是改用句柄版；
//   ② 实现成"内部走句柄、把私钥伪造成一段字节"或者"关掉闩锁取出私钥"。
//
// 选 ①。写法 ② 能让现有测试全绿，而它恰好把这一整套改动的理由取消了：
// 上层再也分不清私钥到底在不在硬件里。**接口说不了谎比接口好用重要。**
//
// encaps 与 verify 是例外：它们只用公钥，字节版天然成立。
// encaps 照常打硬件；verify 故意留在软件，理由见 sdfe_verify 上方。
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>

#include "pqchsm/pqc.h"
#include "sdfe_conn.h"
/* 只为 PQCS_MAXPAY 一个常量。sdfe.h 有意不引它（那是公开头文件，不该依赖
 * 内部线格式），但本文件是 .c、本来就在线上这一侧，直接引比在这里再抄一份
 * "消息最多多长"要安全 —— 抄一份的后果是两边慢慢分叉，症状是长消息莫名被拒。 */
#include "wire.h"

/* pqc_alg_t → SDF 的参数集编号。ML-KEM 与 ML-DSA 各三档，两族的编号空间是
 * **各自独立**的（都是 0/1/2），所以分两个函数、各回各的 —— 合成一个再让调用方
 * 记得"这个 0 是哪一族的 0"，是纯粹自找的麻烦。
 * 两族之外的算法这个后端做不了，如实回 -1，让上层去用软件后端 —— 不猜、不代打。 */
static int pset_of(pqc_alg_t alg)
{
	switch (alg) {
	case PQC_ALG_ML_KEM_512:  return SDFE_MLKEM_512;
	case PQC_ALG_ML_KEM_768:  return SDFE_MLKEM_768;
	case PQC_ALG_ML_KEM_1024: return SDFE_MLKEM_1024;
	default:                  return -1;
	}
}

static int dsa_pset_of(pqc_alg_t alg)
{
	switch (alg) {
	case PQC_ALG_ML_DSA_44: return SDFE_MLDSA_44;
	case PQC_ALG_ML_DSA_65: return SDFE_MLDSA_65;
	case PQC_ALG_ML_DSA_87: return SDFE_MLDSA_87;
	default:                return -1;
	}
}

/* 两族公钥里最长的那个：ML-DSA-87 的 pk = 2592 */
#define SDFE_PK_MAX 2592

static pqc_status_t sdfe_keypair_hw(pqc_alg_t alg, uint8_t *pk, uint32_t *hw)
{
	uint8_t buf[SDFE_PK_MAX];
	uint32_t len = sizeof buf, h = 0;
	int ps  = pset_of(alg);
	int dps = dsa_pset_of(alg);
	int rv;

	SDFE_HANDLE ses;

	if ((ps < 0 && dps < 0) || !pk || !hw)
		return PQC_ERR_UNSUPPORTED;
	sdfe_conn_lock();
	ses = sdfe_conn_get();
	if (!ses) { sdfe_conn_unlock(); return PQC_ERR_BACKEND; }
	rv = (ps >= 0)
	     ? SDFE_GenerateKeyPair_MLKEM(ses, (uint32_t)ps, buf, &len, &h)
	     : SDFE_GenerateKeyPair_MLDSA(ses, (uint32_t)dps, buf, &len, &h);
	if (rv != SDR_OK) {
		sdfe_conn_drop();
		sdfe_conn_unlock();
		return PQC_ERR_BACKEND;
	}
	sdfe_conn_unlock();
	/* 长度必须与元数据表对得上再抄。对不上意味着线上那一层与本层对这把密钥的
	 * 理解不一致，此时按 len 抄进调用方按 pk_len 分配的缓冲就是越界写。 */
	{
		const pqc_alg_info_t *info = pqc_alg_info(alg);

		if (!info || len != info->pk_len)
			return PQC_ERR_BACKEND;
	}
	memcpy(pk, buf, len);
	*hw = h;
	return PQC_OK;
}

/* 按句柄签名：sk 只有一个槽号，本函数里连一个能放它的缓冲区都不存在。
 *
 * 两处**故意的窄**，都不能改成"尽力而为"：
 *   · rnd != NULL：寄存器面没有喂 rnd 的入口，硬件自取 TRNG（hedged）。
 *     默默忽略它，调用方会以为自己跑的是确定性模式，拿到一个合法但对不上
 *     向量的签名 —— 两边都不报错，最难查。
 *   · ctx_len != 0：已定的输入字节流里没给 ctx 留位置（见 sdfe.h）。
 *     猜一个位置的后果是签在另一条 M' 上，签名合法却不是那条消息。
 * 两种情况都回 UNSUPPORTED，让上层显式地看见"这条路今天走不了"。 */
static pqc_status_t sdfe_sign_hw(pqc_alg_t alg, uint32_t hw,
                                 const uint8_t *msg, size_t msg_len,
                                 const uint8_t *ctx, size_t ctx_len,
                                 const uint8_t *rnd,
                                 uint8_t *sig, size_t *sig_len)
{
	const pqc_alg_info_t *info = pqc_alg_info(alg);
	uint32_t n;

	SDFE_HANDLE ses;

	(void)ctx;
	if (dsa_pset_of(alg) < 0 || !info || !sig || !sig_len)
		return PQC_ERR_UNSUPPORTED;
	if (rnd || ctx_len)
		return PQC_ERR_UNSUPPORTED;
	if ((!msg && msg_len) || msg_len > PQCS_MAXPAY || *sig_len < info->sig_len)
		return PQC_ERR_BAD_ARG;

	n = (uint32_t)*sig_len;
	sdfe_conn_lock();
	ses = sdfe_conn_get();
	if (!ses) { sdfe_conn_unlock(); return PQC_ERR_BACKEND; }
	if (SDFE_Sign_MLDSA(ses, hw, msg, (uint32_t)msg_len, NULL, 0,
	                    sig, &n) != SDR_OK) {
		sdfe_conn_drop();
		sdfe_conn_unlock();
		return PQC_ERR_BACKEND;
	}
	sdfe_conn_unlock();
	if (n != info->sig_len)
		return PQC_ERR_BACKEND;
	*sig_len = n;
	return PQC_OK;
}

static pqc_status_t sdfe_decaps_hw(pqc_alg_t alg, uint32_t hw,
                                   const uint8_t *ct, uint8_t *ss)
{
	const pqc_alg_info_t *info = pqc_alg_info(alg);
	uint32_t ss_len = 32;

	SDFE_HANDLE ses;

	if (pset_of(alg) < 0 || !info || !ct || !ss)
		return PQC_ERR_UNSUPPORTED;
	sdfe_conn_lock();
	ses = sdfe_conn_get();
	if (!ses) { sdfe_conn_unlock(); return PQC_ERR_BACKEND; }
	if (SDFE_Decapsulate_MLKEM(ses, hw, ct, (uint32_t)info->ct_len,
	                           ss, &ss_len) != SDR_OK) {
		sdfe_conn_drop();
		sdfe_conn_unlock();
		return PQC_ERR_BACKEND;
	}
	sdfe_conn_unlock();
	return ss_len == info->ss_len ? PQC_OK : PQC_ERR_BACKEND;
}

static pqc_status_t sdfe_encaps(pqc_alg_t alg, const uint8_t *pk,
                                uint8_t *ct, uint8_t *ss)
{
	const pqc_alg_info_t *info = pqc_alg_info(alg);
	uint32_t ct_len, ss_len = 32;
	int ps = pset_of(alg);

	SDFE_HANDLE ses;

	if (ps < 0 || !info || !pk || !ct || !ss)
		return PQC_ERR_UNSUPPORTED;
	sdfe_conn_lock();
	ses = sdfe_conn_get();
	if (!ses) { sdfe_conn_unlock(); return PQC_ERR_BACKEND; }
	ct_len = (uint32_t)info->ct_len;
	if (SDFE_Encapsulate_MLKEM(ses, (uint32_t)ps, pk, (uint32_t)info->pk_len,
	                           ss, &ss_len, ct, &ct_len) != SDR_OK) {
		sdfe_conn_drop();
		sdfe_conn_unlock();
		return PQC_ERR_BACKEND;
	}
	sdfe_conn_unlock();
	return PQC_OK;
}

/* ============================================================================
 * 【按算法分派，而不是按操作降级 —— 这条界线是本文件的要害】
 * ============================================================================
 * PL 里现在只有 ML-KEM。ML-DSA 的签名/验签、以及别的参数集，硬件做不了。
 * 而 pqc_set_backend() 是进程全局的：装上这个后端之后，ML-DSA 也一起没了。
 * 实测就是这样 —— 整套 PKCS#11 用例 93/254 失败，全是签名那一族。
 *
 * 所以这里对硬件做不了的**算法**转交软件后端。但界线必须划死：
 *
 *   · 转交的判据是**算法**，不是"这次调用能不能成"。
 *     调用方要 ML-DSA，硬件没有，交给软件 —— 这是如实回答。
 *
 *   · **ML-KEM 的私钥操作永不转交。** keypair/keypair_from_seed/decaps
 *     一旦对 ML-KEM 回退到软件，"私钥在硬件里"当场变成谎话，
 *     而调用方看到的是一次完全正常的成功。这类回退比失败危险得多。
 *
 * 一句话：**换算法可以，换保证不行。**
 */
static int is_hw_alg(pqc_alg_t alg)
{
	return pset_of(alg) >= 0 || dsa_pset_of(alg) >= 0;
}

static const pqc_backend_t *sw(void)
{
	return pqc_backend_liboqs();
}

static pqc_status_t sdfe_keypair(pqc_alg_t alg, uint8_t *pk, uint8_t *sk)
{
	/* ML-KEM / ML-DSA：私钥必须留在片内，字节版没有正当实现 */
	if (is_hw_alg(alg))
		return PQC_ERR_UNSUPPORTED;
	return sw()->keypair ? sw()->keypair(alg, pk, sk) : PQC_ERR_UNSUPPORTED;
}

static pqc_status_t sdfe_keypair_seed(pqc_alg_t alg, const uint8_t *seed,
                                      size_t seed_len, uint8_t *pk, uint8_t *sk)
{
	if (is_hw_alg(alg))
		return PQC_ERR_UNSUPPORTED;
	return sw()->keypair_from_seed
	       ? sw()->keypair_from_seed(alg, seed, seed_len, pk, sk)
	       : PQC_ERR_UNSUPPORTED;
}

static pqc_status_t sdfe_decaps_bytes(pqc_alg_t alg, const uint8_t *sk,
                                      const uint8_t *ct, uint8_t *ss)
{
	/* 只有 ML-KEM 是 KEM，所以这一条实际上等于"永远拒绝"。
	 * 写成按算法判而不是直接 return，是为了将来加别的 KEM 时
	 * 这里的规则仍然读得懂。 */
	if (is_hw_alg(alg))
		return PQC_ERR_UNSUPPORTED;
	return sw()->decaps ? sw()->decaps(alg, sk, ct, ss) : PQC_ERR_UNSUPPORTED;
}

/* 确定性封装只用公钥，不碰私钥，交给软件不会削弱任何保证 ——
 * 但它也就**不在硬件上跑**，ACVP 的 AFT 向量走的是软件路径。这一点要说出来，
 * 免得有人拿 ACVP 全绿当成"封装在硬件上验过了"。 */
static pqc_status_t sdfe_encaps_derand(pqc_alg_t alg, const uint8_t *pk,
                                       const uint8_t *m, size_t m_len,
                                       uint8_t *ct, uint8_t *ss)
{
	return sw()->encaps_derand
	       ? sw()->encaps_derand(alg, pk, m, m_len, ct, ss) : PQC_ERR_UNSUPPORTED;
}

/* 字节版签名：与 sdfe_decaps_bytes 同一条规则。
 *
 * 它的签名要求 sk 以字节进来，而 ML-DSA 的 sk 现在归片内金库管 —— 正当的路是
 * sign_hw。这里对 ML-DSA 一律拒绝，理由和 decaps 那条一模一样：留着它，
 * 上层就永远分不清自己拿到的签名是硬件保管的私钥做的，还是一段软件里的字节做的。
 *
 * ⚠️ 有一个可见的后果，写出来免得被当成 bug：装上本后端之前用软件后端建过的
 *    ML-DSA 密钥（sk 存在密钥库里），换到本后端之后**签不动了**。
 *    这是对的 —— 那把密钥的私钥确实不在硬件里，本后端没有装作它在的义务。
 *    要用它就换回软件后端，要硬件保管就重新生成。 */
static pqc_status_t sdfe_sign(pqc_alg_t alg, const uint8_t *sk,
                              const uint8_t *msg, size_t msg_len,
                              const uint8_t *ctx, size_t ctx_len,
                              const uint8_t *rnd, uint8_t *sig, size_t *sig_len)
{
	if (is_hw_alg(alg))
		return PQC_ERR_UNSUPPORTED;
	return sw()->sign ? sw()->sign(alg, sk, msg, msg_len, ctx, ctx_len,
	                               rnd, sig, sig_len) : PQC_ERR_UNSUPPORTED;
}

/* 验签只用公钥，**故意留在软件**，理由不止一条：
 *
 *   · 它不碰私钥，放软件不削弱任何保证（与 encaps_derand 同理）；
 *   · 更重要的是，签名由硬件做、验签由软件做，这一对构成一次**独立交叉核对**。
 *     两边都用同一个硬件核，"自己验自己"验不出任何东西。
 *
 * 想显式地在硬件上验签（比如证明 verify 核本身是对的），走 SDFE_Verify_MLDSA
 * 那条 SDF 接口 —— 那是一个明摆着的选择，而不是这一层的默认行为。 */
static pqc_status_t sdfe_verify(pqc_alg_t alg, const uint8_t *pk,
                                const uint8_t *msg, size_t msg_len,
                                const uint8_t *ctx, size_t ctx_len,
                                const uint8_t *sig, size_t sig_len)
{
	return sw()->verify ? sw()->verify(alg, pk, msg, msg_len, ctx, ctx_len,
	                                   sig, sig_len) : PQC_ERR_UNSUPPORTED;
}

static const pqc_backend_t g_be = {
	.name = "sdfe(FPGA via pqchsm_fpgad)",
	.keypair = sdfe_keypair,
	.keypair_from_seed = sdfe_keypair_seed,
	.encaps = sdfe_encaps,
	.encaps_derand = sdfe_encaps_derand,
	.decaps = sdfe_decaps_bytes,
	.sign = sdfe_sign,
	.verify = sdfe_verify,
	.keypair_hw = sdfe_keypair_hw,
	.decaps_hw = sdfe_decaps_hw,
	.sign_hw = sdfe_sign_hw,
};

const pqc_backend_t *pqc_backend_sdfe(void)
{
	return &g_be;
}
