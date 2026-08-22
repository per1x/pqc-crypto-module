/* pqc_native.c —— pqc_backend_t 的 mlkem-native / mldsa-native 实现
 *
 * ============================================================================
 * 【它顶掉了什么，以及为什么值得】
 * ============================================================================
 * 这个文件替掉了 pqc_liboqs.c（266 行）与 oqs_rng.c（185 行），连带**整个
 * liboqs 外部依赖**——根 CMakeLists 里那句"找不到 liboqs 就 FATAL_ERROR"
 * 也一起删了。换来的是三件实打实的事：
 *
 *  ① **同一份算法实现被安全世界与普通世界共用。** vendored 的
 *     mlkem-native / mldsa-native 本来只在 `tee/ta/vendor/` 下、只被 OP-TEE
 *     的 sub.mk 引用；现在挪到 `third_party/pqc-native/`，两套构建共用同一
 *     棵树。以前 TA 与主库跑的是**两套不同的实现**（TA 走 native、主库走
 *     liboqs），"TA 算出来的和普通世界算出来的一样"只能靠 KAT 巧合成立。
 *
 *  ② **那个进程级全局随机源没有了。** liboqs 的 randombytes 是进程级全局
 *     状态，而本项目又要用它做确定性脚本（ACVP 的 KAT 与种子存储），于是
 *     oqs_rng.c 不得不用一把递归锁把**所有**会取随机数的路径串起来 ——
 *     少包一个就会两条路互吃对方的字节（那条并发缺陷有专门的回归用例）。
 *     native 那两个库把去随机化做成**显式参数**（coins / seed / rnd），
 *     确定性与随机两条路在类型上就分开了，锁与"记得包上"这件事一起消失。
 *
 *  ③ 少一个必须从 brew/apt 装的依赖。
 *
 * ⚠️ **本文件不做任何密码学**：它只是把 pqc_backend_t 的 vtable 转接到
 *    tee/ta/ta_pqc.c 那一层（那层再按算法编号分派到六个参数集编译单元）。
 *    调度层与参数集单元物理上仍在 tee/ta/ 下，是有意的：TA 由 OP-TEE 的
 *    dev kit 构建，那套构建的相对路径最脆弱，而它在板上是唯一能验的一侧。
 *    与其为了目录好看去动它，不如让 CMake 这边把同一批 .c 编进来 ——
 *    **一份源码、两套构建**，正是这次要的结果。
 *
 * 【算法编号】ta_pqc.h 的 alg 与 pqc_alg_t 数值一一相同（两处都注明了这条
 * 约定），所以这里直接强转，不做映射表 —— 多一层映射就多一处会对不上的地方。
 */
#include "pqchsm/pqc.h"
#include "pqchsm/util.h"

#include "ta_pqc.h"
#include "ta_pqc_low.h"

#include <string.h>

/* ta_pqc.c 的返回码就是 pqc_status_t 的数值（见 ta_pqc.h 文件头）。
 * 这里仍然显式过一道，免得哪天那边改了数值而这边毫无察觉。 */
static pqc_status_t map(int rc)
{
	switch (rc) {
	case  0: return PQC_OK;
	case -1: return PQC_ERR_BAD_ARG;
	case -2: return PQC_ERR_UNSUPPORTED;
	case -4: return PQC_ERR_VERIFY;
	default: return PQC_ERR_BACKEND;
	}
}

static int is_kem(pqc_alg_t alg)
{
	const ta_pqc_dims_t *d = ta_pqc_dims((uint32_t)alg);

	return d && d->is_kem;
}

/* ---- KEM / SIG 通用 ----------------------------------------------------- */

static pqc_status_t be_keypair(pqc_alg_t alg, uint8_t *pk, uint8_t *sk)
{
	return map(ta_pqc_keypair((uint32_t)alg, pk, sk));
}

static pqc_status_t be_keypair_from_seed(pqc_alg_t alg,
                                         const uint8_t *seed, size_t seed_len,
                                         uint8_t *pk, uint8_t *sk)
{
	/* ML-KEM 要 64 字节 d‖z，ML-DSA 要 32 字节 ξ。长度由 ta_pqc 再校验一遍，
	 * 这里不重复写死数字 —— 写死就是第三处会漂移的地方。 */
	return map(ta_pqc_keypair_from_seed((uint32_t)alg, seed, seed_len, pk, sk));
}

static pqc_status_t be_encaps(pqc_alg_t alg, const uint8_t *pk,
                              uint8_t *ct, uint8_t *ss)
{
	if (!is_kem(alg)) {
		return PQC_ERR_BAD_ARG;
	}
	return map(ta_pqc_encaps((uint32_t)alg, pk, ct, ss));
}

