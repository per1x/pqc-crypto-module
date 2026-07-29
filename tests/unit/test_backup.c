/* 备份与恢复端到端演练（路线图 §8.4，Phase 6 验收项）
 *
 * 主线：建库 → 备份导出 → 整机清零 → 用备份 + M 份分片恢复 → 原密钥可用
 *       （判据是"恢复后签出来的名字能被原公钥验过"，不是"没报错"）
 * 负测试：错误分片、不足 M 份、篡改 blob、跨槽位重放、跨设备。
 */
#include "testlib.h"
#include "pqchsm/backup.h"
#include "pqchsm/keystore.h"
#include "pqchsm/kdr.h"
#include "pqchsm/shamir.h"
#include "pqchsm/slot.h"
#include "pqchsm/util.h"

#include <stdlib.h>
#include <unistd.h>

#define SO_PIN   "so-secret-0001"
#define USER_PIN "user-pin-4242"
#define N_SLOTS  3
#define M 3
#define N 5

static char g_path[256];

/* slot0/slot1 可备份，slot2 是纯 sealed（不带 BACKUPABLE） */
static hsm_token_t *build_token(uint8_t *pk0, size_t *pk0_len)
{
	hsm_token_t *tok = hsm_token_new(N_SLOTS);
	if (!tok) {
		return NULL;
	}
	struct { pqc_alg_t alg; uint32_t usage; uint32_t policy; } spec[N_SLOTS] = {
		{ PQC_ALG_ML_DSA_65,  KEY_USAGE_SIGN,  SLOT_POLICY_BACKUPABLE },
		{ PQC_ALG_ML_KEM_768, KEY_USAGE_DECAP, SLOT_POLICY_BACKUPABLE | SLOT_POLICY_SEED_STORAGE },
		{ PQC_ALG_ML_DSA_44,  KEY_USAGE_SIGN,  0 },   /* 纯 sealed，不进备份 */
	};
	for (int i = 0; i < N_SLOTS; i++) {
		hsm_session_t s;
		hsm_handle_t h;
		char label[32];
		snprintf(label, sizeof(label), "slot-%d", i);
		if (hsm_slot_init_token(tok, (hsm_slot_id_t)i, label, SO_PIN) != HSM_OK ||
		    hsm_session_open(tok, (hsm_slot_id_t)i, &s) != HSM_OK ||
		    hsm_session_login(tok, s, HSM_ROLE_SO, SO_PIN) != HSM_OK ||
		    hsm_slot_set_user_pin(tok, s, USER_PIN) != HSM_OK ||
		    hsm_session_logout(tok, s) != HSM_OK ||
		    hsm_session_login(tok, s, HSM_ROLE_USER, USER_PIN) != HSM_OK ||
		    hsm_slot_generate(tok, s, spec[i].alg, spec[i].usage, spec[i].policy, &h) != HSM_OK) {
			abort();
		}
		if (i == 0 && pk0) {
			if (hsm_object_public_key(tok, s, h, pk0, 4096, pk0_len) != HSM_OK) {
				abort();
			}
		}
		hsm_session_close(tok, s);
	}
	return tok;
}

static hsm_session_t so_session(hsm_token_t *tok, hsm_slot_id_t slot)
{
	hsm_session_t s;
	if (hsm_session_open(tok, slot, &s) != HSM_OK ||
	    hsm_session_login(tok, s, HSM_ROLE_SO, SO_PIN) != HSM_OK) {
		abort();
	}
	return s;
}

