/* 并发压测（+ 验收项「多会话同时用不同槽位/抢同一槽位」）
 *
 * 两个场景：
 *   A. 不同线程用不同槽位 —— 应当真正并行，且各槽位计数精确；
 *   B. 多线程抢同一槽位   —— 每次操作要么完整成功要么干净失败，
 *      绝不能出现元数据 KMAC 校验失败（那意味着有人看到了半更新的状态）。
 */
#include "testlib.h"
#include "pqchsm/slot.h"

#include <pthread.h>
#include <stdlib.h>

#define SO_PIN   "so-secret-0001"
#define USER_PIN "user-pin-4242"

#define N_SLOTS   4
#define N_THREADS 8
#define N_ITERS   40

typedef struct {
	hsm_token_t  *tok;
	hsm_slot_id_t slot;
	int           ok_sign;
	int           integrity_fail;
	int           other_fail;
} worker_arg_t;

static void provision(hsm_token_t *tok, hsm_slot_id_t slot, hsm_handle_t *h)
{
	hsm_session_t s;
	if (hsm_slot_init_token(tok, slot, "concurrent", SO_PIN) != HSM_OK ||
	    hsm_session_open(tok, slot, &s) != HSM_OK ||
	    hsm_session_login(tok, s, HSM_ROLE_SO, SO_PIN) != HSM_OK ||
	    hsm_slot_set_user_pin(tok, s, USER_PIN) != HSM_OK ||
	    hsm_session_logout(tok, s) != HSM_OK ||
	    hsm_session_login(tok, s, HSM_ROLE_USER, USER_PIN) != HSM_OK ||
	    hsm_slot_generate(tok, s, PQC_ALG_ML_DSA_44, KEY_USAGE_SIGN, 0, h) != HSM_OK) {
		abort();
	}
	if (hsm_session_close(tok, s) != HSM_OK) {
		abort();
	}
}

/* 每个线程开自己的会话，反复签名 */
static void *worker(void *p)
{
	worker_arg_t *a = (worker_arg_t *)p;
	const pqc_alg_info_t *info = pqc_alg_info(PQC_ALG_ML_DSA_44);
	uint8_t *sig = malloc(info->sig_len);
	if (!sig) {
		return NULL;
	}

	hsm_session_t sess;
	if (hsm_session_open(a->tok, a->slot, &sess) != HSM_OK) {
		free(sig);
		return NULL;
	}
	if (hsm_session_login(a->tok, sess, HSM_ROLE_USER, USER_PIN) != HSM_OK) {
		hsm_session_close(a->tok, sess);
		free(sig);
		return NULL;
	}

	/* 句柄由本线程自行推导：generation 为 0 之外的值时会失败，
	 * 所以用 get_meta 拿当前 generation 组出句柄 */
	slot_meta_t m;
	if (hsm_slot_get_meta(a->tok, a->slot, &m) != HSM_OK) {
		hsm_session_close(a->tok, sess);
		free(sig);
		return NULL;
	}
	hsm_handle_t h = ((hsm_handle_t)m.generation << 32) | (hsm_handle_t)(a->slot + 1);

	const uint8_t msg[] = "concurrent-sign";
	for (int i = 0; i < N_ITERS; i++) {
		size_t sig_len = 0;
		hsm_status_t st = hsm_object_sign(a->tok, sess, h, msg, sizeof(msg), NULL, 0,
		                                  sig, info->sig_len, &sig_len);
		if (st == HSM_OK) {
			a->ok_sign++;
		} else if (st == HSM_ERR_INTEGRITY) {
			a->integrity_fail++;    /* 这一项必须恒为 0 */
		} else {
			a->other_fail++;
		}
		/* 顺带读元数据，制造读写交错 */
		slot_meta_t snap;
		if (hsm_slot_get_meta(a->tok, a->slot, &snap) == HSM_ERR_INTEGRITY) {
			a->integrity_fail++;
		}
	}
	hsm_session_close(a->tok, sess);
	free(sig);
	return NULL;
}