static pqc_status_t be_decaps(pqc_alg_t alg, const uint8_t *sk,
                              const uint8_t *ct, uint8_t *ss)
{
	if (!is_kem(alg)) {
		return PQC_ERR_BAD_ARG;
	}
	return map(ta_pqc_decaps((uint32_t)alg, sk, ct, ss));
}

/* ---- 去随机化的封装：ACVP 的 encaps 向量要按给定的 m 复现 --------------- */
static pqc_status_t be_encaps_derand(pqc_alg_t alg, const uint8_t *pk,
                                     const uint8_t *m, size_t m_len,
                                     uint8_t *ct, uint8_t *ss)
{
	if (!is_kem(alg) || !pk || !m || !ct || !ss || m_len != 32) {
		return PQC_ERR_BAD_ARG;
	}
	switch (alg) {
	case PQC_ALG_ML_KEM_512:
		return map(pqchsm_mlk512_enc_derand(ct, ss, pk, m) ? -3 : 0);
	case PQC_ALG_ML_KEM_768:
		return map(pqchsm_mlk768_enc_derand(ct, ss, pk, m) ? -3 : 0);
	case PQC_ALG_ML_KEM_1024:
		return map(pqchsm_mlk1024_enc_derand(ct, ss, pk, m) ? -3 : 0);
	default:
		return PQC_ERR_BAD_ARG;
	}
}

/* ---- 签名 --------------------------------------------------------------- */
/* rnd == NULL 走 hedged（库内部现取 32 字节）；给了 rnd 就是 FIPS 204
 * §Algorithm 2 里那 32 字节的显式值 —— 全 0 即确定性签名，ACVP 的
 * siggen 确定性条目正是它。
 *
 * 这里与 liboqs 那一版最大的不同：**不需要脚本化全局随机源**。
 * 老版本要先 pqc_oqs_rng_begin(rnd) 把 32 字节喂进进程级随机源、跑完再
 * 核对"消费了正好 32 字节"，因为 liboqs 没有显式 rnd 的入口。
 * native 有，于是那一整套（连同它的锁与"消费模型不符就作废"的兜底）
 * 都不存在了。 */
static pqc_status_t be_sign(pqc_alg_t alg, const uint8_t *sk,
                            const uint8_t *msg, size_t msg_len,
                            const uint8_t *ctx, size_t ctx_len,
                            const uint8_t *rnd,
                            uint8_t *sig, size_t *sig_len)
{
	if (is_kem(alg) || !ta_pqc_dims((uint32_t)alg)) {
		return PQC_ERR_BAD_ARG;
	}
	if (!rnd) {
		return map(ta_pqc_sign((uint32_t)alg, sk, msg, msg_len,
		                       ctx, ctx_len, sig, sig_len));
	}
	if (ctx_len > 255) {
		return PQC_ERR_BAD_ARG;
	}
	switch (alg) {
	case PQC_ALG_ML_DSA_44:
		return map(pqchsm_mld44_sign_rnd(sig, sig_len, msg, msg_len,
		                                 ctx, ctx_len, rnd, sk) ? -3 : 0);
	case PQC_ALG_ML_DSA_65:
		return map(pqchsm_mld65_sign_rnd(sig, sig_len, msg, msg_len,
		                                 ctx, ctx_len, rnd, sk) ? -3 : 0);
	case PQC_ALG_ML_DSA_87:
		return map(pqchsm_mld87_sign_rnd(sig, sig_len, msg, msg_len,
		                                 ctx, ctx_len, rnd, sk) ? -3 : 0);
	default:
		return PQC_ERR_BAD_ARG;
	}
}

static pqc_status_t be_verify(pqc_alg_t alg, const uint8_t *pk,
                              const uint8_t *msg, size_t msg_len,
                              const uint8_t *ctx, size_t ctx_len,
                              const uint8_t *sig, size_t sig_len)
{
	if (is_kem(alg)) {
		return PQC_ERR_BAD_ARG;
	}
	/* 验签不过是"结果"不是"故障"，区分开上层才能正确落审计 ——
	 * ta_pqc_verify 已经把它映成 -4（= PQC_ERR_VERIFY）。 */
	return map(ta_pqc_verify((uint32_t)alg, pk, msg, msg_len,
	                         ctx, ctx_len, sig, sig_len));
}

static const pqc_backend_t g_native = {
	.name              = "pqc-native(mlkem-native/mldsa-native)",
	.keypair           = be_keypair,
	.keypair_from_seed = be_keypair_from_seed,
	.encaps            = be_encaps,
	.encaps_derand     = be_encaps_derand,
	.decaps            = be_decaps,
	.sign              = be_sign,
	.verify            = be_verify,
};

const pqc_backend_t *pqc_backend_native(void)
{
	return &g_native;
}