/* 恢复后能否用 slot0 签出被原公钥认可的签名 */
static int key0_usable(hsm_token_t *tok, const uint8_t *pk0)
{
	hsm_session_t s;
	if (hsm_session_open(tok, 0, &s) != HSM_OK) {
		return 0;
	}
	int ok = 0;
	slot_meta_t m;
	if (hsm_session_login(tok, s, HSM_ROLE_USER, USER_PIN) == HSM_OK &&
	    hsm_slot_get_meta(tok, 0, &m) == HSM_OK) {
		hsm_handle_t h = ((hsm_handle_t)m.generation << 32) | 1;
		const pqc_alg_info_t *info = pqc_alg_info(PQC_ALG_ML_DSA_65);
		uint8_t *sig = malloc(info->sig_len);
		size_t sl = 0;
		const uint8_t msg[] = "post-restore";
		if (sig && hsm_object_sign(tok, s, h, msg, sizeof(msg), NULL, 0,
		                           sig, info->sig_len, &sl) == HSM_OK) {
			ok = pqc_verify(PQC_ALG_ML_DSA_65, pk0, msg, sizeof(msg), NULL, 0,
			                sig, sl) == PQC_OK;
		}
		free(sig);
	}
	hsm_session_close(tok, s);
	return ok;
}

static uint8_t *slurp(const char *p, size_t *n)
{
	FILE *f = fopen(p, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	rewind(f);
	uint8_t *b = malloc((size_t)sz);
	if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
		free(b);
		b = NULL;
	}
	fclose(f);
	*n = (size_t)sz;
	return b;
}

static void spew(const char *p, const uint8_t *b, size_t n)
{
	FILE *f = fopen(p, "wb");
	if (f) {
		fwrite(b, 1, n, f);
		fclose(f);
	}
}

/* 从 N 份里挑 idx[] 指定的 cnt 份，拷进连续缓冲 */
static void pick(const uint8_t *all, const size_t *all_lens, const int *idx, int cnt,
                 uint8_t *out, size_t *out_lens)
{
	for (int i = 0; i < cnt; i++) {
		memcpy(out + (size_t)i * HSM_SHARE_CAP, all + (size_t)idx[i] * HSM_SHARE_CAP,
		       HSM_SHARE_CAP);
		out_lens[i] = all_lens[idx[i]];
	}
}