static void scenario_distinct_slots(void)
{
	TCASE("并发：N 个线程各用各的槽位");
	hsm_token_t *tok = hsm_token_new(N_SLOTS);
	CHECK(tok != NULL);
	hsm_handle_t h;
	for (int i = 0; i < N_SLOTS; i++) {
		provision(tok, (hsm_slot_id_t)i, &h);
	}

	pthread_t th[N_SLOTS];
	worker_arg_t args[N_SLOTS];
	memset(args, 0, sizeof(args));
	for (int i = 0; i < N_SLOTS; i++) {
		args[i].tok = tok;
		args[i].slot = (hsm_slot_id_t)i;
		CHECK_EQ_INT(pthread_create(&th[i], NULL, worker, &args[i]), 0);
	}
	for (int i = 0; i < N_SLOTS; i++) {
		pthread_join(th[i], NULL);
	}

	for (int i = 0; i < N_SLOTS; i++) {
		CHECK_EQ_INT(args[i].integrity_fail, 0);
		CHECK_EQ_INT(args[i].other_fail, 0);
		CHECK_EQ_INT(args[i].ok_sign, N_ITERS);
		/* 各槽位使用计数必须精确等于本线程的成功次数 —— 没有丢失更新 */
		slot_meta_t m;
		CHECK_EQ_INT(hsm_slot_get_meta(tok, (hsm_slot_id_t)i, &m), HSM_OK);
		CHECK_EQ_INT(m.use_count, N_ITERS);
		CHECK_EQ_INT(m.state, SLOT_ST_LOADED);
	}
	hsm_token_free(tok);
}

static void scenario_same_slot(void)
{
	TCASE("并发：N 个线程抢同一个槽位");
	hsm_token_t *tok = hsm_token_new(1);
	CHECK(tok != NULL);
	hsm_handle_t h;
	provision(tok, 0, &h);

	pthread_t th[N_THREADS];
	worker_arg_t args[N_THREADS];
	memset(args, 0, sizeof(args));
	for (int i = 0; i < N_THREADS; i++) {
		args[i].tok = tok;
		args[i].slot = 0;
		CHECK_EQ_INT(pthread_create(&th[i], NULL, worker, &args[i]), 0);
	}
	for (int i = 0; i < N_THREADS; i++) {
		pthread_join(th[i], NULL);
	}

	int total_ok = 0;
	for (int i = 0; i < N_THREADS; i++) {
		/* 关键断言：抢同一槽位时也绝不能看到半更新的元数据 */
		CHECK_EQ_INT(args[i].integrity_fail, 0);
		CHECK_EQ_INT(args[i].other_fail, 0);
		total_ok += args[i].ok_sign;
	}
	CHECK_EQ_INT(total_ok, N_THREADS * N_ITERS);

	/* 使用计数必须等于所有线程成功次数之和 —— 证明 use_count++ 没有丢失更新 */
	slot_meta_t m;
	CHECK_EQ_INT(hsm_slot_get_meta(tok, 0, &m), HSM_OK);
	CHECK_EQ_INT(m.use_count, total_ok);
	CHECK_EQ_INT(m.state, SLOT_ST_LOADED);
	hsm_token_free(tok);
}

/* 并发开关会话：会话表本身的竞争 */
static void *session_churn(void *p)
{
	hsm_token_t *tok = (hsm_token_t *)p;
	for (int i = 0; i < 200; i++) {
		hsm_session_t s;
		if (hsm_session_open(tok, 0, &s) == HSM_OK) {
			hsm_role_t r;
			(void)hsm_session_role(tok, s, &r);
			(void)hsm_session_login(tok, s, HSM_ROLE_USER, USER_PIN);
			(void)hsm_session_logout(tok, s);
			(void)hsm_session_close(tok, s);
		}
	}
	return NULL;
}

static void scenario_session_table(void)
{
	TCASE("并发：会话表开关竞争");
	hsm_token_t *tok = hsm_token_new(1);
	CHECK(tok != NULL);
	hsm_handle_t h;
	provision(tok, 0, &h);

	pthread_t th[N_THREADS];
	for (int i = 0; i < N_THREADS; i++) {
		CHECK_EQ_INT(pthread_create(&th[i], NULL, session_churn, tok), 0);
	}
	for (int i = 0; i < N_THREADS; i++) {
		pthread_join(th[i], NULL);
	}
	/* 全部关闭后应当能重新开满 MAX_SESSIONS 个（没有泄漏会话槽） */
	hsm_session_t s[16];
	int opened = 0;
	for (int i = 0; i < 16; i++) {
		if (hsm_session_open(tok, 0, &s[i]) == HSM_OK) {
			opened++;
		}
	}
	CHECK_EQ_INT(opened, 16);
	slot_meta_t m;
	CHECK_EQ_INT(hsm_slot_get_meta(tok, 0, &m), HSM_OK);
	hsm_token_free(tok);
}

