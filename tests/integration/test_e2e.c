/* 端到端集成：把整条链一次串起来
 *
 *   建库 → 生成 ML-DSA / ML-KEM 密钥 → sign/verify + encaps/decaps
 *        → 密钥库落盘 → 备份导出（M-of-N 分片）→ 审计锚点固化
 *        → **整机清零** → 确认密钥真的没了
 *        → 用备份 + M 份分片恢复 → 原公钥能验证恢复后签出的签名
 *        → 审计链验证 + 锚点验证 + 操作序列核对
 *
 * 之前每一段都有自己的单测，但**没有一条测试把它们串起来跑一遍**。
 * 分段绿 ≠ 整条链绿：跨模块的状态传递（会话失效、句柄代数、审计连续性、
 * 恢复后 PIN 还能不能用）只有整条跑才暴露。
 *
 * 判据一律是"能不能继续用"，不是"有没有报错"：
 * 恢复之后必须能用**恢复出来的私钥**签出一个能被**清零之前导出的公钥**验过的签名。
 */
#include "testlib.h"
#include "pqchsm/anchor.h"
#include "pqchsm/audit.h"
#include "pqchsm/backup.h"
#include "pqchsm/keystore.h"
#include "pqchsm/slot.h"
#include "pqchsm/util.h"

#include <stdlib.h>
#include <unistd.h>

#define SO_PIN   "so-secret-0001"
#define USER_PIN "user-pin-4242"
#define N_SLOTS  3
#define M 3
#define N 5

/* slot0 = 签名工作钥（可备份）
 * slot1 = KEM 工作钥（可备份 + 种子存储）
 * slot2 = 设备身份钥（**不可备份** —— 纯 sealed，用来签审计锚点） */
#define SLOT_SIG 0
#define SLOT_KEM 1
#define SLOT_ID  2

static char g_ks[192], g_bk[192], g_log[192], g_anc[192];

static void die(const char *what, hsm_status_t st)
{
	fprintf(stderr, "致命：%s 失败 -> %s (%d)\n", what, hsm_strerror(st), st);
	fflush(stderr);
	abort();
}

static hsm_session_t login_user(hsm_token_t *tok, hsm_slot_id_t slot)
{
	hsm_session_t s;
	hsm_status_t st = hsm_session_open(tok, slot, &s);
	if (st != HSM_OK) {
		die("session_open(user)", st);
	}
	st = hsm_session_login(tok, s, HSM_ROLE_USER, USER_PIN);
	if (st != HSM_OK) {
		die("login(user)", st);
	}
	return s;
}

static hsm_session_t login_so(hsm_token_t *tok, hsm_slot_id_t slot)
{
	hsm_session_t s;
	hsm_status_t st = hsm_session_open(tok, slot, &s);
	if (st != HSM_OK) {
		die("session_open(so)", st);
	}
	st = hsm_session_login(tok, s, HSM_ROLE_SO, SO_PIN);
	if (st != HSM_OK) {
		die("login(so)", st);
	}
	return s;
}

/* 供应一个槽位：init_token + 设 User PIN */
static void provision(hsm_token_t *tok, hsm_slot_id_t slot, const char *label)
{
	hsm_session_t s;
	hsm_status_t st = hsm_slot_init_token(tok, slot, label, SO_PIN);
	if (st != HSM_OK) {
		die("init_token", st);
	}
	s = login_so(tok, slot);
	st = hsm_slot_set_user_pin(tok, s, USER_PIN);
	if (st != HSM_OK) {
		die("set_user_pin", st);
	}
	if (hsm_session_close(tok, s) != HSM_OK) {
		die("session_close", HSM_ERR_BAD_ARG);
	}
}

/* 由槽位当前 generation 推出对象句柄 */
static hsm_handle_t handle_of(hsm_token_t *tok, hsm_slot_id_t slot)
{
	slot_meta_t m;
	if (hsm_slot_get_meta(tok, slot, &m) != HSM_OK) {
		return HSM_INVALID_HANDLE;
	}
	return ((hsm_handle_t)m.generation << 32) | (hsm_handle_t)(slot + 1);
}

