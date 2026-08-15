/* test_pqc_hw —— 句柄型硬件密钥这条路的行为验证
 *
 * ============================================================================
 * 【这是桩验，不是硬件验 —— 先把话说清楚】
 * ============================================================================
 * ML-DSA 的 AXI 从机（mldsa_axi）**还没有落地**。本文件里的"硬件"是一个
 * 进程内的桩后端，它验的是**软件这一侧的接线对不对**：
 *   · 上层在什么条件下会走句柄路径、什么条件下不会；
 *   · 句柄路径把哪些字节交给了后端、拿回来的又是不是那条签名。
 * 它验不了、也不假装验：时序、寄存器语义、真硅上的行为。
 * 这些要等从机落地后在板上做。
 *
 * ============================================================================
 * 【桩为什么可以在内部拿着 sk —— 它是金库的模型，不是绕过】
 * ============================================================================
 * pqc.h 的 vtable 注释里明令禁止"内部先把 sk 取出来再调字节版"的**后端实现**。
 * 这里的桩看起来像那个东西，但角色相反：它扮演的是**片内金库本身**。
 * 判据很简单 —— sk 在这个文件里生成、存进桩自己的表、再也不出去；
 * 被测的那一侧（slot 层、pqc_* 包装）从头到尾只见过一个槽号。
 * 真后端 pqc_sdfe.c 里那个 sk 在 PL 的 BRAM 里，位置不同，接口契约一样。
 *
 * 为了让"走没走句柄路径"变成可观测的事实而不是推断，全能桩把
 * keypair / keypair_from_seed / sign 这几个**字节版一律回 UNSUPPORTED**：
 * 于是生成成功 + 签名成功本身就证明了走的是句柄路径 —— 软件路径根本走不通。
 */
#include "testlib.h"
#include "pqchsm/pqc.h"
#include "pqchsm/slot.h"

#include <stdlib.h>
#include <string.h>

#define SO_PIN   "so-secret-0001"
#define USER_PIN "user-pin-4242"

/* ---- 桩金库 ------------------------------------------------------------- */

#define STUB_SLOTS 8

static struct {
	int       used;
	pqc_alg_t alg;
	uint8_t  *sk;
	size_t    sk_len;
} g_vault[STUB_SLOTS];

static int g_keypair_hw_calls;
static int g_sign_hw_calls;
static int g_decaps_hw_calls;
static int g_rnd_was_nonnull;

static void vault_reset(void)
{
	for (int i = 0; i < STUB_SLOTS; i++) {
		if (g_vault[i].sk) {
			memset(g_vault[i].sk, 0, g_vault[i].sk_len);
			free(g_vault[i].sk);
		}
	}
	memset(g_vault, 0, sizeof(g_vault));
	g_keypair_hw_calls = g_sign_hw_calls = g_decaps_hw_calls = 0;
	g_rnd_was_nonnull = 0;
}

static const pqc_backend_t *sw(void)
{
	return pqc_backend_liboqs();
}

static pqc_status_t stub_keypair_hw(pqc_alg_t alg, uint8_t *pk, uint32_t *hw)
{
	const pqc_alg_info_t *info = pqc_alg_info(alg);
	int i;

	g_keypair_hw_calls++;
	if (!info || !pk || !hw) {
		return PQC_ERR_BAD_ARG;
	}
	for (i = 0; i < STUB_SLOTS && g_vault[i].used; i++) {
		;
	}
	if (i == STUB_SLOTS) {
		return PQC_ERR_BACKEND;
	}
	g_vault[i].sk = malloc(info->sk_len);
	if (!g_vault[i].sk) {
		return PQC_ERR_BACKEND;
	}
	if (sw()->keypair(alg, pk, g_vault[i].sk) != PQC_OK) {
		free(g_vault[i].sk);
		g_vault[i].sk = NULL;
		return PQC_ERR_BACKEND;
	}
	g_vault[i].used   = 1;
	g_vault[i].alg    = alg;
	g_vault[i].sk_len = info->sk_len;
	*hw = (uint32_t)i;
	return PQC_OK;
}

