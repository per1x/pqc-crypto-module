/* 密钥库持久化测试
 *
 * 重点三条：
 *   1. 存→读往返后密钥仍可用（签名结果可被原公钥验证）；
 *   2. 文件被篡改（任意一字节）装载必须失败；
 *   3. 掉电一致性：在 save 的每一个写点注入"断电"，重启后要么旧值要么新值。
 */
#include "testlib.h"
#include "pqchsm/keystore.h"
#include "pqchsm/slot.h"
#include "pqchsm/kdr.h"
#include "pqchsm/util.h"

#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define SO_PIN   "so-secret-0001"
#define USER_PIN "user-pin-4242"
#define N_SLOTS  3

static char g_path[256];

static void provision(hsm_token_t *tok, hsm_slot_id_t slot, pqc_alg_t alg,
                      uint32_t usage, uint32_t policy, uint8_t *pk_out, size_t *pk_len)
{
	hsm_session_t s;
	hsm_handle_t h;
	char label[32];
	snprintf(label, sizeof(label), "slot-%u", slot);
	if (hsm_slot_init_token(tok, slot, label, SO_PIN) != HSM_OK ||
	    hsm_session_open(tok, slot, &s) != HSM_OK ||
	    hsm_session_login(tok, s, HSM_ROLE_SO, SO_PIN) != HSM_OK ||
	    hsm_slot_set_user_pin(tok, s, USER_PIN) != HSM_OK ||
	    hsm_session_logout(tok, s) != HSM_OK ||
	    hsm_session_login(tok, s, HSM_ROLE_USER, USER_PIN) != HSM_OK ||
	    hsm_slot_generate(tok, s, alg, usage, policy, &h) != HSM_OK) {
		abort();
	}
	if (pk_out && hsm_object_public_key(tok, s, h, pk_out, 4096, pk_len) != HSM_OK) {
		abort();
	}
	hsm_session_close(tok, s);
}

static hsm_token_t *make_populated(uint8_t *pk0, size_t *pk0_len)
{
	hsm_token_t *tok = hsm_token_new(N_SLOTS);
	if (!tok) {
		return NULL;
	}
	/* slot0：完整私钥存储的签名钥；slot1：种子存储的 KEM 钥；slot2：只 init 不装载 */
	provision(tok, 0, PQC_ALG_ML_DSA_65, KEY_USAGE_SIGN, 0, pk0, pk0_len);
	provision(tok, 1, PQC_ALG_ML_KEM_768, KEY_USAGE_DECAP, SLOT_POLICY_SEED_STORAGE, NULL, NULL);
	hsm_slot_init_token(tok, 2, "empty-slot", SO_PIN);
	return tok;
}

/* 用恢复出来的槽位签名，并用原公钥验证 —— 证明恢复的是同一把密钥 */
static int can_sign_with(hsm_token_t *tok, const uint8_t *pk0, size_t pk0_len)
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
		size_t sig_len = 0;
		const uint8_t msg[] = "restored-key-check";
		if (sig && hsm_object_sign(tok, s, h, msg, sizeof(msg), NULL, 0,
		                           sig, info->sig_len, &sig_len) == HSM_OK) {
			ok = (pqc_verify(PQC_ALG_ML_DSA_65, pk0, msg, sizeof(msg), NULL, 0,
			                 sig, sig_len) == PQC_OK) && pk0_len == info->pk_len;
		}
		free(sig);
	}
	hsm_session_close(tok, s);
	return ok;
}

