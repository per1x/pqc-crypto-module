/* oqs_rng.c —— 给 liboqs 注入随机源，并把所有消费它的路径串行化
 *
 * 为什么需要脚本模式：liboqs 对 ML-DSA **不提供**去随机化 API（对比 ML-KEM 的
 * keypair_derand / encaps_derand）。而两件事都要求确定性：
 *   1. ACVP KAT：keyGen 由 ξ 定、sigGen 由 rnd 定；
 *   2. 生产路径：种子存储优化 —— 槽位只存 32 B ξ，装载时重展开。
 *
 * 做法：用 OQS_randombytes_custom_algorithm 挂一个"脚本化"随机源。
 * 进入脚本模式时提供一段字节流，liboqs 内部的 randombytes() 调用按序消费它。
 *
 * 【并发 —— 这一段是 2026-08-18 的修复，别退回去】
 * 随机源与脚本状态都是**进程级全局**的。老版本只在 begin()/end() 之间持锁，
 * 于是普通（随机化）的 OQS_KEM_keypair / OQS_KEM_encaps / OQS_SIG_keypair /
 * OQS_SIG_sign 完全没有被这把锁覆盖 —— 另一个线程在脚本区间里做这些操作，
 * 它的 randombytes 会落进同一个回调，把脚本吃掉。
 *
 * 两边的症状都很难查：
 *   · 确定性那条路少吃了字节 → end() 的 consumed 校验发现 → KAT **偶发**失败；
 *   · 随机那条路吃到的是**脚本里的常量** → 不报任何错，只是密钥不再随机。
 * 后者是安全事故，且不会有任何人发现。
 *
 * 现在的规矩：**所有会让 liboqs 调 randombytes 的调用，一律在这把锁里做**
 * （见 pqc_liboqs.c 里的 RNG_GUARD）。锁是递归的，回调自身也再上一次 ——
 * 于是即便将来出现一条没被包起来的路径，它仍然是串行的，只是看不到脚本。
 * 脚本按**开启它的线程**认领（g_script_owner），拿错脚本这件事在结构上不可能。
 */
#include "oqs_rng.h"

#include <oqs/oqs.h>
#include <openssl/rand.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "pqchsm/hwrng.h"
#include "pqchsm/util.h"

static pthread_mutex_t g_lock;
static pthread_once_t  g_once = PTHREAD_ONCE_INIT;

static const uint8_t *g_script;      /* 非 NULL 表示处于脚本模式 */
static size_t         g_script_len;
static size_t         g_script_used;
static int            g_overrun;     /* 脚本耗尽后仍被索取 → 置位 */
static pthread_t      g_script_owner;

/* 回调自身也上锁：见文件头。锁是递归的，所以被 RNG_GUARD 包住的调用
 * 走到这里只是把计数 +1，没有额外代价。 */
static void rng_callback(uint8_t *out, size_t n)
{
	int scripted;

	pqc_oqs_rng_lock();
	scripted = (g_script != NULL) && pthread_equal(g_script_owner, pthread_self());
	if (scripted) {
		if (g_script_used + n <= g_script_len) {
			memcpy(out, g_script + g_script_used, n);
			g_script_used += n;
			pqc_oqs_rng_unlock();
			return;
		}
		/* 脚本不够用：说明对后端随机数消费模型的假设不成立。
		 * 记下来让 end() 报错，绝不能悄悄用真随机数糊过去。 */
		g_overrun = 1;
	}
	pqc_oqs_rng_unlock();

	/* 装了硬件熵源就走它。**不回退到 OpenSSL** —— 硬件熵源出故障时悄悄换回
	 * 软件源，等于让"密钥的熵来自 PL 里那颗 TRNG"这句话在故障时静默失效，
	 * 而没有任何人会发现。这里没有返回值可用（liboqs 的回调签名是 void），
	 * 所以只能崩：宁可崩也不能返回可预测数据。 */
	if (hwrng_available()) {
		if (hwrng_bytes(out, n) != HWRNG_OK) {
			abort();
		}
		return;
	}
	if (RAND_bytes(out, (int)n) != 1) {
		/* 拿不到随机数是不可恢复的错误，宁可崩也不能返回可预测数据 */
		abort();
	}
}

static void rng_install(void)
{
	pthread_mutexattr_t attr;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&g_lock, &attr);
	pthread_mutexattr_destroy(&attr);

	OQS_randombytes_custom_algorithm(rng_callback);
}

void pqc_oqs_rng_init(void)
{
	pthread_once(&g_once, rng_install);
}

void pqc_oqs_rng_lock(void)
{
	pqc_oqs_rng_init();
	pthread_mutex_lock(&g_lock);
}

void pqc_oqs_rng_unlock(void)
{
	pthread_mutex_unlock(&g_lock);
}

int pqc_oqs_rng_begin(const uint8_t *script, size_t len)
{
	if (!script || len == 0) {
		return -1;
	}
	pqc_oqs_rng_lock();
	/* 递归锁意味着同一线程可以嵌套 begin()。那会让内层的 end() 提前把脚本
	 * 撤掉，而外层还以为自己在脚本模式里 —— 与其留这个陷阱，不如直接拒绝。 */
	if (g_script) {
		pqc_oqs_rng_unlock();
		return -1;
	}
	g_script       = script;
	g_script_len   = len;
	g_script_used  = 0;
	g_overrun      = 0;
	g_script_owner = pthread_self();
	return 0;
}

int pqc_oqs_rng_end(size_t *consumed)
{
	int overrun = g_overrun;
	if (consumed) {
		*consumed = g_script_used;
	}
	g_script      = NULL;
	g_script_len  = 0;
	g_script_used = 0;
	g_overrun     = 0;
	pqc_oqs_rng_unlock();
	return overrun ? -1 : 0;
}
