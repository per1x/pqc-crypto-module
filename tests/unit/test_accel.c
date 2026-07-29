/* Level B 的核心断言：**把后端换成寄存器接口，上层一行不改还能跑**
 *
 * 换后端只有一句 pqc_set_backend(pqc_backend_accel())，
 * 之后槽位管理器、密钥库、备份恢复全都照常工作 —— 这就是 §5.7.1 说的
 * "真板到手后把 accel_stub.c 换成 accel_mmap.c，上层一行不改"。
 *
 * 这里同时验证两件事：
 *   1. 寄存器握手协议本身对（写 MODE/送数据/START/轮询 DONE/读结果）；
 *   2. 走这条路算出来的结果与直接调 liboqs **逐字节相同** ——
 *      否则"能跑"只是看起来能跑。
 */
#include "testlib.h"
#include "pqchsm/accel.h"
#include "pqchsm/backup.h"
#include "pqchsm/keystore.h"
#include "pqchsm/slot.h"
#include "pqchsm/util.h"

#include <stdlib.h>
#include <unistd.h>

#define SO_PIN   "so-secret-0001"
#define USER_PIN "user-pin-4242"

/* 同一个种子、同一份输入，两个后端算出来必须逐字节相同 */
static void test_backend_equivalence(void)
{
	TCASE("两个后端逐字节等价：keygen_from_seed");
	const pqc_alg_t algs[] = {
		PQC_ALG_ML_KEM_512, PQC_ALG_ML_KEM_768, PQC_ALG_ML_KEM_1024,
		PQC_ALG_ML_DSA_44,  PQC_ALG_ML_DSA_65,  PQC_ALG_ML_DSA_87,
	};
	for (size_t i = 0; i < sizeof(algs) / sizeof(algs[0]); i++) {
		const pqc_alg_info_t *info = pqc_alg_info(algs[i]);
		uint8_t seed[64];
		for (size_t k = 0; k < sizeof(seed); k++) {
			seed[k] = (uint8_t)(k * 5 + i);
		}
		uint8_t *pk_a = malloc(info->pk_len), *sk_a = malloc(info->sk_len);
		uint8_t *pk_b = malloc(info->pk_len), *sk_b = malloc(info->sk_len);

		pqc_set_backend(pqc_backend_liboqs());
		CHECK_EQ_INT(pqc_keypair_from_seed(algs[i], seed, info->seed_len, pk_a, sk_a),
		             PQC_OK);
		pqc_set_backend(pqc_backend_accel());
		CHECK_EQ_INT(pqc_keypair_from_seed(algs[i], seed, info->seed_len, pk_b, sk_b),
		             PQC_OK);
		CHECK_EQ_MEM(pk_a, pk_b, info->pk_len);
		CHECK_EQ_MEM(sk_a, sk_b, info->sk_len);

		free(pk_a); free(sk_a); free(pk_b); free(sk_b);
	}

	TCASE("两个后端逐字节等价：encaps_derand / decaps");
	{
		const pqc_alg_info_t *info = pqc_alg_info(PQC_ALG_ML_KEM_768);
		uint8_t seed[64], m[32];
		memset(seed, 0x31, sizeof(seed));
		memset(m, 0x7e, sizeof(m));
		uint8_t *pk = malloc(info->pk_len), *sk = malloc(info->sk_len);
		uint8_t *ct_a = malloc(info->ct_len), *ct_b = malloc(info->ct_len);
		uint8_t ss_a[64], ss_b[64], ss_d[64];

		pqc_set_backend(pqc_backend_liboqs());
		CHECK_EQ_INT(pqc_keypair_from_seed(PQC_ALG_ML_KEM_768, seed, 64, pk, sk), PQC_OK);
		CHECK_EQ_INT(pqc_encaps_derand(PQC_ALG_ML_KEM_768, pk, m, 32, ct_a, ss_a), PQC_OK);

		pqc_set_backend(pqc_backend_accel());
		CHECK_EQ_INT(pqc_encaps_derand(PQC_ALG_ML_KEM_768, pk, m, 32, ct_b, ss_b), PQC_OK);
		CHECK_EQ_MEM(ct_a, ct_b, info->ct_len);
		CHECK_EQ_MEM(ss_a, ss_b, info->ss_len);
		/* 用寄存器接口解封装，应当解回同一个共享秘密 */
		CHECK_EQ_INT(pqc_decaps(PQC_ALG_ML_KEM_768, sk, ct_a, ss_d), PQC_OK);
		CHECK_EQ_MEM(ss_a, ss_d, info->ss_len);

		free(pk); free(sk); free(ct_a); free(ct_b);
	}

	TCASE("两个后端逐字节等价：deterministic 签名（rnd = 0^32）");
	{
		const pqc_alg_info_t *info = pqc_alg_info(PQC_ALG_ML_DSA_65);
		uint8_t seed[32];
		memset(seed, 0x5c, sizeof(seed));
		uint8_t *pk = malloc(info->pk_len), *sk = malloc(info->sk_len);
		uint8_t *s_a = malloc(info->sig_len), *s_b = malloc(info->sig_len);
		size_t la = info->sig_len, lb = info->sig_len;
		const uint8_t msg[] = "accel-vs-liboqs";
		const uint8_t ctx[] = { 9, 8, 7 };
		/* ⚠️ 注意用非全零的 rnd：寄存器接口把全 0 约定成"自取 TRNG"，
		 * 那样两次结果本来就不该相同。这里用一个显式的非零 rnd。 */
		uint8_t rnd[PQC_SIG_RND_LEN];
		memset(rnd, 0xA7, sizeof(rnd));

		pqc_set_backend(pqc_backend_liboqs());
		CHECK_EQ_INT(pqc_keypair_from_seed(PQC_ALG_ML_DSA_65, seed, 32, pk, sk), PQC_OK);
		CHECK_EQ_INT(pqc_sign(PQC_ALG_ML_DSA_65, sk, msg, sizeof(msg), ctx, sizeof(ctx),
		                      rnd, s_a, &la), PQC_OK);

		pqc_set_backend(pqc_backend_accel());
		CHECK_EQ_INT(pqc_sign(PQC_ALG_ML_DSA_65, sk, msg, sizeof(msg), ctx, sizeof(ctx),
		                      rnd, s_b, &lb), PQC_OK);
		CHECK_EQ_INT(la, lb);
		CHECK_EQ_MEM(s_a, s_b, la);

		/* 验签也走寄存器接口 */
		CHECK_EQ_INT(pqc_verify(PQC_ALG_ML_DSA_65, pk, msg, sizeof(msg), ctx, sizeof(ctx),
		                        s_b, lb), PQC_OK);
		s_b[0] ^= 0x01;
		CHECK_EQ_INT(pqc_verify(PQC_ALG_ML_DSA_65, pk, msg, sizeof(msg), ctx, sizeof(ctx),
		                        s_b, lb), PQC_ERR_VERIFY);

		free(pk); free(sk); free(s_a); free(s_b);
	}

	TCASE("hedged 签名（rnd = NULL）经寄存器接口仍然可用且可验证");
	{
		const pqc_alg_info_t *info = pqc_alg_info(PQC_ALG_ML_DSA_44);
		uint8_t *pk = malloc(info->pk_len), *sk = malloc(info->sk_len);
		uint8_t *sig = malloc(info->sig_len);
		size_t sl = info->sig_len;
		const uint8_t msg[] = "hedged over registers";
		pqc_set_backend(pqc_backend_accel());
		CHECK_EQ_INT(pqc_keypair(PQC_ALG_ML_DSA_44, pk, sk), PQC_OK);
		CHECK_EQ_INT(pqc_sign(PQC_ALG_ML_DSA_44, sk, msg, sizeof(msg), NULL, 0, NULL,
		                      sig, &sl), PQC_OK);
		CHECK_EQ_INT(pqc_verify(PQC_ALG_ML_DSA_44, pk, msg, sizeof(msg), NULL, 0, sig, sl),
		             PQC_OK);
		free(pk); free(sk); free(sig);
	}
}