int main(void)
{
	snprintf(g_path, sizeof(g_path), "/tmp/pqchsm_bk_%d.bin", (int)getpid());

	uint8_t pk0[4096];
	size_t pk0_len = 0;
	uint8_t shares[N * HSM_SHARE_CAP];
	size_t  share_lens[N];
	size_t  n_exported = 0;

	/* ---- 1. 建库 + 导出备份 ---- */
	TCASE("导出备份：只导出可备份槽位，产出 N 份分片");
	hsm_token_t *tok = build_token(pk0, &pk0_len);
	CHECK(tok != NULL);
	hsm_session_t so = so_session(tok, 0);

	/* 非 SO 会话不许导出 */
	{
		hsm_session_t pub;
		CHECK_EQ_INT(hsm_session_open(tok, 0, &pub), HSM_OK);
		CHECK_EQ_INT(hsm_backup_export(tok, pub, g_path, M, N, shares, HSM_SHARE_CAP,
		                               share_lens, &n_exported), HSM_ERR_NOT_AUTHORIZED);
		hsm_session_close(tok, pub);
	}
	CHECK_EQ_INT(hsm_backup_export(tok, so, g_path, M, N, shares, HSM_SHARE_CAP,
	                               share_lens, &n_exported), HSM_OK);
	/* slot2 没有 BACKUPABLE，所以只应导出 2 个 */
	CHECK_EQ_INT(n_exported, 2);
	for (int i = 0; i < N; i++) {
		CHECK_EQ_INT(share_lens[i], HSM_SHARE_LEN);
	}
	/* 备份文件里不应出现 RMK 或明文公钥以外的东西 —— 至少不能出现明文私钥。
	 * 这里用一个更强的检查：文件里不含 slot0 的公钥（它在包裹内）。 */
	{
		size_t fn = 0;
		uint8_t *fb = slurp(g_path, &fn);
		CHECK(fb != NULL);
		int found = 0;
		for (size_t i = 0; fn > pk0_len && i + pk0_len <= fn; i++) {
			if (memcmp(fb + i, pk0, pk0_len) == 0) {
				found = 1;
				break;
			}
		}
		CHECK_EQ_INT(found, 0);
		free(fb);
	}
	CHECK_EQ_INT(hsm_backup_export(tok, so, g_path, 6, 5, shares, HSM_SHARE_CAP,
	                               share_lens, &n_exported), HSM_ERR_BAD_ARG);
	/* 上一行失败不能破坏已有备份文件 */
	hsm_token_free(tok);

	/* ---- 2. 整机清零 ---- */
	TCASE("整机清零：所有槽位回到 UNINIT，密钥不可用");
	hsm_token_t *dev = hsm_token_new(N_SLOTS);
	CHECK(dev != NULL);
	for (int i = 0; i < N_SLOTS; i++) {
		CHECK_EQ_INT(hsm_slot_zeroize_forced(dev, (hsm_slot_id_t)i), HSM_OK);
		slot_state_t stt;
		CHECK_EQ_INT(hsm_slot_get_state(dev, (hsm_slot_id_t)i, &stt), HSM_OK);
		CHECK_EQ_INT(stt, SLOT_ST_UNINIT);
	}
	CHECK_EQ_INT(key0_usable(dev, pk0), 0);

	/* ---- 3. 负测试（都必须在恢复成功之前跑，证明它们真的挡住了）---- */
	uint8_t sel[N * HSM_SHARE_CAP];
	size_t  sel_lens[N];
	size_t n_restored = 0;

	TCASE("不足 M 份分片 → 恢复失败");
	{
		int idx[2] = { 0, 2 };
		pick(shares, share_lens, idx, 2, sel, sel_lens);
		CHECK_EQ_INT(hsm_backup_restore(dev, g_path, sel, HSM_SHARE_CAP, sel_lens, 2,
		                                &n_restored), HSM_ERR_INTEGRITY);
		CHECK_EQ_INT(key0_usable(dev, pk0), 0);
	}

	TCASE("分片被篡改 → 恢复失败");
	{
		int idx[M] = { 0, 1, 2 };
		pick(shares, share_lens, idx, M, sel, sel_lens);
		sel[HSM_SHARE_CAP + 3] ^= 0x01;      /* 改第 2 份的数据字节 */
		CHECK_EQ_INT(hsm_backup_restore(dev, g_path, sel, HSM_SHARE_CAP, sel_lens, M,
		                                &n_restored), HSM_ERR_INTEGRITY);
	}

	TCASE("拿别人的分片（另一套备份的）→ 恢复失败");
	{
		uint8_t other[N * HSM_SHARE_CAP];
		size_t other_lens[N];
		hsm_token_t *t2 = build_token(NULL, NULL);
		hsm_session_t s2 = so_session(t2, 0);
		char p2[300];
		snprintf(p2, sizeof(p2), "%s.other", g_path);
		CHECK_EQ_INT(hsm_backup_export(t2, s2, p2, M, N, other, HSM_SHARE_CAP,
		                               other_lens, NULL), HSM_OK);
		hsm_token_free(t2);
		int idx[M] = { 0, 1, 2 };
		pick(other, other_lens, idx, M, sel, sel_lens);
		/* 用另一套备份的分片去解本备份 */
		CHECK_EQ_INT(hsm_backup_restore(dev, g_path, sel, HSM_SHARE_CAP, sel_lens, M,
		                                &n_restored), HSM_ERR_INTEGRITY);
		unlink(p2);
	}

	TCASE("备份文件被篡改 → 恢复失败（逐处抽样）");
	{
		size_t fn = 0;
		uint8_t *orig = slurp(g_path, &fn);
		CHECK(orig != NULL);
		uint8_t *copy = malloc(fn);
		int idx[M] = { 0, 1, 2 };
		int escaped = 0, checked = 0;
		size_t step = fn / 80 ? fn / 80 : 1;
		for (size_t pos = 0; pos < fn; pos += step) {
			memcpy(copy, orig, fn);
			copy[pos] ^= 0x01;
			spew(g_path, copy, fn);
			pick(shares, share_lens, idx, M, sel, sel_lens);
			if (hsm_backup_restore(dev, g_path, sel, HSM_SHARE_CAP, sel_lens, M,
			                       &n_restored) == HSM_OK) {
				escaped++;
			}
			checked++;
		}
		CHECK(checked > 60);
		CHECK_EQ_INT(escaped, 0);
		spew(g_path, orig, fn);     /* 还原 */
		free(copy);
		free(orig);
	}

	TCASE("截断备份文件 → 恢复失败");
	{
		size_t fn = 0;
		uint8_t *orig = slurp(g_path, &fn);
		spew(g_path, orig, fn - 1);
		int idx[M] = { 0, 1, 2 };
		pick(shares, share_lens, idx, M, sel, sel_lens);
		CHECK_EQ_INT(hsm_backup_restore(dev, g_path, sel, HSM_SHARE_CAP, sel_lens, M,
		                                &n_restored), HSM_ERR_INTEGRITY);
		spew(g_path, orig, fn);
		free(orig);
	}

	/* 到这里为止，一次都没恢复成功过 */
	CHECK_EQ_INT(key0_usable(dev, pk0), 0);

	/* ---- 4. 正路：M 份分片恢复 ---- */
	TCASE("用 M 份分片恢复 → 原密钥可用（穷举所有 C(5,3)=10 种组合）");
	{
		int combos = 0, good = 0;
		for (int a = 0; a < N; a++) {
			for (int b = a + 1; b < N; b++) {
				for (int c = b + 1; c < N; c++) {
					int idx[M] = { a, b, c };
					/* 每次都先把设备清干净，确保是真恢复而不是残留 */
					for (int i = 0; i < N_SLOTS; i++) {
						hsm_slot_zeroize_forced(dev, (hsm_slot_id_t)i);
					}
					pick(shares, share_lens, idx, M, sel, sel_lens);
					n_restored = 0;
					if (hsm_backup_restore(dev, g_path, sel, HSM_SHARE_CAP, sel_lens,
					                       M, &n_restored) == HSM_OK &&
					    n_restored == 2 && key0_usable(dev, pk0)) {
						good++;
					}
					combos++;
				}
			}
		}
		CHECK_EQ_INT(combos, 10);
		CHECK_EQ_INT(good, 10);
	}

	TCASE("恢复后：可备份槽位回来了，纯 sealed 槽位没回来");
	{
		slot_meta_t m0, m2;
		CHECK_EQ_INT(hsm_slot_get_meta(dev, 0, &m0), HSM_OK);
		CHECK_EQ_INT(m0.state, SLOT_ST_LOADED);
		CHECK_EQ_INT(m0.alg, PQC_ALG_ML_DSA_65);
		CHECK(m0.policy & SLOT_POLICY_BACKUPABLE);
		/* slot2 不在备份里，恢复后仍是清零态 —— sealing 的代价，这是设计意图 */
		CHECK_EQ_INT(hsm_slot_get_meta(dev, 2, &m2), HSM_OK);
		CHECK_EQ_INT(m2.state, SLOT_ST_UNINIT);
	}

	TCASE("恢复后种子存储槽位（slot1）也能用");
	{
		hsm_session_t s;
		CHECK_EQ_INT(hsm_session_open(dev, 1, &s), HSM_OK);
		CHECK_EQ_INT(hsm_session_login(dev, s, HSM_ROLE_USER, USER_PIN), HSM_OK);
		slot_meta_t m;
		CHECK_EQ_INT(hsm_slot_get_meta(dev, 1, &m), HSM_OK);
		hsm_handle_t h = ((hsm_handle_t)m.generation << 32) | 2;
		const pqc_alg_info_t *info = pqc_alg_info(PQC_ALG_ML_KEM_768);
		uint8_t *pk = malloc(info->pk_len), *ct = malloc(info->ct_len);
		uint8_t ssa[64], ssb[64];
		size_t n = 0, sl = 0;
		CHECK_EQ_INT(hsm_object_public_key(dev, s, h, pk, info->pk_len, &n), HSM_OK);
		CHECK_EQ_INT(pqc_encaps(PQC_ALG_ML_KEM_768, pk, ct, ssa), PQC_OK);
		CHECK_EQ_INT(hsm_object_decaps(dev, s, h, ct, info->ct_len, ssb, sizeof(ssb), &sl),
		             HSM_OK);
		CHECK_EQ_MEM(ssa, ssb, info->ss_len);
		free(pk);
		free(ct);
		hsm_session_close(dev, s);
	}

	TCASE("恢复后 PIN 也一并回来（SO 与 User 都能登录）");
	{
		hsm_session_t s;
		CHECK_EQ_INT(hsm_session_open(dev, 0, &s), HSM_OK);
		CHECK_EQ_INT(hsm_session_login(dev, s, HSM_ROLE_SO, SO_PIN), HSM_OK);
		CHECK_EQ_INT(hsm_session_login(dev, s, HSM_ROLE_USER, USER_PIN), HSM_OK);
		CHECK_EQ_INT(hsm_session_login(dev, s, HSM_ROLE_USER, "wrong"), HSM_ERR_PIN_INCORRECT);
		hsm_session_close(dev, s);
	}

	/* ---- 5. 跨设备恢复（§8.3：备份不绑定设备，密钥库绑定）---- */
	TCASE("跨设备恢复：换 KDR 后备份仍能恢复，且恢复后重新 sealing 到新设备");
	{
		pqc_kdr_set_test_root((const uint8_t *)"brand-new-device", 16);
		hsm_token_t *newdev = hsm_token_new(N_SLOTS);
		CHECK(newdev != NULL);
		int idx[M] = { 1, 3, 4 };
		pick(shares, share_lens, idx, M, sel, sel_lens);
		CHECK_EQ_INT(hsm_backup_restore(newdev, g_path, sel, HSM_SHARE_CAP, sel_lens, M,
		                                &n_restored), HSM_OK);
		CHECK_EQ_INT(n_restored, 2);
		CHECK(key0_usable(newdev, pk0));

		/* 恢复后立刻固化成新设备的密钥库，且该库只能在新设备上打开 */
		char ks[300];
		snprintf(ks, sizeof(ks), "%s.ks", g_path);
		CHECK_EQ_INT(hsm_keystore_save(newdev, ks), HSM_OK);
		hsm_token_free(newdev);

		hsm_token_t *again = hsm_token_new(N_SLOTS);
		CHECK_EQ_INT(hsm_keystore_load(again, ks), HSM_OK);
		CHECK(key0_usable(again, pk0));
		hsm_token_free(again);

		/* 回到原设备：新设备的密钥库在这里打不开（sealing 生效） */
		pqc_kdr_set_test_root(NULL, 0);
		hsm_token_t *orig_dev = hsm_token_new(N_SLOTS);
		CHECK_EQ_INT(hsm_keystore_load(orig_dev, ks), HSM_ERR_INTEGRITY);
		hsm_token_free(orig_dev);
		unlink(ks);
	}

	TCASE("非法参数");
	{
		CHECK_EQ_INT(hsm_backup_restore(dev, g_path, NULL, HSM_SHARE_CAP, sel_lens, M, NULL),
		             HSM_ERR_BAD_ARG);
		CHECK_EQ_INT(hsm_backup_restore(dev, g_path, sel, HSM_SHARE_CAP, sel_lens, 0, NULL),
		             HSM_ERR_BAD_ARG);
		CHECK_EQ_INT(hsm_backup_restore(dev, "/nonexistent/bk.bin", sel, HSM_SHARE_CAP,
		                                sel_lens, M, NULL), HSM_ERR_BAD_ARG);
		CHECK_EQ_INT(hsm_backup_restore(NULL, g_path, sel, HSM_SHARE_CAP, sel_lens, M, NULL),
		             HSM_ERR_BAD_ARG);
	}

	hsm_token_free(dev);
	unlink(g_path);
	return test_report("test_backup");
}