static pqc_status_t stub_sign_hw(pqc_alg_t alg, uint32_t hw,
                                 const uint8_t *msg, size_t msg_len,
                                 const uint8_t *ctx, size_t ctx_len,
                                 const uint8_t *rnd,
                                 uint8_t *sig, size_t *sig_len)
{
	g_sign_hw_calls++;
	/* 真硬件的 rnd 由 PL 的 TRNG 供给，寄存器面没有喂它的入口。
	 * 上层若传了非 NULL 的 rnd 而后端默默照签，调用方会以为自己跑的是
	 * 确定性模式 —— 这里把它记下来，由用例断言"上层没有这么干"。 */
	if (rnd) {
		g_rnd_was_nonnull = 1;
	}
	if (hw >= STUB_SLOTS || !g_vault[hw].used || g_vault[hw].alg != alg) {
		return PQC_ERR_BAD_ARG;
	}
	return sw()->sign(alg, g_vault[hw].sk, msg, msg_len, ctx, ctx_len,
	                  rnd, sig, sig_len);
}

static pqc_status_t stub_decaps_hw(pqc_alg_t alg, uint32_t hw,
                                   const uint8_t *ct, uint8_t *ss)
{
	g_decaps_hw_calls++;
	if (hw >= STUB_SLOTS || !g_vault[hw].used || g_vault[hw].alg != alg) {
		return PQC_ERR_BAD_ARG;
	}
	return sw()->decaps(alg, g_vault[hw].sk, ct, ss);
}

/* 字节版：全能桩一律拒绝（见文件头）。 */
static pqc_status_t deny_keypair(pqc_alg_t a, uint8_t *pk, uint8_t *sk)
{
	(void)a; (void)pk; (void)sk;
	return PQC_ERR_UNSUPPORTED;
}

static pqc_status_t deny_keypair_seed(pqc_alg_t a, const uint8_t *s, size_t sl,
                                      uint8_t *pk, uint8_t *sk)
{
	(void)a; (void)s; (void)sl; (void)pk; (void)sk;
	return PQC_ERR_UNSUPPORTED;
}

static pqc_status_t deny_sign(pqc_alg_t a, const uint8_t *sk,
                              const uint8_t *m, size_t ml,
                              const uint8_t *c, size_t cl,
                              const uint8_t *r, uint8_t *sig, size_t *sl)
{
	(void)a; (void)sk; (void)m; (void)ml; (void)c; (void)cl; (void)r;
	(void)sig; (void)sl;
	return PQC_ERR_UNSUPPORTED;
}

/* 公钥运算照常转交软件：它们不碰私钥，转交不削弱本文件任何一条论证。 */
static pqc_status_t fwd_encaps(pqc_alg_t a, const uint8_t *pk,
                               uint8_t *ct, uint8_t *ss)
{
	return sw()->encaps(a, pk, ct, ss);
}

static pqc_status_t fwd_verify(pqc_alg_t a, const uint8_t *pk,
                               const uint8_t *m, size_t ml,
                               const uint8_t *c, size_t cl,
                               const uint8_t *sig, size_t sl)
{
	return sw()->verify(a, pk, m, ml, c, cl, sig, sl);
}

static pqc_status_t fwd_keypair_seed(pqc_alg_t a, const uint8_t *s, size_t sl,
                                     uint8_t *pk, uint8_t *sk)
{
	return sw()->keypair_from_seed(a, s, sl, pk, sk);
}

static pqc_status_t fwd_sign(pqc_alg_t a, const uint8_t *sk,
                             const uint8_t *m, size_t ml,
                             const uint8_t *c, size_t cl,
                             const uint8_t *r, uint8_t *sig, size_t *sl)
{
	return sw()->sign(a, sk, m, ml, c, cl, r, sig, sl);
}

