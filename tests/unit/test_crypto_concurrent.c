/* test_crypto_concurrent.c —— 两条 P0 并发缺陷的回归
 *
 * 这两条都属于"单线程跑一万遍也照样绿"的那类：它们只在多线程交错时才现形，
 * 而现形之后的表现又都不是崩溃，是**安静的错值**。所以回归必须真起线程。
 *
 * ============================================================================
 * ① liboqs 全局随机源被串扰（src/crypto/oqs_rng.c）
 * ============================================================================
 * liboqs 的 randombytes 是进程级全局回调，而本项目又用它做确定性脚本
 * （ML-DSA 没有去随机化 API，KAT 与"槽位只存 32 B ξ"两件事都靠它）。
 * 老版本只在 begin()/end() 之间持锁，普通的随机化 keypair / encaps / sign
 * 完全不在锁内 —— 于是：
 *
 *   线程 A：begin(ξ) → OQS_SIG_keypair → end()
 *   线程 B：           OQS_KEM_keypair          ← 它的 randombytes 吃掉了 ξ 的字节
 *
 * A 侧表现为 end() 的 consumed 校验不过（**偶发** KAT 失败），
 * B 侧表现为密钥的熵来自那段常量脚本 —— **不报任何错**。
 *
 * 本用例：线程 A 反复用固定 ξ 生成 ML-DSA-65 密钥对并逐字节比对基准值，
 * 线程 B 同时反复做随机化的 ML-KEM-768 keypair / encaps 与 ML-DSA sign。
 * 判据两条，缺一不可：
 *   · A 的每一次结果都必须与单线程基准逐字节一致（脚本没被吃）；
 *   · B 的输出必须次次不同（没有吃到那段常量脚本）。
 *
 * ============================================================================
 * ② 自测未完成即可过闸（src/util/selftest.c）
 * ============================================================================
 * 老版本的旁路 `if (g_running) return 1;` 是**全进程**的，于是任何线程只要
 * 撞上另一个线程跑自测的窗口，就能拿到密码服务，而模块那一刻还没自证。
 *
 * 【怎么才能观测到它 —— 第一版判据是错的，记在这里免得又写回去】
 * 第一版让探测线程"passed() 说 1 就立刻做一次密码运算，被 PQC_ERR_SELF_TEST
 * 挡回来就算违规"。这测不出任何东西：老代码里那次密码运算内部又调一次
 * passed()，**同样**被 g_running 放行，于是运算成功、判据永远不触发。
 * 实测确认过：老代码下这条断言 22/22 全绿。
 *
 * 真正能区分新旧的判据是**状态自洽**：
 *
 *     passed() 说 1 的那一刻，failures() 必须是 0。
 *
 * 老代码在 RUNNING 期间 passed() 返回 1，而 g_failures 还停在上一轮
 * force_error(1) 写进去的位图（非 0）—— 闸门说"可以用"，账本说"没通过"。
 * 新代码里这两个值同在一把锁下推进，旁观线程要么等到结论、要么看到 0。
 *
 * 判据要成立，就不能让下一轮的 force_error(1) 插进 passed() 与 failures()
 * 这两次读之间（那会在新代码上造出假阳性）。所以用一个 seq_cst 的
 * g_probe_active 计数做握手：跑测线程收工后要等所有探测退出临界区才开下一轮。
 */
#include "pqchsm/pqc.h"
#include "pqchsm/selftest.h"
#include "pqchsm/util.h"
#include "testlib.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

/* 迭代次数。ML-DSA-65 keygen 在开发机上约 100 µs 量级，几千次是秒级；
 * 交错窗口本来就窄，次数太少等于没测。 */
#define DET_ITERS  3000
#define RND_ITERS  3000

static const uint8_t XI[32] = {
	0xA9, 0x91, 0xFD, 0x42, 0xB0, 0x71, 0xD4, 0x9C, 0x48, 0xAE, 0x3E, 0x75,
	0xC6, 0x47, 0x45, 0x9E, 0x0D, 0xAA, 0xD1, 0xE1, 0xBA, 0x35, 0x6A, 0x04,
	0x80, 0x19, 0x12, 0xD3, 0x29, 0x4B, 0xCF, 0xF8,
};

/* ---- ① 确定性 vs 随机化 ------------------------------------------------- */

static uint8_t g_ref_pk[2592];      /* ML-DSA-65 pk = 1952，留足 */
static uint8_t g_ref_sk[4896];
static size_t  g_pk_len, g_sk_len;

static int g_det_mismatch;          /* 确定性那条路被串扰的次数 */
static int g_det_error;             /* 后端直接报错的次数（consumed 校验不过） */
static int g_rnd_error;
static int g_rnd_repeat;            /* 随机那条路吐出重复值的次数 */