/* NTT 模式：寄存器接口直通，用来给 RTL 对拍（Verilator 后端只实现了这个） */
static void test_ntt_mode(void)
{
	TCASE("NTT 模式：正变换 → 逆变换 ≡ x·2^16 (mod q)");
	int16_t in[256], fwd[256], back[256];
	for (int i = 0; i < 256; i++) {
		in[i] = (int16_t)((i * 37) % 3329 - 1664);
	}
	CHECK_EQ_INT(accel_ntt(in, fwd, 0), 0);
	CHECK_EQ_INT(accel_ntt(fwd, back, 1), 0);
	int ok = 1;
	for (int i = 0; i < 256; i++) {
		/* 参考实现的 invntt 带 f = mont^2/128，往返得到 x·2^16 */
		int32_t lhs = ((int32_t)in[i] * 2285) % 3329;
		int32_t rhs = ((int32_t)back[i]) % 3329;
		if (((lhs - rhs) % 3329 + 3329) % 3329 != 0) {
			ok = 0;
		}
	}
	CHECK(ok);
	/* 正变换必须真的改变了系数 */
	CHECK(memcmp(in, fwd, sizeof(in)) != 0);
}

/* 整条 HSM 链在 accel 后端下再跑一遍 */
static void test_full_chain_on_accel(void)
{
	TCASE("整条链跑在 accel 后端上：生成→签名→备份→清零→恢复→验签");
	pqc_set_backend(pqc_backend_accel());

	char ks[160], bk[160];
	snprintf(ks, sizeof(ks), "/tmp/pqchsm_accel_%d.ks", (int)getpid());
	snprintf(bk, sizeof(bk), "/tmp/pqchsm_accel_%d.bk", (int)getpid());

	hsm_token_t *tok = hsm_token_new(2);
	CHECK(tok != NULL);
	hsm_session_t s;
	hsm_handle_t h;
	CHECK_EQ_INT(hsm_slot_init_token(tok, 0, "accel-slot", SO_PIN), HSM_OK);
	CHECK_EQ_INT(hsm_session_open(tok, 0, &s), HSM_OK);
	CHECK_EQ_INT(hsm_session_login(tok, s, HSM_ROLE_SO, SO_PIN), HSM_OK);
	CHECK_EQ_INT(hsm_slot_set_user_pin(tok, s, USER_PIN), HSM_OK);
	CHECK_EQ_INT(hsm_session_logout(tok, s), HSM_OK);
	CHECK_EQ_INT(hsm_session_login(tok, s, HSM_ROLE_USER, USER_PIN), HSM_OK);
	CHECK_EQ_INT(hsm_slot_generate(tok, s, PQC_ALG_ML_DSA_65, KEY_USAGE_SIGN,
	                               SLOT_POLICY_BACKUPABLE, &h), HSM_OK);

	const pqc_alg_info_t *info = pqc_alg_info(PQC_ALG_ML_DSA_65);
	uint8_t *pk = malloc(info->pk_len), *sig = malloc(info->sig_len);
	size_t n = 0, sl = 0;
	CHECK_EQ_INT(hsm_object_public_key(tok, s, h, pk, info->pk_len, &n), HSM_OK);
	const uint8_t msg[] = "full chain on the accelerator";
	CHECK_EQ_INT(hsm_object_sign(tok, s, h, msg, sizeof(msg), NULL, 0, sig, info->sig_len,
	                             &sl), HSM_OK);
	CHECK_EQ_INT(pqc_verify(PQC_ALG_ML_DSA_65, pk, msg, sizeof(msg), NULL, 0, sig, sl),
	             PQC_OK);
	CHECK_EQ_INT(hsm_keystore_save(tok, ks), HSM_OK);

	/* 备份 → 清零 → 恢复 */
	uint8_t shares[5 * HSM_SHARE_CAP];
	size_t lens[5];
	CHECK_EQ_INT(hsm_session_logout(tok, s), HSM_OK);
	CHECK_EQ_INT(hsm_session_login(tok, s, HSM_ROLE_SO, SO_PIN), HSM_OK);
	CHECK_EQ_INT(hsm_backup_export(tok, s, bk, 3, 5, shares, HSM_SHARE_CAP, lens, NULL),
	             HSM_OK);
	CHECK_EQ_INT(hsm_slot_zeroize_forced(tok, 0), HSM_OK);
	{
		uint8_t sel[3 * HSM_SHARE_CAP];
		size_t sl3[3];
		int idx[3] = { 0, 1, 4 };
		for (int i = 0; i < 3; i++) {
			memcpy(sel + (size_t)i * HSM_SHARE_CAP,
			       shares + (size_t)idx[i] * HSM_SHARE_CAP, HSM_SHARE_CAP);
			sl3[i] = lens[idx[i]];
		}
		CHECK_EQ_INT(hsm_backup_restore(tok, bk, sel, HSM_SHARE_CAP, sl3, 3, NULL), HSM_OK);
	}
	/* 恢复后用寄存器接口再签一次，必须被清零前的公钥验过 */
	{
		hsm_session_t s2;
		CHECK_EQ_INT(hsm_session_open(tok, 0, &s2), HSM_OK);
		CHECK_EQ_INT(hsm_session_login(tok, s2, HSM_ROLE_USER, USER_PIN), HSM_OK);
		slot_meta_t m;
		CHECK_EQ_INT(hsm_slot_get_meta(tok, 0, &m), HSM_OK);
		hsm_handle_t h2 = ((hsm_handle_t)m.generation << 32) | 1;
		size_t l2 = info->sig_len;
		CHECK_EQ_INT(hsm_object_sign(tok, s2, h2, msg, sizeof(msg), NULL, 0, sig,
		                             info->sig_len, &l2), HSM_OK);
		CHECK_EQ_INT(pqc_verify(PQC_ALG_ML_DSA_65, pk, msg, sizeof(msg), NULL, 0, sig, l2),
		             PQC_OK);
		hsm_session_close(tok, s2);
	}

	free(pk);
	free(sig);
	hsm_token_free(tok);
	unlink(ks);
	unlink(bk);
}