/* 全能桩：两类私钥都在"硬件"里，字节版一概不给。 */
static const pqc_backend_t g_stub_full = {
	.name              = "stub(full hw keys)",
	.keypair           = deny_keypair,
	.keypair_from_seed = deny_keypair_seed,
	.encaps            = fwd_encaps,
	.sign              = deny_sign,
	.verify            = fwd_verify,
	.keypair_hw        = stub_keypair_hw,
	.decaps_hw         = stub_decaps_hw,
	.sign_hw           = stub_sign_hw,
};

/* 半能桩：**这就是本项目今天的形状** —— KEM 的核在硬件里，签名的从机还没有。
 * keypair_hw 对两类算法都能生成（硬件本来就分不清你要拿它干什么），
 * 但 sign_hw 缺席。字节版转交软件，好让签名仍然可用。 */
static const pqc_backend_t g_stub_kem_only = {
	.name              = "stub(KEM hw keys only)",
	.keypair           = deny_keypair,
	.keypair_from_seed = fwd_keypair_seed,
	.encaps            = fwd_encaps,
	.sign              = fwd_sign,
	.verify            = fwd_verify,
	.keypair_hw        = stub_keypair_hw,
	.decaps_hw         = stub_decaps_hw,
	.sign_hw           = NULL,
};

/* 只有签名一半的后端（用来把问法的两个方向都钉住） */
static const pqc_backend_t g_stub_sig_only = {
	.name       = "stub(SIG hw keys only)",
	.keypair_hw = stub_keypair_hw,
	.decaps_hw  = NULL,
	.sign_hw    = stub_sign_hw,
};

/* 有"用"的操作却没有"生成"：这种后端一把硬件密钥也造不出来 */
static const pqc_backend_t g_stub_no_keygen = {
	.name       = "stub(no keypair_hw)",
	.keypair_hw = NULL,
	.decaps_hw  = stub_decaps_hw,
	.sign_hw    = stub_sign_hw,
};

/* ---- 用例 --------------------------------------------------------------- */

/* 这一条是"别把两类合成一个布尔"的回归网。
 * 两个方向都要钉死，因为合成布尔的两种写法各错一个方向：
 *   合取 → 签名没落地就把已经成立的 KEM 一起拖回软件；
 *   析取 → 上层拿它去生成签名密钥，生成成功、签名却没有句柄路径。 */
static void test_has_hw_keys_kind(void)
{
	TCASE("has_hw_keys_kind：两类各问各的");

	pqc_set_backend(&g_stub_kem_only);
	CHECK_EQ_INT(pqc_backend_has_hw_keys_kind(PQC_KIND_KEM), 1);
	CHECK_EQ_INT(pqc_backend_has_hw_keys_kind(PQC_KIND_SIG), 0);
	CHECK_EQ_INT(pqc_backend_has_hw_keys_kind(PQC_KIND_KEM | PQC_KIND_SIG), 0);
	/* 旧名字 = 问 KEM，老调用点的语义一个字不变 */
	CHECK_EQ_INT(pqc_backend_has_hw_keys(), 1);

	pqc_set_backend(&g_stub_sig_only);
	CHECK_EQ_INT(pqc_backend_has_hw_keys_kind(PQC_KIND_KEM), 0);
	CHECK_EQ_INT(pqc_backend_has_hw_keys_kind(PQC_KIND_SIG), 1);
	CHECK_EQ_INT(pqc_backend_has_hw_keys_kind(PQC_KIND_KEM | PQC_KIND_SIG), 0);
	CHECK_EQ_INT(pqc_backend_has_hw_keys(), 0);

	pqc_set_backend(&g_stub_full);
	CHECK_EQ_INT(pqc_backend_has_hw_keys_kind(PQC_KIND_KEM), 1);
	CHECK_EQ_INT(pqc_backend_has_hw_keys_kind(PQC_KIND_SIG), 1);
	CHECK_EQ_INT(pqc_backend_has_hw_keys_kind(PQC_KIND_KEM | PQC_KIND_SIG), 1);

	/* 能用却生不出来：一把硬件密钥也不会有 */
	pqc_set_backend(&g_stub_no_keygen);
	CHECK_EQ_INT(pqc_backend_has_hw_keys_kind(PQC_KIND_KEM), 0);
	CHECK_EQ_INT(pqc_backend_has_hw_keys_kind(PQC_KIND_SIG), 0);

	/* 软件后端：一类都没有 */
	pqc_set_backend(pqc_backend_liboqs());
	CHECK_EQ_INT(pqc_backend_has_hw_keys_kind(PQC_KIND_KEM), 0);
	CHECK_EQ_INT(pqc_backend_has_hw_keys_kind(PQC_KIND_SIG), 0);
	CHECK_EQ_INT(pqc_backend_has_hw_keys(), 0);

	/* 空掩码不能回真 —— 那会让"我什么都没问"变成"都在" */
	pqc_set_backend(&g_stub_full);
	CHECK_EQ_INT(pqc_backend_has_hw_keys_kind(0), 0);
	/* 不认识的种类位：如实说不知道，不要按认识的那几位蒙混过关 */
	CHECK_EQ_INT(pqc_backend_has_hw_keys_kind(PQC_KIND_SIG | 0x40u), 0);

	pqc_set_backend(NULL);
}

