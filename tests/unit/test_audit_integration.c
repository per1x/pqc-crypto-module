/* 审计与槽位管理器的联动
 *
 * audit.c 自身的哈希链正确性由 test_audit 覆盖；这里验证的是**接线**：
 * 敏感操作是否真的落审计、失败是否也落、以及日志里绝不出现密钥材料。
 */
#include "testlib.h"
#include "pqchsm/audit.h"
#include "pqchsm/backup.h"
#include "pqchsm/keystore.h"
#include "pqchsm/slot.h"

#include "../../src/slot/slot_internal.h"

#include <stdlib.h>
#include <unistd.h>

#define SO_PIN   "so-secret-0001"
#define USER_PIN "user-pin-4242"

static char g_log[128], g_ks[128], g_bk[128];

static int mem_contains(const void *hay, size_t hn, const void *needle, size_t nn)
{
	if (nn == 0 || hn < nn) {
		return 0;
	}
	const uint8_t *h = (const uint8_t *)hay;
	for (size_t i = 0; i + nn <= hn; i++) {
		if (memcmp(h + i, needle, nn) == 0) {
			return 1;
		}
	}
	return 0;
}

/* 数一数日志里某个 op 出现了几次 */
static int count_op(const char *path, uint32_t want_op, uint64_t n)
{
	int c = 0;
	for (uint64_t i = 0; i < n; i++) {
		uint64_t ts;
		uint32_t op, role, slot, result;
		char detail[AUDIT_DETAIL_LEN + 1];
		if (audit_read(path, i, &ts, &op, &role, &slot, &result, detail) == 0 &&
		    op == want_op) {
			c++;
		}
	}
	return c;
}