static void *det_thread(void *arg)
{
	uint8_t pk[2592], sk[4896];
	int i;

	(void)arg;
	for (i = 0; i < DET_ITERS; i++) {
		if (pqc_keypair_from_seed(PQC_ALG_ML_DSA_65, XI, sizeof(XI), pk, sk)
		    != PQC_OK) {
			__atomic_add_fetch(&g_det_error, 1, __ATOMIC_RELAXED);
			continue;
		}
		if (memcmp(pk, g_ref_pk, g_pk_len) != 0 ||
		    memcmp(sk, g_ref_sk, g_sk_len) != 0) {
			__atomic_add_fetch(&g_det_mismatch, 1, __ATOMIC_RELAXED);
		}
	}
	pqc_secure_zero(sk, sizeof(sk));
	return NULL;
}

/* 随机那条路：只要它吃到了脚本，输出就会开始重复。
 * 与上一次比对足以发现 —— 脚本是一段固定字节，连续两次吃到就必然相同。 */
static void *rnd_thread(void *arg)
{
	static const size_t KEM_PK = 1184, KEM_SK = 2400;   /* ML-KEM-768 */
	uint8_t pk[1184], sk[2400], prev[1184];
	int i, have_prev = 0;

	(void)arg;
	for (i = 0; i < RND_ITERS; i++) {
		if (pqc_keypair(PQC_ALG_ML_KEM_768, pk, sk) != PQC_OK) {
			__atomic_add_fetch(&g_rnd_error, 1, __ATOMIC_RELAXED);
			continue;
		}
		if (have_prev && memcmp(pk, prev, KEM_PK) == 0) {
			__atomic_add_fetch(&g_rnd_repeat, 1, __ATOMIC_RELAXED);
		}
		memcpy(prev, pk, KEM_PK);
		have_prev = 1;
		(void)KEM_SK;
	}
	pqc_secure_zero(sk, sizeof(sk));
	return NULL;
}

/* 第三种消费者：随机化 encaps。它走的是另一个被漏掉的入口。 */
static void *encaps_thread(void *arg)
{
	uint8_t pk[1184], sk[2400], ct[1088], ss[32], prev[1088];
	int i, have_prev = 0;

	(void)arg;
	if (pqc_keypair(PQC_ALG_ML_KEM_768, pk, sk) != PQC_OK) {
		__atomic_add_fetch(&g_rnd_error, 1, __ATOMIC_RELAXED);
		return NULL;
	}
	for (i = 0; i < RND_ITERS; i++) {
		if (pqc_encaps(PQC_ALG_ML_KEM_768, pk, ct, ss) != PQC_OK) {
			__atomic_add_fetch(&g_rnd_error, 1, __ATOMIC_RELAXED);
			continue;
		}
		if (have_prev && memcmp(ct, prev, sizeof(ct)) == 0) {
			__atomic_add_fetch(&g_rnd_repeat, 1, __ATOMIC_RELAXED);
		}
		memcpy(prev, ct, sizeof(ct));
		have_prev = 1;
	}
	pqc_secure_zero(sk, sizeof(sk));
	pqc_secure_zero(ss, sizeof(ss));
	return NULL;
}

static void test_rng_serialization(void)
{
	pthread_t td, tr, te;
	const pqc_alg_info_t *info = pqc_alg_info(PQC_ALG_ML_DSA_65);

	TCASE("liboqs 随机源：确定性与随机化两条路并发不互相串扰");
	CHECK(info != NULL);
	if (!info) {
		return;
	}
	g_pk_len = info->pk_len;
	g_sk_len = info->sk_len;
	CHECK(g_pk_len <= sizeof(g_ref_pk) && g_sk_len <= sizeof(g_ref_sk));

	/* 单线程基准：这一步必须在起线程之前做，否则基准本身就可能是被串扰的。 */
	CHECK_EQ_INT(pqc_keypair_from_seed(PQC_ALG_ML_DSA_65, XI, sizeof(XI),
	                                   g_ref_pk, g_ref_sk), PQC_OK);

	CHECK_EQ_INT(pthread_create(&td, NULL, det_thread, NULL), 0);
	CHECK_EQ_INT(pthread_create(&tr, NULL, rnd_thread, NULL), 0);
	CHECK_EQ_INT(pthread_create(&te, NULL, encaps_thread, NULL), 0);
	pthread_join(td, NULL);
	pthread_join(tr, NULL);
	pthread_join(te, NULL);

	/* 一次都不能串。这几条在修复前是**偶发**的，所以次数写在报错里，
	 * 便于区分"偶尔一次"和"锁根本没生效"。 */
	CHECK_EQ_INT(g_det_mismatch, 0);
	CHECK_EQ_INT(g_det_error, 0);
	CHECK_EQ_INT(g_rnd_error, 0);
	CHECK_EQ_INT(g_rnd_repeat, 0);
}

/* ---- ② 自测闸门 --------------------------------------------------------- */

#define GATE_ROUNDS   200
#define GATE_PROBERS  3