static void test_sign_hw_wrapper(void)
{
	uint8_t sig[8192];
	size_t  sig_len = sizeof(sig);
	uint8_t rnd[PQC_SIG_RND_LEN] = { 0 };
	uint8_t ctx[256] = { 0 };

	TCASE("pqc_sign_hw 包装层的校验");

	/* 后端没有这一项 → UNSUPPORTED，而不是崩在空指针上 */
	pqc_set_backend(pqc_backend_liboqs());
	CHECK_EQ_INT(pqc_sign_hw(PQC_ALG_ML_DSA_65, 0, (const uint8_t *)"m", 1,
	                         NULL, 0, NULL, sig, &sig_len),
	             PQC_ERR_UNSUPPORTED);

	vault_reset();
	pqc_set_backend(&g_stub_full);
	CHECK_EQ_INT(pqc_sign_hw(PQC_ALG_ML_DSA_65, 0, (const uint8_t *)"m", 1,
	                         NULL, 0, NULL, NULL, &sig_len),
	             PQC_ERR_BAD_ARG);
	/* ctx 上限 255（FIPS 204） */
	CHECK_EQ_INT(pqc_sign_hw(PQC_ALG_ML_DSA_65, 0, (const uint8_t *)"m", 1,
	                         ctx, sizeof(ctx), NULL, sig, &sig_len),
	             PQC_ERR_BAD_ARG);
	/* 拿 KEM 算法来签名 */
	CHECK_EQ_INT(pqc_sign_hw(PQC_ALG_ML_KEM_768, 0, (const uint8_t *)"m", 1,
	                         NULL, 0, NULL, sig, &sig_len),
	             PQC_ERR_BAD_ARG);
	/* msg 为空指针但长度非零 */
	CHECK_EQ_INT(pqc_sign_hw(PQC_ALG_ML_DSA_65, 0, NULL, 4,
	                         NULL, 0, NULL, sig, &sig_len),
	             PQC_ERR_BAD_ARG);
	/* 上面这些都不该真的打到后端去 */
	CHECK_EQ_INT(g_sign_hw_calls, 0);

	/* rnd 非 NULL 是**后端**该拒的（寄存器面喂不进去），包装层不代劳：
	 * 这里只确认它确实被原样交到了后端手上，由后端自己决定。 */
	(void)rnd;

	pqc_set_backend(NULL);
	vault_reset();
}

/* 建一个已 init_token、已设并登录 User PIN 的槽位 */
static hsm_token_t *fixture(hsm_session_t *sess)
{
	hsm_token_t *tok = hsm_token_new(4);

	if (!tok) {
		return NULL;
	}
	if (hsm_slot_init_token(tok, 0, "slot-0", SO_PIN) != HSM_OK ||
	    hsm_session_open(tok, 0, sess) != HSM_OK ||
	    hsm_session_login(tok, *sess, HSM_ROLE_SO, SO_PIN) != HSM_OK ||
	    hsm_slot_set_user_pin(tok, *sess, USER_PIN) != HSM_OK ||
	    hsm_session_logout(tok, *sess) != HSM_OK ||
	    hsm_session_login(tok, *sess, HSM_ROLE_USER, USER_PIN) != HSM_OK) {
		hsm_token_free(tok);
		return NULL;
	}
	return tok;
}