/* ---- 回归 P1⑥：hsm_session_close 的 ABA/TOCTOU ---------------------------
 *
 * 老实现是两次取锁：
 *
 *     session_info(sess)          // 取锁验 generation、放锁
 *     ... 这一格里别人可以 close + open，把表项换成另一个会话 ...
 *     lock; memset(&sessions[idx-1]); unlock
 *
 * 后果有两条，第二条更要命：
 *   ① 同一个句柄被两个线程同时 close，**两次都会返回 HSM_OK** ——
 *      它们都在各自的第一段里通过了校验；
 *   ② 第二次那个 memset 抹掉的是**别人刚 open 出来的会话**。
 *      表现是"会话莫名其妙失效"，只在并发下偶发，查起来极贵。
 *
 * 这个用例直接钉 ①：N 个线程同时关同一个句柄，**成功的必须恰好一个**。
 * 同时另有一个"受害者"线程不停地 open/用/close 自己的会话，
 * 它的会话在自己 close 之前一次都不许失效（那就是 ②）。
 */
#define CLOSERS   4
#define CLOSE_ROUNDS 300

typedef struct {
	hsm_token_t  *tok;
	hsm_session_t target;
	int           ok;          /* 本轮 close 返回 HSM_OK 的次数（原子累加） */
	int           go;
	int           victim_lost; /* 受害者的会话被别人抹掉的次数 */
	int           stop;
} close_arg_t;

static close_arg_t g_ca;

static void *closer(void *p)
{
	(void)p;
	for (int r = 0; r < CLOSE_ROUNDS; r++) {
		int mygo;
		/* 等本轮开锣 */
		do {
			mygo = __atomic_load_n(&g_ca.go, __ATOMIC_SEQ_CST);
		} while (mygo != r + 1);
		if (hsm_session_close(g_ca.tok, g_ca.target) == HSM_OK) {
			__atomic_add_fetch(&g_ca.ok, 1, __ATOMIC_SEQ_CST);
		}
		__atomic_add_fetch(&g_ca.stop, 1, __ATOMIC_SEQ_CST);   /* 本轮我做完了 */
	}
	return NULL;
}

/* 受害者：一直开自己的会话、确认它还在、再自己关掉。
 * 它的会话被别人的 close 抹掉，就会表现为 role 查不到或 close 失败。 */
static void *victim(void *p)
{
	(void)p;
	while (__atomic_load_n(&g_ca.go, __ATOMIC_SEQ_CST) <= CLOSE_ROUNDS) {
		hsm_session_t s;
		hsm_role_t r;

		if (hsm_session_open(g_ca.tok, 0, &s) != HSM_OK) {
			continue;
		}
		if (hsm_session_role(g_ca.tok, s, &r) != HSM_OK ||
		    hsm_session_close(g_ca.tok, s) != HSM_OK) {
			__atomic_add_fetch(&g_ca.victim_lost, 1, __ATOMIC_SEQ_CST);
		}
	}
	return NULL;
}

static void scenario_close_race(void)
{
	pthread_t th[CLOSERS], tv;
	int r;

	TCASE("并发：同一句柄被多线程 close，成功的必须恰好一个");
	g_ca.tok = hsm_token_new(1);
	CHECK(g_ca.tok != NULL);
	g_ca.ok = 0;
	g_ca.go = 0;
	g_ca.victim_lost = 0;
	g_ca.stop = 0;

	for (int i = 0; i < CLOSERS; i++) {
		CHECK_EQ_INT(pthread_create(&th[i], NULL, closer, NULL), 0);
	}
	CHECK_EQ_INT(pthread_create(&tv, NULL, victim, NULL), 0);

	for (r = 0; r < CLOSE_ROUNDS; r++) {
		CHECK_EQ_INT(hsm_session_open(g_ca.tok, 0, &g_ca.target), HSM_OK);
		__atomic_store_n(&g_ca.stop, 0, __ATOMIC_SEQ_CST);
		__atomic_store_n(&g_ca.go, r + 1, __ATOMIC_SEQ_CST);
		while (__atomic_load_n(&g_ca.stop, __ATOMIC_SEQ_CST) < CLOSERS) {
			/* 等本轮四个 closer 都做完再开下一轮 */
		}
	}
	__atomic_store_n(&g_ca.go, CLOSE_ROUNDS + 1, __ATOMIC_SEQ_CST);
	for (int i = 0; i < CLOSERS; i++) {
		pthread_join(th[i], NULL);
	}
	pthread_join(tv, NULL);

	/* 每一轮只开了一个会话，所以总成功数必须等于轮数 */
	CHECK_EQ_INT(__atomic_load_n(&g_ca.ok, __ATOMIC_SEQ_CST), CLOSE_ROUNDS);
	CHECK_EQ_INT(__atomic_load_n(&g_ca.victim_lost, __ATOMIC_SEQ_CST), 0);
	hsm_token_free(g_ca.tok);
}

int main(void)
{
	test_use_stub_kdr();   /* 显式装桩：库里没有自动回退了 */
	scenario_distinct_slots();
	scenario_same_slot();
	scenario_session_table();
	scenario_close_race();
	return test_report("test_slot_concurrent");
}