int main(void)
{
	TCASE("默认 transport 是软件桩，且明确标记为非硬件");
	const accel_transport_t *t = accel_get_transport();
	CHECK(t != NULL);
	CHECK(t == accel_transport_stub());
	CHECK_EQ_INT(t->is_hardware, 0);

	TCASE("Verilator transport：编了就能用，没编返回 NULL（如实反映）");
	{
		const accel_transport_t *v = accel_transport_verilator();
		if (v) {
			printf("      Verilator transport 可用：%s\n", v->name);
			CHECK_EQ_INT(v->is_hardware, 0);   /* 仿真也不是真硬件 */
		} else {
			printf("      Verilator transport 未编入（需要 verilator）\n");
			CHECK(1);
		}
	}

	/* ★ Level B 的核心断言：真 RTL 与软件桩，经**同一个寄存器接口**，结果逐字节相同 */
	{
		const accel_transport_t *v = accel_transport_verilator();
		if (v) {
			TCASE("★ RTL(Verilator) 与软件桩的 NTT 结果逐字节相同");
			int16_t in[256], sw_f[256], rtl_f[256], sw_i[256], rtl_i[256];
			for (int i = 0; i < 256; i++) {
				in[i] = (int16_t)((i * 37 + 11) % 3329 - 1664);
			}
			accel_set_transport(accel_transport_stub());
			CHECK_EQ_INT(accel_ntt(in, sw_f, 0), 0);
			CHECK_EQ_INT(accel_ntt(in, sw_i, 1), 0);

			accel_set_transport(v);
			CHECK_EQ_INT(accel_ntt(in, rtl_f, 0), 0);
			CHECK_EQ_INT(accel_ntt(in, rtl_i, 1), 0);
			CHECK_EQ_MEM(sw_f, rtl_f, sizeof(sw_f));
			CHECK_EQ_MEM(sw_i, rtl_i, sizeof(sw_i));

			TCASE("RTL 后端对未实现的模式明确报 UNSUPPORTED（不偷偷回落软件）");
			{
				pqc_set_backend(pqc_backend_accel());
				const pqc_alg_info_t *ki = pqc_alg_info(PQC_ALG_ML_KEM_768);
				uint8_t seed[64] = { 0 };
				uint8_t *pk = malloc(ki->pk_len), *sk = malloc(ki->sk_len);
				CHECK_EQ_INT(pqc_keypair_from_seed(PQC_ALG_ML_KEM_768, seed, 64, pk, sk),
				             PQC_ERR_UNSUPPORTED);
				free(pk);
				free(sk);
				pqc_set_backend(NULL);
			}
			accel_set_transport(accel_transport_stub());
		}
	}

	test_backend_equivalence();
	test_ntt_mode();
	test_full_chain_on_accel();

	pqc_set_backend(NULL);   /* 恢复默认，免得影响同进程后续 */
	return test_report("test_accel");
}