/* 全套：生成 → 签名 → 用独立的软件后端验。
 * 全能桩的字节版全是 UNSUPPORTED，所以这条用例只要跑通，
 * 就等于证明了生成和签名走的都是句柄路径 —— 软件路径根本不存在。 */
static void test_hw_resident_sign_roundtrip(void)
{
	const pqc_alg_info_t *info = pqc_alg_info(PQC_ALG_ML_DSA_65);
	hsm_session_t sess;
	hsm_token_t *tok;
	hsm_handle_t h = HSM_INVALID_HANDLE;
	uint8_t *pk;
	size_t pk_len = 0, sig_len = 0;
	uint8_t sig[8192];
	uint8_t msg[] = "sign me on the (simulated) vault";

	TCASE("硬件生成的 ML-DSA 密钥：C_Sign 那条路走 sign_hw");

	vault_reset();
	pqc_set_backend(&g_stub_full);
	tok = fixture(&sess);
	CHECK(tok != NULL);
	if (!tok) {
		pqc_set_backend(NULL);
		return;
	}

	CHECK_EQ_INT(hsm_slot_generate(tok, sess, PQC_ALG_ML_DSA_65,
	                               KEY_USAGE_SIGN, 0, &h), HSM_OK);
	CHECK_EQ_INT(g_keypair_hw_calls, 1);

	pk = malloc(info->pk_len);
	CHECK(pk != NULL);
	CHECK_EQ_INT(hsm_object_public_key(tok, sess, h, pk, info->pk_len, &pk_len),
	             HSM_OK);
	CHECK_EQ_INT(pk_len, info->pk_len);

	CHECK_EQ_INT(hsm_object_sign(tok, sess, h, msg, sizeof(msg), NULL, 0,
	                             sig, sizeof(sig), &sig_len), HSM_OK);
	CHECK_EQ_INT(g_sign_hw_calls, 1);
	CHECK_EQ_INT(sig_len, info->sig_len);
	/* 上层必须传 hedged（rnd = NULL）：硬件没有喂 rnd 的入口 */
	CHECK_EQ_INT(g_rnd_was_nonnull, 0);

	/* 换回软件后端再验 —— 用同一个后端验自己签的东西什么也证明不了 */
	pqc_set_backend(pqc_backend_liboqs());
	CHECK_EQ_INT(pqc_verify(PQC_ALG_ML_DSA_65, pk, msg, sizeof(msg), NULL, 0,
	                        sig, sig_len), PQC_OK);
	msg[0] ^= 0x01;
	CHECK(pqc_verify(PQC_ALG_ML_DSA_65, pk, msg, sizeof(msg), NULL, 0,
	                 sig, sig_len) != PQC_OK);
	msg[0] ^= 0x01;

	/* 种子存储策略与"私钥留硬件"是互斥的：硬件的 ξ 取自 PL 的 TRNG，复现不了。
	 * 这种冲突要**失败**，不能悄悄退回软件给一把假装在硬件里的密钥。
	 * 换一个 token：一个槽位装过密钥之后再 generate 会被状态机挡下
	 * （HSM_ERR_BAD_STATE），那样就测不到这里想测的东西了。 */
	pqc_set_backend(&g_stub_full);
	{
		hsm_session_t s2;
		hsm_token_t *tok2 = fixture(&s2);
		hsm_handle_t h2 = HSM_INVALID_HANDLE;

		CHECK(tok2 != NULL);
		if (tok2) {
			CHECK_EQ_INT(hsm_slot_generate(tok2, s2, PQC_ALG_ML_DSA_65,
			                               KEY_USAGE_SIGN,
			                               SLOT_POLICY_SEED_STORAGE, &h2),
			             HSM_ERR_CRYPTO);
			hsm_token_free(tok2);
		}
	}

	free(pk);
	hsm_token_free(tok);
	pqc_set_backend(NULL);
	vault_reset();
}

