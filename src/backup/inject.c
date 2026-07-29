#include "pqchsm/inject.h"

#include "pqchsm/kdf.h"
#include "pqchsm/util.h"
#include "pqchsm/wrap.h"

#include <stdlib.h>
#include <string.h>

static const uint8_t IJ_MAGIC[8] = { 'P', 'Q', 'C', 'H', 'S', 'M', 'I', 'J' };

/* 头部布局（同时是 GCM 的 AAD，所以改任何一个字段都会让解包失败） */
#define OFF_MAGIC   0u
#define OFF_VERSION 8u
#define OFF_KEM_ALG 12u
#define OFF_KEY_ALG 16u
#define OFF_USAGE   20u
#define OFF_POLICY  24u
#define OFF_CT_LEN  28u
#define IJ_HDR_LEN  32u

static void put_u32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++) {
		p[i] = (uint8_t)(v >> (8 * i));
	}
}

static uint32_t get_u32(const uint8_t *p)
{
	uint32_t v = 0;
	for (int i = 0; i < 4; i++) {
		v |= (uint32_t)p[i] << (8 * i);
	}
	return v;
}

size_t hsm_inject_blob_len(pqc_alg_t kem_alg, size_t seed_len)
{
	const pqc_alg_info_t *k = pqc_alg_info(kem_alg);
	if (!k || k->kind != PQC_KIND_KEM) {
		return 0;
	}
	return IJ_HDR_LEN + k->ct_len + pqc_wrap_blob_len(seed_len);
}

/* CEK 由 KEM 的共享秘密再派生一层：共享秘密直接当 AES 密钥用也不是不行，
 * 但多一层域分隔可以让同一个共享秘密将来还能派生别的用途，且明确"这是注入用的"。 */
static int derive_cek(const uint8_t *ss, size_t ss_len, uint8_t cek[PQC_KEK_LEN])
{
	return pqc_kdf(ss, ss_len, NULL, 0, "pqc-hsm/inject-cek", cek, PQC_KEK_LEN);
}

hsm_status_t hsm_inject_build(pqc_alg_t kem_alg,
                              const uint8_t *device_pk, size_t device_pk_len,
                              pqc_alg_t key_alg, uint32_t usage, uint32_t policy,
                              const uint8_t *seed, size_t seed_len,
                              uint8_t *blob, size_t cap, size_t *blob_len)
{
	const pqc_alg_info_t *kem = pqc_alg_info(kem_alg);
	const pqc_alg_info_t *key = pqc_alg_info(key_alg);
	if (!kem || kem->kind != PQC_KIND_KEM || !key || !device_pk || !seed || !blob ||
	    !blob_len) {
		return HSM_ERR_BAD_ARG;
	}
	if (device_pk_len != kem->pk_len || seed_len != key->seed_len) {
		return HSM_ERR_BAD_ARG;
	}
	size_t need = hsm_inject_blob_len(kem_alg, seed_len);
	if (cap < need) {
		return HSM_ERR_BAD_ARG;
	}

	uint8_t *hdr = blob;
	memcpy(hdr + OFF_MAGIC, IJ_MAGIC, 8);
	put_u32(hdr + OFF_VERSION, HSM_INJECT_VERSION);
	put_u32(hdr + OFF_KEM_ALG, (uint32_t)kem_alg);
	put_u32(hdr + OFF_KEY_ALG, (uint32_t)key_alg);
	put_u32(hdr + OFF_USAGE, usage);
	put_u32(hdr + OFF_POLICY, policy);
	put_u32(hdr + OFF_CT_LEN, (uint32_t)kem->ct_len);

	uint8_t ss[64], cek[PQC_KEK_LEN];
	hsm_status_t st = HSM_ERR_CRYPTO;
	/* 一次性 CEK：封装产生，用完即弃。注入端不保存它。 */
	if (pqc_encaps(kem_alg, device_pk, blob + IJ_HDR_LEN, ss) != PQC_OK) {
		goto out;
	}
	if (derive_cek(ss, kem->ss_len, cek) != 0) {
		goto out;
	}
	{
		size_t wlen = 0;
		/* 头进 AAD：改 usage / policy / 算法都会让设备端解包失败 */
		if (pqc_wrap(cek, sizeof(cek), hdr, IJ_HDR_LEN, seed, seed_len,
		             blob + IJ_HDR_LEN + kem->ct_len,
		             cap - IJ_HDR_LEN - kem->ct_len, &wlen) != 0) {
			goto out;
		}
		*blob_len = IJ_HDR_LEN + kem->ct_len + wlen;
		st = HSM_OK;
	}
out:
	pqc_secure_zero(ss, sizeof(ss));
	pqc_secure_zero(cek, sizeof(cek));
	return st;
}