static long file_size(const char *p)
{
	FILE *f = fopen(p, "rb");
	if (!f) {
		return -1;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fclose(f);
	return n;
}

static uint8_t *slurp(const char *p, size_t *n)
{
	long sz = file_size(p);
	if (sz <= 0) {
		return NULL;
	}
	FILE *f = fopen(p, "rb");
	if (!f) {
		return NULL;
	}
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

static void test_roundtrip(void)
{
	TCASE("存 → 读往返，密钥仍可用");
	uint8_t pk0[4096];
	size_t pk0_len = 0;
	hsm_token_t *tok = make_populated(pk0, &pk0_len);
	CHECK(tok != NULL);
	CHECK_EQ_INT(hsm_keystore_save(tok, g_path), HSM_OK);

	slot_meta_t before[N_SLOTS];
	for (int i = 0; i < N_SLOTS; i++) {
		CHECK_EQ_INT(hsm_slot_get_meta(tok, (hsm_slot_id_t)i, &before[i]), HSM_OK);
	}
	hsm_token_free(tok);

	/* 全新 token，从文件恢复 */
	hsm_token_t *tok2 = hsm_token_new(N_SLOTS);
	CHECK(tok2 != NULL);
	CHECK_EQ_INT(hsm_keystore_load(tok2, g_path), HSM_OK);
	for (int i = 0; i < N_SLOTS; i++) {
		slot_meta_t after;
		CHECK_EQ_INT(hsm_slot_get_meta(tok2, (hsm_slot_id_t)i, &after), HSM_OK);
		CHECK_EQ_INT(after.state, before[i].state);
		CHECK_EQ_INT(after.alg, before[i].alg);
		CHECK_EQ_INT(after.usage, before[i].usage);
		CHECK_EQ_INT(after.policy, before[i].policy);
		CHECK_EQ_INT(after.generation, before[i].generation);
		CHECK(strcmp(after.label, before[i].label) == 0);
	}
	/* PIN 也要一并恢复 */
	CHECK(can_sign_with(tok2, pk0, pk0_len));

	/* 种子存储的 KEM 槽位恢复后也要能用 */
	{
		hsm_session_t s;
		CHECK_EQ_INT(hsm_session_open(tok2, 1, &s), HSM_OK);
		CHECK_EQ_INT(hsm_session_login(tok2, s, HSM_ROLE_USER, USER_PIN), HSM_OK);
		slot_meta_t m;
		CHECK_EQ_INT(hsm_slot_get_meta(tok2, 1, &m), HSM_OK);
		hsm_handle_t h = ((hsm_handle_t)m.generation << 32) | 2;
		const pqc_alg_info_t *info = pqc_alg_info(PQC_ALG_ML_KEM_768);
		uint8_t *pk = malloc(info->pk_len), *ct = malloc(info->ct_len);
		uint8_t ss_a[64], ss_b[64];
		size_t n = 0, ss_len = 0;
		CHECK_EQ_INT(hsm_object_public_key(tok2, s, h, pk, info->pk_len, &n), HSM_OK);
		CHECK_EQ_INT(pqc_encaps(PQC_ALG_ML_KEM_768, pk, ct, ss_a), PQC_OK);
		CHECK_EQ_INT(hsm_object_decaps(tok2, s, h, ct, info->ct_len, ss_b, sizeof(ss_b),
		                               &ss_len), HSM_OK);
		CHECK_EQ_MEM(ss_a, ss_b, info->ss_len);
		free(pk);
		free(ct);
		hsm_session_close(tok2, s);
	}
	hsm_token_free(tok2);
}

static void test_tamper(void)
{
	TCASE("文件被篡改 → 装载必须失败");
	uint8_t pk0[4096];
	size_t pk0_len = 0;
	hsm_token_t *tok = make_populated(pk0, &pk0_len);
	CHECK(tok != NULL);
	CHECK_EQ_INT(hsm_keystore_save(tok, g_path), HSM_OK);
	hsm_token_free(tok);

	size_t n = 0;
	uint8_t *orig = slurp(g_path, &n);
	CHECK(orig != NULL);
	CHECK(n > 0);

	/* 逐字节翻转（抽样，避免测试太慢）：每一处都必须被检出 */
	int checked = 0, escaped = 0;
	size_t step = n / 120 ? n / 120 : 1;
	uint8_t *copy = malloc(n);
	for (size_t pos = 0; pos < n; pos += step) {
		memcpy(copy, orig, n);
		copy[pos] ^= 0x01;
		spew(g_path, copy, n);
		hsm_token_t *t = hsm_token_new(N_SLOTS);
		hsm_status_t st = hsm_keystore_load(t, g_path);
		if (st == HSM_OK) {
			escaped++;
			fprintf(stderr, "  偏移 %zu 被改动后仍装载成功！\n", pos);
		}
		hsm_token_free(t);
		checked++;
	}
	CHECK(checked > 100);
	CHECK_EQ_INT(escaped, 0);

	TCASE("截断文件 → 失败");
	spew(g_path, orig, n - 1);
	{
		hsm_token_t *t = hsm_token_new(N_SLOTS);
		CHECK_EQ_INT(hsm_keystore_load(t, g_path), HSM_ERR_INTEGRITY);
		hsm_token_free(t);
	}
	TCASE("尾部追加垃圾 → 失败");
	{
		uint8_t *big = malloc(n + 8);
		memcpy(big, orig, n);
		memset(big + n, 0xAA, 8);
		spew(g_path, big, n + 8);
		hsm_token_t *t = hsm_token_new(N_SLOTS);
		CHECK_EQ_INT(hsm_keystore_load(t, g_path), HSM_ERR_INTEGRITY);
		hsm_token_free(t);
		free(big);
	}
	TCASE("槽位数不符 → 失败");
	spew(g_path, orig, n);
	{
		hsm_token_t *t = hsm_token_new(N_SLOTS + 1);
		CHECK_EQ_INT(hsm_keystore_load(t, g_path), HSM_ERR_INTEGRITY);
		hsm_token_free(t);
	}
	TCASE("换设备（换 KDR）→ 装载失败");
	{
		pqc_kdr_set_test_root((const uint8_t *)"other-device", 12);
		hsm_token_t *t = hsm_token_new(N_SLOTS);
		CHECK_EQ_INT(hsm_keystore_load(t, g_path), HSM_ERR_INTEGRITY);
		hsm_token_free(t);
		pqc_kdr_set_test_root(NULL, 0);
		hsm_token_t *t2 = hsm_token_new(N_SLOTS);
		CHECK_EQ_INT(hsm_keystore_load(t2, g_path), HSM_OK);
		hsm_token_free(t2);
	}

	free(copy);
	free(orig);
}

/* 在子进程里 save 并在第 crash_at 次写之后"断电" */
static void save_and_crash(int crash_at)
{
	pid_t pid = fork();
	if (pid == 0) {
		uint8_t pk[4096];
		size_t pl = 0;
		hsm_token_t *t = make_populated(pk, &pl);
		hsm_keystore_set_crash_point(crash_at);
		hsm_keystore_save(t, g_path);
		_exit(0);
	}
	int status = 0;
	waitpid(pid, &status, 0);
}

static void test_power_fail(void)
{
	TCASE("掉电一致性：save 的每个写点断电，旧文件都必须完好");
	uint8_t pk0[4096];
	size_t pk0_len = 0;
	hsm_token_t *tok = make_populated(pk0, &pk0_len);
	CHECK(tok != NULL);
	CHECK_EQ_INT(hsm_keystore_save(tok, g_path), HSM_OK);
	int total_writes = hsm_keystore_last_write_count();
	CHECK(total_writes >= 2 + N_SLOTS);   /* 头 + 每槽一条 + tag */
	hsm_token_free(tok);

	size_t good_len = 0;
	uint8_t *good = slurp(g_path, &good_len);
	CHECK(good != NULL);

	/* 穷举所有写点 */
	for (int cp = 1; cp <= total_writes; cp++) {
		spew(g_path, good, good_len);      /* 恢复"旧的完整文件" */
		save_and_crash(cp);

		/* 断电后：主文件必须仍是旧的完整文件（rename 还没发生） */
		hsm_token_t *t = hsm_token_new(N_SLOTS);
		hsm_status_t st = hsm_keystore_load(t, g_path);
		if (st != HSM_OK) {
			fprintf(stderr, "  在第 %d 次写后断电，旧文件损坏了\n", cp);
		}
		CHECK_EQ_INT(st, HSM_OK);
		/* 而且旧文件里的密钥仍然可用 */
		CHECK(can_sign_with(t, pk0, pk0_len));
		hsm_token_free(t);

		/* 残留的 .tmp 不应被误当成主文件；清掉它进入下一轮 */
		char tmp[300];
		snprintf(tmp, sizeof(tmp), "%s.tmp", g_path);
		unlink(tmp);
	}

	TCASE("崩溃残留的 .tmp 不影响下一次正常 save");
	{
		spew(g_path, good, good_len);
		save_and_crash(2);                 /* 故意留下半截 .tmp */
		char tmp[300];
		snprintf(tmp, sizeof(tmp), "%s.tmp", g_path);
		CHECK(file_size(tmp) > 0);         /* 确实有残留 */
		hsm_token_t *t = hsm_token_new(N_SLOTS);
		CHECK_EQ_INT(hsm_keystore_load(t, g_path), HSM_OK);
		CHECK_EQ_INT(hsm_keystore_save(t, g_path), HSM_OK);   /* 应当直接覆盖残留 */
		hsm_token_free(t);
		hsm_token_t *t2 = hsm_token_new(N_SLOTS);
		CHECK_EQ_INT(hsm_keystore_load(t2, g_path), HSM_OK);
		CHECK(can_sign_with(t2, pk0, pk0_len));
		hsm_token_free(t2);
		unlink(tmp);
	}

	TCASE("完整跑完一次 save 后，新文件可装载");
	spew(g_path, good, good_len);
	save_and_crash(-1);
	{
		hsm_token_t *t = hsm_token_new(N_SLOTS);
		CHECK_EQ_INT(hsm_keystore_load(t, g_path), HSM_OK);
		hsm_token_free(t);
	}
	free(good);
}

/* ============================================================================
 * 防回滚：把整份 keystore 换成一份**旧快照**必须被拒
 * ============================================================================
 * 这一条盯的是独立评审 H3。旧快照的全文件 MAC **是对的**（当初就是自己算的），
 * 所以 MAC 拦不住它 —— 拦住它的只能是那个每次写盘 +1 的 epoch。
 *
 * 场景就是最朴素的那个：拿到 SD 卡，把 keystore 拷回一份先前的备份，
 * 于是 PIN 锁定计数、已吊销的密钥、槽位状态全都回到从前。
 *
 * ⚠️ 这条用例证明的是"**换一个文件**会被发现"。它**不**证明防回滚成立 ——
 *    攻击者若把 <keystore>.epoch 也一起换回旧值就照样能过。真正的防回滚
 *    需要攻击者写不了的单调存储（eFUSE 计数器/RPMB），这块板没有。
 *    口径见 src/store/keystore.c 里 epoch 那段。
 */
static void test_rollback(void)
{
	TCASE("防回滚：旧快照被拒（MAC 是对的也不行）");
	char snap[320], epoch_path[320];

	snprintf(snap, sizeof(snap), "%s.snap", g_path);
	snprintf(epoch_path, sizeof(epoch_path), "%s.epoch", g_path);

	uint8_t pk0[4096];
	size_t pk0_len = 0;
	hsm_token_t *tok = make_populated(pk0, &pk0_len);
	CHECK(tok != NULL);

	/* 第一次落盘 → epoch 1，留一份快照当作"攻击者手里的旧备份" */
	CHECK_EQ_INT(hsm_keystore_save(tok, g_path), HSM_OK);
	{
		FILE *a = fopen(g_path, "rb"), *b = fopen(snap, "wb");
		CHECK(a != NULL && b != NULL);
		if (a && b) {
			int c;
			while ((c = fgetc(a)) != EOF) { fputc(c, b); }
		}
		if (a) fclose(a);
		if (b) fclose(b);
	}

	/* 再写两次 → epoch 3。锚点跟着推到 3。 */
	CHECK_EQ_INT(hsm_keystore_save(tok, g_path), HSM_OK);
	CHECK_EQ_INT(hsm_keystore_save(tok, g_path), HSM_OK);
	hsm_token_free(tok);

	/* 当前文件当然装得上 */
	{
		hsm_token_t *t = hsm_token_new(N_SLOTS);
		CHECK(t != NULL);
		CHECK_EQ_INT(hsm_keystore_load(t, g_path), HSM_OK);
		hsm_token_free(t);
	}

	/* ---- 攻击：把旧快照拷回去 ---- */
	{
		FILE *a = fopen(snap, "rb"), *b = fopen(g_path, "wb");
		CHECK(a != NULL && b != NULL);
		if (a && b) {
			int c;
			while ((c = fgetc(a)) != EOF) { fputc(c, b); }
		}
		if (a) fclose(a);
		if (b) fclose(b);
	}
	{
		hsm_token_t *t = hsm_token_new(N_SLOTS);
		CHECK(t != NULL);
		/* 旧快照的 MAC 是对的，但 epoch(1) < 锚点(3) → 必须拒 */
		CHECK_EQ_INT(hsm_keystore_load(t, g_path), HSM_ERR_INTEGRITY);
		hsm_token_free(t);
	}

	/* ---- 反面：把锚点也一起换回去，就挡不住了 ----
	 * 这一步不是"漏洞"，是**如实把边界画出来**：本机制要求攻击者
	 * 一致地换两个文件，而不是不可绕过。 */
	{
		FILE *f = fopen(epoch_path, "w");
		CHECK(f != NULL);
		if (f) { fprintf(f, "1\n"); fclose(f); }
	}
	{
		hsm_token_t *t = hsm_token_new(N_SLOTS);
		CHECK(t != NULL);
		CHECK_EQ_INT(hsm_keystore_load(t, g_path), HSM_OK);
		hsm_token_free(t);
	}

	unlink(snap);
	unlink(epoch_path);
	unlink(g_path);
}

int main(void)
{
	snprintf(g_path, sizeof(g_path), "/tmp/pqchsm_ks_%d.bin", (int)getpid());

	test_roundtrip();
	test_tamper();
	test_power_fail();
	test_rollback();

	unlink(g_path);
	char tmp[340];
	snprintf(tmp, sizeof(tmp), "%s.tmp", g_path);
	unlink(tmp);
	snprintf(tmp, sizeof(tmp), "%s.epoch", g_path);
	unlink(tmp);

	TCASE("非法参数");
	CHECK_EQ_INT(hsm_keystore_save(NULL, g_path), HSM_ERR_BAD_ARG);
	CHECK_EQ_INT(hsm_keystore_load(NULL, g_path), HSM_ERR_BAD_ARG);
	{
		hsm_token_t *t = hsm_token_new(1);
		CHECK_EQ_INT(hsm_keystore_save(t, NULL), HSM_ERR_BAD_ARG);
		CHECK_EQ_INT(hsm_keystore_load(t, "/nonexistent/dir/ks.bin"), HSM_ERR_BAD_ARG);
		hsm_token_free(t);
	}

	return test_report("test_keystore");
}