int main(void)
{
	int pid = (int)getpid();
	snprintf(g_log, sizeof(g_log), "/tmp/pqchsm_ai_%d.log", pid);
	snprintf(g_ks, sizeof(g_ks), "/tmp/pqchsm_ai_%d.ks", pid);
	snprintf(g_bk, sizeof(g_bk), "/tmp/pqchsm_ai_%d.bk", pid);
	unlink(g_log);

	TCASE("挂接审计后，敏感操作自动落链");
	audit_log_t *log = audit_open(g_log);
	CHECK(log != NULL);
	hsm_token_t *tok = hsm_token_new(2);
	CHECK(tok != NULL);
	hsm_token_attach_audit(tok, log);

	hsm_session_t s;
	hsm_handle_t h;
	CHECK_EQ_INT(hsm_slot_init_token(tok, 0, "audited", SO_PIN), HSM_OK);
	CHECK_EQ_INT(hsm_session_open(tok, 0, &s), HSM_OK);
	CHECK_EQ_INT(hsm_session_login(tok, s, HSM_ROLE_SO, SO_PIN), HSM_OK);
	CHECK_EQ_INT(hsm_slot_set_user_pin(tok, s, USER_PIN), HSM_OK);
	CHECK_EQ_INT(hsm_session_logout(tok, s), HSM_OK);
	CHECK_EQ_INT(hsm_session_login(tok, s, HSM_ROLE_USER, USER_PIN), HSM_OK);
	CHECK_EQ_INT(hsm_slot_generate(tok, s, PQC_ALG_ML_DSA_65, KEY_USAGE_SIGN,
	                               SLOT_POLICY_BACKUPABLE, &h), HSM_OK);

	const pqc_alg_info_t *info = pqc_alg_info(PQC_ALG_ML_DSA_65);
	uint8_t *sig = malloc(info->sig_len);
	size_t sl = 0;
	CHECK_EQ_INT(hsm_object_sign(tok, s, h, (const uint8_t *)"m", 1, NULL, 0,
	                             sig, info->sig_len, &sl), HSM_OK);

	uint64_t n = audit_count(log);
	CHECK(n >= 5);
	CHECK_EQ_INT(audit_verify_file(g_log, NULL), 0);

	TCASE("各类操作都在日志里各就各位");
	CHECK_EQ_INT(count_op(g_log, AUDIT_OP_INIT_TOKEN, n), 1);
	CHECK_EQ_INT(count_op(g_log, AUDIT_OP_LOGIN, n), 2);        /* SO + User */
	CHECK_EQ_INT(count_op(g_log, AUDIT_OP_GENERATE, n), 1);
	CHECK_EQ_INT(count_op(g_log, AUDIT_OP_SIGN, n), 1);

	TCASE("失败与锁定也必须落审计");
	CHECK_EQ_INT(hsm_session_logout(tok, s), HSM_OK);
	for (uint32_t i = 1; i < HSM_PIN_MAX_FAILS; i++) {
		CHECK_EQ_INT(hsm_session_login(tok, s, HSM_ROLE_USER, "bad"), HSM_ERR_PIN_INCORRECT);
	}
	CHECK_EQ_INT(hsm_session_login(tok, s, HSM_ROLE_USER, "bad"), HSM_ERR_PIN_LOCKED);
	n = audit_count(log);
	CHECK_EQ_INT(count_op(g_log, AUDIT_OP_LOGIN_FAIL, n), (int)HSM_PIN_MAX_FAILS - 1);
	CHECK_EQ_INT(count_op(g_log, AUDIT_OP_LOCKOUT, n), 1);
	CHECK_EQ_INT(audit_verify_file(g_log, NULL), 0);

	TCASE("解锁 / 备份导出 / 密钥库落盘 / 清零 都留痕");
	CHECK_EQ_INT(hsm_session_login(tok, s, HSM_ROLE_SO, SO_PIN), HSM_OK);
	CHECK_EQ_INT(hsm_slot_unlock(tok, s, 0), HSM_OK);
	{
		uint8_t shares[5 * HSM_SHARE_CAP];
		size_t lens[5];
		CHECK_EQ_INT(hsm_backup_export(tok, s, g_bk, 3, 5, shares, HSM_SHARE_CAP, lens, NULL),
		             HSM_OK);
	}
	CHECK_EQ_INT(hsm_keystore_save(tok, g_ks), HSM_OK);
	CHECK_EQ_INT(hsm_slot_zeroize(tok, s, 0), HSM_OK);
	n = audit_count(log);
	/* 这一条以前是"假通过"：hsm_slot_unlock() 根本没落审计，唯一那条
	 * AUDIT_OP_UNLOCK 是 hsm_slot_set_user_pin() 写的。现在两件事各有各的
	 * op（AUDIT_OP_SET_USER_PIN），这条断言才真的在测解锁。 */
	CHECK_EQ_INT(count_op(g_log, AUDIT_OP_UNLOCK, n), 1);
	CHECK_EQ_INT(count_op(g_log, AUDIT_OP_SET_USER_PIN, n), 1);
	CHECK_EQ_INT(count_op(g_log, AUDIT_OP_BACKUP_EXPORT, n), 1);
	CHECK_EQ_INT(count_op(g_log, AUDIT_OP_KEK_ROTATE, n), 1);
	CHECK_EQ_INT(count_op(g_log, AUDIT_OP_ZEROIZE, n), 1);
	CHECK_EQ_INT(audit_verify_file(g_log, NULL), 0);

	TCASE("红线：审计日志里搜不到任何密钥材料 / PIN / 验证值");
	{
		FILE *f = fopen(g_log, "rb");
		CHECK(f != NULL);
		fseek(f, 0, SEEK_END);
		long fsz = ftell(f);
		rewind(f);
		uint8_t *buf = malloc((size_t)fsz);
		CHECK(fread(buf, 1, (size_t)fsz, f) == (size_t)fsz);
		fclose(f);
		/* PIN 明文不能出现 */
		CHECK_EQ_INT(mem_contains(buf, (size_t)fsz, SO_PIN, strlen(SO_PIN)), 0);
		CHECK_EQ_INT(mem_contains(buf, (size_t)fsz, USER_PIN, strlen(USER_PIN)), 0);
		/* 签名（含密钥相关材料）不能出现 */
		CHECK_EQ_INT(mem_contains(buf, (size_t)fsz, sig, 48), 0);
		free(buf);
	}

	TCASE("解除挂接后不再落审计");
	{
		uint64_t before = audit_count(log);
		hsm_token_attach_audit(tok, NULL);
		CHECK_EQ_INT(hsm_slot_init_token(tok, 1, "silent", SO_PIN), HSM_OK);
		CHECK_EQ_INT(audit_count(log), before);
	}

	TCASE("没挂审计的 token 一切照常工作");
	{
		hsm_token_t *plain = hsm_token_new(1);
		CHECK(plain != NULL);
		hsm_session_t ps;
		hsm_handle_t ph;
		CHECK_EQ_INT(hsm_slot_init_token(plain, 0, "no-audit", SO_PIN), HSM_OK);
		CHECK_EQ_INT(hsm_session_open(plain, 0, &ps), HSM_OK);
		CHECK_EQ_INT(hsm_session_login(plain, ps, HSM_ROLE_SO, SO_PIN), HSM_OK);
		CHECK_EQ_INT(hsm_slot_set_user_pin(plain, ps, USER_PIN), HSM_OK);
		CHECK_EQ_INT(hsm_session_logout(plain, ps), HSM_OK);
		CHECK_EQ_INT(hsm_session_login(plain, ps, HSM_ROLE_USER, USER_PIN), HSM_OK);
		CHECK_EQ_INT(hsm_slot_generate(plain, ps, PQC_ALG_ML_DSA_44, KEY_USAGE_SIGN, 0, &ph),
		             HSM_OK);
		hsm_token_free(plain);
	}

	free(sig);
	hsm_token_free(tok);
	audit_close(log);
	unlink(g_log);
	unlink(g_ks);
	unlink(g_bk);
	return test_report("test_audit_integration");
}