hsm_status_t hsm_inject_apply(hsm_token_t *tok,
                              hsm_session_t kem_sess, hsm_handle_t kem_handle,
                              hsm_session_t target_sess,
                              const uint8_t *blob, size_t blob_len,
                              hsm_handle_t *out)
{
	if (!tok || !blob || blob_len < IJ_HDR_LEN) {
		return HSM_ERR_BAD_ARG;
	}
	if (memcmp(blob, IJ_MAGIC, 8) != 0 || get_u32(blob + OFF_VERSION) != HSM_INJECT_VERSION) {
		return HSM_ERR_INTEGRITY;
	}
	pqc_alg_t kem_alg = (pqc_alg_t)get_u32(blob + OFF_KEM_ALG);
	pqc_alg_t key_alg = (pqc_alg_t)get_u32(blob + OFF_KEY_ALG);
	uint32_t  usage   = get_u32(blob + OFF_USAGE);
	uint32_t  policy  = get_u32(blob + OFF_POLICY);
	size_t    ct_len  = get_u32(blob + OFF_CT_LEN);

	const pqc_alg_info_t *kem = pqc_alg_info(kem_alg);
	const pqc_alg_info_t *key = pqc_alg_info(key_alg);
	if (!kem || kem->kind != PQC_KIND_KEM || !key) {
		return HSM_ERR_INTEGRITY;
	}
	if (ct_len != kem->ct_len || blob_len < IJ_HDR_LEN + ct_len) {
		return HSM_ERR_INTEGRITY;
	}

	/* 目标槽位已装载时，必须显式允许被注入更新（§7.2 的策略位） */
	{
		hsm_slot_id_t tslot = 0;
		hsm_role_t role;
		hsm_status_t rs = hsm_session_role(tok, target_sess, &role);
		if (rs != HSM_OK) {
			return rs;
		}
		/* 通过一次 get_meta 拿到目标槽位状态：会话所属槽位由 slot 层校验，
		 * 这里遍历找出会话对应的槽位并不划算，改为让 generate/load 自己报状态错。
		 * 但"已装载且不可注入"这条必须在装载前判掉，所以用下面的探测方式。 */
		for (size_t i = 0; i < hsm_token_slot_count(tok); i++) {
			slot_meta_t m;
			if (hsm_slot_get_meta(tok, (hsm_slot_id_t)i, &m) != HSM_OK) {
				continue;
			}
			if (m.state != SLOT_ST_LOADED && m.state != SLOT_ST_IN_USE) {
				continue;
			}
			hsm_handle_t h = ((hsm_handle_t)m.generation << 32) | (hsm_handle_t)(i + 1);
			uint8_t probe[8];
			size_t plen = 0;
			/* 能读到公钥 = 这个槽位属于 target_sess */
			if (hsm_object_public_key(tok, target_sess, h, probe, sizeof(probe),
			                          &plen) == HSM_ERR_BAD_HANDLE) {
				continue;
			}
			tslot = (hsm_slot_id_t)i;
			if (!(m.policy & SLOT_POLICY_INJECTABLE)) {
				return HSM_ERR_POLICY;
			}
			/* 允许更新：先销毁旧对象，槽位回到 EMPTY */
			hsm_status_t ds = hsm_object_destroy(tok, target_sess, h);
			if (ds != HSM_OK) {
				return ds;
			}
			(void)tslot;
			break;
		}
	}

	uint8_t ss[64], cek[PQC_KEK_LEN], seed[64];
	size_t ss_len = 0, seed_len = 0;
	hsm_status_t st = HSM_ERR_INTEGRITY;

	/* 解封装在设备内完成：CEK 从不出现在链路上 */
	st = hsm_object_decaps(tok, kem_sess, kem_handle, blob + IJ_HDR_LEN, ct_len,
	                       ss, sizeof(ss), &ss_len);
	if (st != HSM_OK) {
		goto out;
	}
	if (derive_cek(ss, ss_len, cek) != 0) {
		st = HSM_ERR_CRYPTO;
		goto out;
	}
	if (pqc_unwrap(cek, sizeof(cek), blob, IJ_HDR_LEN,
	               blob + IJ_HDR_LEN + ct_len, blob_len - IJ_HDR_LEN - ct_len,
	               seed, sizeof(seed), &seed_len) != 0) {
		/* 解包失败：可能是 blob 被改、也可能是拿错了注入钥 */
		st = HSM_ERR_INTEGRITY;
		goto out;
	}
	if (seed_len != key->seed_len) {
		st = HSM_ERR_INTEGRITY;
		goto out;
	}
	/* 种子直接装进目标槽位 —— 明文密钥从未离开设备 */
	st = hsm_slot_load_seed(tok, target_sess, key_alg, usage, policy,
	                        seed, seed_len, out);
out:
	pqc_secure_zero(ss, sizeof(ss));
	pqc_secure_zero(cek, sizeof(cek));
	pqc_secure_zero(seed, sizeof(seed));
	return st;
}

/* ---- 制造模式（桩）------------------------------------------------------ */

static int g_mfg_mode = 0;
static int g_mfg_blown = 0;   /* 模拟 eFUSE 熔断：置位后永远回不去 */

int hsm_inject_set_manufacturing_mode(int on)
{
	if (on) {
		if (g_mfg_blown) {
			return -1;   /* 已熔断，不可再打开 */
		}
		g_mfg_mode = 1;
		return 0;
	}
	g_mfg_mode = 0;
	g_mfg_blown = 1;
	return 0;
}

int hsm_inject_manufacturing_mode(void)
{
	return g_mfg_mode;
}

hsm_status_t hsm_inject_plaintext(hsm_token_t *tok, hsm_session_t target_sess,
                                  pqc_alg_t key_alg, uint32_t usage, uint32_t policy,
                                  const uint8_t *seed, size_t seed_len,
                                  hsm_handle_t *out)
{
	if (!g_mfg_mode) {
		/* §8.5：明文注入只允许在制造模式下用 */
		return HSM_ERR_POLICY;
	}
	return hsm_slot_load_seed(tok, target_sess, key_alg, usage, policy,
	                          seed, seed_len, out);
}