/* 用 slot0 的私钥签 msg，返回签名长度；**任何一步失败都返回 0 而不是 abort** ——
 * 清零之后这个函数本来就该失败，它是判据的一部分。 */
static size_t sign_with_slot0(hsm_token_t *tok, const uint8_t *msg, size_t msg_len,
                              uint8_t *sig, size_t cap)
{
	hsm_session_t s;
	if (hsm_session_open(tok, SLOT_SIG, &s) != HSM_OK) {
		return 0;
	}
	size_t sl = 0;
	if (hsm_session_login(tok, s, HSM_ROLE_USER, USER_PIN) != HSM_OK) {
		hsm_session_close(tok, s);
		return 0;
	}
	hsm_status_t st = hsm_object_sign(tok, s, handle_of(tok, SLOT_SIG),
	                                  msg, msg_len, NULL, 0, sig, cap, &sl);
	hsm_session_close(tok, s);
	return st == HSM_OK ? sl : 0;
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);   /* abort 时别把已打印的内容吞掉 */
	int pid = (int)getpid();
	snprintf(g_ks, sizeof(g_ks), "/tmp/pqchsm_e2e_%d.ks", pid);
	snprintf(g_bk, sizeof(g_bk), "/tmp/pqchsm_e2e_%d.bk", pid);
	snprintf(g_log, sizeof(g_log), "/tmp/pqchsm_e2e_%d.log", pid);
	snprintf(g_anc, sizeof(g_anc), "/tmp/pqchsm_e2e_%d.anchor", pid);
	unlink(g_log);

	const pqc_alg_info_t *dsa = pqc_alg_info(PQC_ALG_ML_DSA_65);
	const pqc_alg_info_t *kem = pqc_alg_info(PQC_ALG_ML_KEM_768);
	uint8_t *pk0 = malloc(dsa->pk_len);        /* 清零前导出的公钥 —— 全程的判据 */
	uint8_t *idpk = malloc(dsa->pk_len);       /* 设备身份公钥，验锚点用 */
	uint8_t *sig = malloc(dsa->sig_len);
	uint8_t shares[N * HSM_SHARE_CAP];
	size_t share_lens[N];
	const uint8_t MSG[] = "level-A end-to-end";

	/* ---------- 1. 建库 ---------- */
	TCASE("1. 建库：3 个槽位 + 审计日志挂接");
	audit_log_t *log = audit_open(g_log);
	CHECK(log != NULL);
	hsm_token_t *tok = hsm_token_new(N_SLOTS);
	CHECK(tok != NULL);
	hsm_token_attach_audit(tok, log);

	provision(tok, SLOT_SIG, "sign-key");
	provision(tok, SLOT_KEM, "kem-key");
	provision(tok, SLOT_ID, "device-identity");
	CHECK_EQ_INT(audit_verify_file(g_log, NULL), 0);

	/* ---------- 2. 生成密钥 ---------- */
	TCASE("2. 生成三把密钥（签名钥/KEM 钥可备份，身份钥纯 sealed）");
	{
		hsm_session_t s = login_user(tok, SLOT_SIG);
		hsm_handle_t h;
		CHECK_EQ_INT(hsm_slot_generate(tok, s, PQC_ALG_ML_DSA_65, KEY_USAGE_SIGN,
		                               SLOT_POLICY_BACKUPABLE, &h), HSM_OK);
		size_t n = 0;
		CHECK_EQ_INT(hsm_object_public_key(tok, s, h, pk0, dsa->pk_len, &n), HSM_OK);
		CHECK_EQ_INT(n, dsa->pk_len);
		hsm_session_close(tok, s);

		s = login_user(tok, SLOT_KEM);
		CHECK_EQ_INT(hsm_slot_generate(tok, s, PQC_ALG_ML_KEM_768, KEY_USAGE_DECAP,
		                               SLOT_POLICY_BACKUPABLE | SLOT_POLICY_SEED_STORAGE,
		                               &h), HSM_OK);
		hsm_session_close(tok, s);

		s = login_user(tok, SLOT_ID);
		/* 身份钥不带 BACKUPABLE：设备损坏就该跟着消失 */
		CHECK_EQ_INT(hsm_slot_generate(tok, s, PQC_ALG_ML_DSA_65, KEY_USAGE_SIGN, 0, &h),
		             HSM_OK);
		CHECK_EQ_INT(hsm_object_public_key(tok, s, h, idpk, dsa->pk_len, &n), HSM_OK);
		hsm_session_close(tok, s);
	}

	/* ---------- 3. 用起来 ---------- */
	TCASE("3. sign/verify 与 encaps/decaps 都能用");
	size_t sl = sign_with_slot0(tok, MSG, sizeof(MSG), sig, dsa->sig_len);
	CHECK(sl > 0);
	CHECK_EQ_INT(pqc_verify(PQC_ALG_ML_DSA_65, pk0, MSG, sizeof(MSG), NULL, 0, sig, sl),
	             PQC_OK);
	{
		hsm_session_t s = login_user(tok, SLOT_KEM);
		uint8_t *kpk = malloc(kem->pk_len), *ct = malloc(kem->ct_len);
		uint8_t ss_host[64], ss_hsm[64];
		size_t n = 0, ssl = 0;
		CHECK_EQ_INT(hsm_object_public_key(tok, s, handle_of(tok, SLOT_KEM), kpk,
		                                   kem->pk_len, &n), HSM_OK);
		CHECK_EQ_INT(pqc_encaps(PQC_ALG_ML_KEM_768, kpk, ct, ss_host), PQC_OK);
		CHECK_EQ_INT(hsm_object_decaps(tok, s, handle_of(tok, SLOT_KEM), ct, kem->ct_len,
		                               ss_hsm, sizeof(ss_hsm), &ssl), HSM_OK);
		CHECK_EQ_MEM(ss_host, ss_hsm, kem->ss_len);
		free(kpk);
		free(ct);
		hsm_session_close(tok, s);
	}

	/* ---------- 4. 落盘 + 备份 + 锚点 ---------- */
	TCASE("4. 密钥库落盘 → 备份导出 → 审计锚点固化");
	CHECK_EQ_INT(hsm_keystore_save(tok, g_ks), HSM_OK);
	{
		hsm_session_t so = login_so(tok, SLOT_SIG);
		size_t exported = 0;
		CHECK_EQ_INT(hsm_backup_export(tok, so, g_bk, M, N, shares, HSM_SHARE_CAP,
		                               share_lens, &exported), HSM_OK);
		/* 只有两把带 BACKUPABLE 的进备份，身份钥不进 */
		CHECK_EQ_INT(exported, 2);
		hsm_session_close(tok, so);
	}
	{
		/* 锚点用设备身份钥签 —— 需要 User 会话 */
		hsm_session_t s = login_user(tok, SLOT_ID);
		CHECK_EQ_INT(hsm_audit_anchor_create(log, g_anc, tok, s, handle_of(tok, SLOT_ID),
		                                     1700000000ull), HSM_OK);
		hsm_session_close(tok, s);
	}
	uint64_t anchored = 0;
	CHECK_EQ_INT(hsm_audit_anchor_verify(g_log, g_anc, idpk, dsa->pk_len, &anchored), HSM_OK);
	CHECK(anchored > 0);
	uint64_t ops_before = audit_count(log);

	/* ---------- 5. 整机清零 ---------- */
	TCASE("5. 整机清零：三个槽位全清，密钥真的没了");
	for (int i = 0; i < N_SLOTS; i++) {
		CHECK_EQ_INT(hsm_slot_zeroize_forced(tok, (hsm_slot_id_t)i), HSM_OK);
		slot_state_t st;
		CHECK_EQ_INT(hsm_slot_get_state(tok, (hsm_slot_id_t)i, &st), HSM_OK);
		CHECK_EQ_INT(st, SLOT_ST_UNINIT);
	}
	/* 清零后签不出东西 */
	CHECK_EQ_INT(sign_with_slot0(tok, MSG, sizeof(MSG), sig, dsa->sig_len), 0);
	/* PIN 也没了 */
	{
		hsm_session_t s;
		CHECK_EQ_INT(hsm_session_open(tok, SLOT_SIG, &s), HSM_OK);
		CHECK_EQ_INT(hsm_session_login(tok, s, HSM_ROLE_USER, USER_PIN), HSM_ERR_BAD_STATE);
		hsm_session_close(tok, s);
	}

	/* ---------- 6. 恢复 ---------- */
	TCASE("6. 用备份 + M 份分片恢复");
	{
		uint8_t sel[M * HSM_SHARE_CAP];
		size_t sel_lens[M];
		int idx[M] = { 0, 2, 4 };
		for (int i = 0; i < M; i++) {
			memcpy(sel + (size_t)i * HSM_SHARE_CAP,
			       shares + (size_t)idx[i] * HSM_SHARE_CAP, HSM_SHARE_CAP);
			sel_lens[i] = share_lens[idx[i]];
		}
		/* 先来一次负测试：只给 M-1 份必须失败 */
		size_t nr = 0;
		CHECK_EQ_INT(hsm_backup_restore(tok, g_bk, sel, HSM_SHARE_CAP, sel_lens, M - 1, &nr),
		             HSM_ERR_INTEGRITY);
		CHECK_EQ_INT(sign_with_slot0(tok, MSG, sizeof(MSG), sig, dsa->sig_len), 0);

		CHECK_EQ_INT(hsm_backup_restore(tok, g_bk, sel, HSM_SHARE_CAP, sel_lens, M, &nr),
		             HSM_OK);
		CHECK_EQ_INT(nr, 2);
	}

	TCASE("7. ★ 判据：恢复出来的私钥能签出被**清零前的公钥**验过的签名");
	sl = sign_with_slot0(tok, MSG, sizeof(MSG), sig, dsa->sig_len);
	CHECK(sl > 0);
	CHECK_EQ_INT(pqc_verify(PQC_ALG_ML_DSA_65, pk0, MSG, sizeof(MSG), NULL, 0, sig, sl),
	             PQC_OK);

	TCASE("8. 恢复后 KEM 槽位（种子存储）也照常工作");
	{
		hsm_session_t s = login_user(tok, SLOT_KEM);
		uint8_t *kpk = malloc(kem->pk_len), *ct = malloc(kem->ct_len);
		uint8_t a[64], b[64];
		size_t n = 0, ssl = 0;
		CHECK_EQ_INT(hsm_object_public_key(tok, s, handle_of(tok, SLOT_KEM), kpk,
		                                   kem->pk_len, &n), HSM_OK);
		CHECK_EQ_INT(pqc_encaps(PQC_ALG_ML_KEM_768, kpk, ct, a), PQC_OK);
		CHECK_EQ_INT(hsm_object_decaps(tok, s, handle_of(tok, SLOT_KEM), ct, kem->ct_len,
		                               b, sizeof(b), &ssl), HSM_OK);
		CHECK_EQ_MEM(a, b, kem->ss_len);
		free(kpk);
		free(ct);
		hsm_session_close(tok, s);
	}

	TCASE("9. 身份钥没进备份，所以恢复后仍是空的");
	{
		slot_meta_t m;
		CHECK_EQ_INT(hsm_slot_get_meta(tok, SLOT_ID, &m), HSM_OK);
		CHECK_EQ_INT(m.state, SLOT_ST_UNINIT);
	}

	/* ---------- 7. 审计 ---------- */
	TCASE("10. 审计链完好、锚点仍然对得上、操作序列符合预期");
	CHECK_EQ_INT(audit_verify_file(g_log, NULL), 0);
	/* 锚点覆盖的是清零之前那一段前缀，清零/恢复只是往后追加 —— 仍应通过 */
	CHECK_EQ_INT(hsm_audit_anchor_verify(g_log, g_anc, idpk, dsa->pk_len, NULL), HSM_OK);
	{
		uint64_t total = audit_count(log);
		CHECK(total > ops_before);   /* 清零与恢复都落了审计 */
		int n_zeroize = 0, n_restore = 0, n_backup = 0, n_generate = 0, n_sign = 0;
		for (uint64_t i = 0; i < total; i++) {
			uint32_t op, role, slot, res;
			uint64_t ts;
			char detail[AUDIT_DETAIL_LEN + 1];
			if (audit_read(g_log, i, &ts, &op, &role, &slot, &res, detail) != 0) {
				continue;
			}
			switch (op) {
			case AUDIT_OP_ZEROIZE:       n_zeroize++;  break;
			case AUDIT_OP_RESTORE:       n_restore++;  break;
			case AUDIT_OP_BACKUP_EXPORT: n_backup++;   break;
			case AUDIT_OP_GENERATE:      n_generate++; break;
			case AUDIT_OP_SIGN:          n_sign++;     break;
			default: break;
			}
		}
		CHECK_EQ_INT(n_generate, 3);      /* 三把密钥 */
		CHECK_EQ_INT(n_backup, 1);
		CHECK_EQ_INT(n_zeroize, 3);       /* 三个槽位 */
		CHECK_EQ_INT(n_restore, 2);       /* 一次失败 + 一次成功，都要留痕 */
		CHECK(n_sign >= 3);
	}

	TCASE("11. 红线：整条链跑完，日志与备份文件里都搜不到 PIN 明文");
	{
		const char *files[] = { g_log, g_bk, g_ks };
		for (size_t f = 0; f < 3; f++) {
			FILE *fp = fopen(files[f], "rb");
			CHECK(fp != NULL);
			if (!fp) {
				continue;
			}
			fseek(fp, 0, SEEK_END);
			long n = ftell(fp);
			rewind(fp);
			uint8_t *buf = malloc((size_t)n);
			CHECK(fread(buf, 1, (size_t)n, fp) == (size_t)n);
			fclose(fp);
			int found = 0;
			size_t pl = strlen(USER_PIN);
			for (long i = 0; i + (long)pl <= n; i++) {
				if (memcmp(buf + i, USER_PIN, pl) == 0) {
					found = 1;
					break;
				}
			}
			CHECK_EQ_INT(found, 0);
			free(buf);
		}
	}

	TCASE("12. 恢复后的状态可以再次落盘并重新装载（闭环）");
	CHECK_EQ_INT(hsm_keystore_save(tok, g_ks), HSM_OK);
	{
		hsm_token_t *t2 = hsm_token_new(N_SLOTS);
		CHECK(t2 != NULL);
		CHECK_EQ_INT(hsm_keystore_load(t2, g_ks), HSM_OK);
		uint8_t *s2 = malloc(dsa->sig_len);
		hsm_session_t s = login_user(t2, SLOT_SIG);
		size_t n = 0;
		CHECK_EQ_INT(hsm_object_sign(t2, s, handle_of(t2, SLOT_SIG), MSG, sizeof(MSG),
		                             NULL, 0, s2, dsa->sig_len, &n), HSM_OK);
		CHECK_EQ_INT(pqc_verify(PQC_ALG_ML_DSA_65, pk0, MSG, sizeof(MSG), NULL, 0, s2, n),
		             PQC_OK);
		hsm_session_close(t2, s);
		free(s2);
		hsm_token_free(t2);
	}

	hsm_token_free(tok);
	audit_close(log);
	free(pk0);
	free(idpk);
	free(sig);
	unlink(g_ks);
	unlink(g_bk);
	unlink(g_log);
	unlink(g_anc);
	return test_report("test_e2e");
}