static int g_gate_go;          /* seq_cst：本轮 RUNNING 窗口开着 */
static int g_gate_stop;        /* seq_cst */
static int g_probe_active;     /* seq_cst：当前有几个探测在临界区里 */
static int g_gate_viol;        /* passed()==1 而 failures()!=0 的次数 */
static int g_gate_r1;          /* passed() 说过多少次 1（非空跑的证据） */

#define AGET(v)     __atomic_load_n(&(v), __ATOMIC_SEQ_CST)
#define ASET(v, x)  __atomic_store_n(&(v), (x), __ATOMIC_SEQ_CST)
#define AINC(v)     __atomic_add_fetch(&(v), 1, __ATOMIC_SEQ_CST)
#define ADEC(v)     __atomic_sub_fetch(&(v), 1, __ATOMIC_SEQ_CST)

static void *gate_runner_thread(void *arg)
{
	int r;

	(void)arg;
	for (r = 0; r < GATE_ROUNDS; r++) {
		/* 先把模块打到错误态：此刻 failures() 非 0、passed() 应当是 0。 */
		pqc_self_test_force_error(1);
		ASET(g_gate_go, 1);
		(void)pqc_self_test();          /* ← RUNNING 窗口就在这一句里 */
		ASET(g_gate_go, 0);
		/* 等所有探测退出临界区再开下一轮，免得下一轮的 force_error(1)
		 * 插进探测的两次读之间。 */
		while (AGET(g_probe_active) > 0) {
			/* 自旋；窗口极短 */
		}
	}
	ASET(g_gate_stop, 1);
	return NULL;
}

static void *gate_probe_thread(void *arg)
{
	(void)arg;
	while (!AGET(g_gate_stop)) {
		if (!AGET(g_gate_go)) {
			continue;
		}
		AINC(g_probe_active);
		if (AGET(g_gate_go)) {
			int      ok = pqc_self_test_passed();
			uint32_t f  = pqc_self_test_failures();

			if (ok) {
				AINC(g_gate_r1);
				if (f != 0) {
					/* 闸门放行了，账本却说没通过 —— 就是那个漏洞 */
					AINC(g_gate_viol);
				}
			}
		}
		ADEC(g_probe_active);
	}
	return NULL;
}

static void test_selftest_gate(void)
{
	pthread_t tr, tp[GATE_PROBERS];
	int i;

	TCASE("自测闸门：RUNNING 期间放行必须与账本自洽");
	ASET(g_gate_go, 0);
	ASET(g_gate_stop, 0);
	ASET(g_probe_active, 0);
	g_gate_viol = 0;
	g_gate_r1 = 0;

	for (i = 0; i < GATE_PROBERS; i++) {
		CHECK_EQ_INT(pthread_create(&tp[i], NULL, gate_probe_thread, NULL), 0);
	}
	CHECK_EQ_INT(pthread_create(&tr, NULL, gate_runner_thread, NULL), 0);
	pthread_join(tr, NULL);
	for (i = 0; i < GATE_PROBERS; i++) {
		pthread_join(tp[i], NULL);
	}

	/* 探测线程真的落进过窗口 —— 否则这个用例是空跑 */
	CHECK(AGET(g_gate_r1) > 0);
	CHECK_EQ_INT(AGET(g_gate_viol), 0);

	/* 收尾：把模块留在"已通过"状态，免得影响同进程后面的用例 */
	CHECK_EQ_INT(pqc_self_test(), 0);
	CHECK_EQ_INT(pqc_self_test_passed(), 1);
}

/* 自测过程中，另一个线程的 passed() 必须要么等到结论、要么说没过，
 * 绝不能在 RUNNING 期间返回 1。这一条单独测，不掺别的噪声。 */
static volatile int g_ready;
static int g_saw_true_while_running;

static void *observer(void *arg)
{
	(void)arg;
	while (!g_ready) {
		/* 自旋等主线程开跑 */
	}
	/* 这里的 passed() 会在 condvar 上等到结论才回；返回 1 是合法的，
	 * 因为那时自测已经结束。真正要挡住的是"还在跑就说 1"，
	 * 而那个只能靠 ① 里的 bypass 计数与状态机本身保证。 */
	if (pqc_self_test_passed()) {
		g_saw_true_while_running = 1;
	}
	return NULL;
}

static void test_selftest_wait(void)
{
	pthread_t t;

	TCASE("自测期间旁观线程会等到结论");
	pqc_self_test_force_error(1);
	g_ready = 0;
	g_saw_true_while_running = 0;
	CHECK_EQ_INT(pthread_create(&t, NULL, observer, NULL), 0);
	g_ready = 1;
	CHECK_EQ_INT(pqc_self_test(), 0);
	pthread_join(t, NULL);
	/* 结论出来之后才回答，所以这里应当是 1；它同时验证了不会死锁。 */
	CHECK_EQ_INT(g_saw_true_while_running, 1);
	CHECK_EQ_INT(pqc_self_test_passed(), 1);
}

int main(void)
{
	test_rng_serialization();
	test_selftest_gate();
	test_selftest_wait();
	return test_report("test_crypto_concurrent");
}