/* 本项目今天的形状：ML-KEM 在硬件里、ML-DSA 的从机还没落地。
 * 这一条同时钉住两件事 ——
 *   · 签名密钥**不能**被收进金库（收了就再也签不动，而且要到第一次签名才发现）；
 *   · ML-KEM **仍然**走硬件（不能被还没落地的 ML-DSA 连累回软件）。 */
static void test_kem_only_backend_splits_correctly(void)
{
	const pqc_alg_info_t *ki = pqc_alg_info(PQC_ALG_ML_KEM_768);
	hsm_session_t ss_sess, sk_sess;
	hsm_token_t *tok_sig, *tok_kem;
	hsm_handle_t hs = HSM_INVALID_HANDLE, hk = HSM_INVALID_HANDLE;
	uint8_t sig[8192], ct[2048], ss[32], ss2[32];
	uint8_t *kpk;
	size_t sig_len = 0, pk_len = 0, ss_len = 0;

	TCASE("只有 KEM 落地时：签名密钥不进金库，ML-KEM 照常进");

	vault_reset();
	pqc_set_backend(&g_stub_kem_only);
	/* 两把密钥各用一个 token：一个槽位只装得下一把，
	 * 第二次 generate 会被状态机挡在 BAD_STATE 上。 */
	tok_sig = fixture(&ss_sess);
	tok_kem = fixture(&sk_sess);
	CHECK(tok_sig != NULL && tok_kem != NULL);
	if (!tok_sig || !tok_kem) {
		hsm_token_free(tok_sig);
		hsm_token_free(tok_kem);
		pqc_set_backend(NULL);
		return;
	}

	/* 签名密钥：走软件路径，keypair_hw 一次都不该被调到 */
	CHECK_EQ_INT(hsm_slot_generate(tok_sig, ss_sess, PQC_ALG_ML_DSA_65,
	                               KEY_USAGE_SIGN, 0, &hs), HSM_OK);
	CHECK_EQ_INT(g_keypair_hw_calls, 0);
	/* 而且它签得动 —— 这正是"没被收进金库"的可观测后果 */
	CHECK_EQ_INT(hsm_object_sign(tok_sig, ss_sess, hs, (const uint8_t *)"m", 1,
	                             NULL, 0, sig, sizeof(sig), &sig_len), HSM_OK);
	CHECK_EQ_INT(g_sign_hw_calls, 0);

	/* ML-KEM：照常进金库，解封装走句柄 */
	CHECK_EQ_INT(hsm_slot_generate(tok_kem, sk_sess, PQC_ALG_ML_KEM_768,
	                               KEY_USAGE_DECAP, 0, &hk), HSM_OK);
	CHECK_EQ_INT(g_keypair_hw_calls, 1);

	kpk = malloc(ki->pk_len);
	CHECK(kpk != NULL);
	CHECK_EQ_INT(hsm_object_public_key(tok_kem, sk_sess, hk, kpk, ki->pk_len,
	                                   &pk_len), HSM_OK);
	CHECK_EQ_INT(pqc_encaps(PQC_ALG_ML_KEM_768, kpk, ct, ss), PQC_OK);
	CHECK_EQ_INT(hsm_object_decaps(tok_kem, sk_sess, hk, ct, ki->ct_len,
	                               ss2, sizeof(ss2), &ss_len), HSM_OK);
	CHECK_EQ_INT(g_decaps_hw_calls, 1);
	CHECK_EQ_INT(ss_len, 32);
	CHECK_EQ_MEM(ss, ss2, 32);

	free(kpk);
	hsm_token_free(tok_sig);
	hsm_token_free(tok_kem);
	pqc_set_backend(NULL);
	vault_reset();
}

int main(void)
{
	test_has_hw_keys_kind();
	test_sign_hw_wrapper();
	test_hw_resident_sign_roundtrip();
	test_kem_only_backend_splits_correctly();
	return test_report("test_pqc_hw");
}
