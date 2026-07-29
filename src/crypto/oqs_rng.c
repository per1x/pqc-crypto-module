/* oqs_rng.c —— 给 liboqs 注入确定性随机源
 *
 * 为什么需要：liboqs 对 ML-DSA **不提供**去随机化 API（对比 ML-KEM 的
 * keypair_derand / encaps_derand）。而两件事都要求确定性：
 *   1. ACVP KAT：keyGen 由 ξ 定、sigGen 由 rnd 定（路线图 §10.1）；
 *   2. 生产路径：路线图 §7.6 的种子存储优化 —— 槽位只存 32 B ξ，装载时重展开。
 *
 * 做法：用 OQS_randombytes_custom_algorithm 挂一个"脚本化"随机源。
 * 进入脚本模式时提供一段字节流，liboqs 内部的 randombytes() 调用按序消费它。
 *
 * 并发：随机源是 liboqs 的进程级全局状态，因此 begin/end 之间持锁，
 * 脚本化操作在进程内串行。上层（槽位管理器）对同一算法核本来就要串行调度
 * （路线图 §7.4「算法核是共享资源，调度器按槽位分时复用」），语义一致。
 */
#include "oqs_rng.h"

#include <oqs/oqs.h>
#include <openssl/rand.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "pqchsm/util.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t  g_once = PTHREAD_ONCE_INIT;

static const uint8_t *g_script;      /* 非 NULL 表示处于脚本模式 */
static size_t         g_script_len;
static size_t         g_script_used;
static int            g_overrun;     /* 脚本耗尽后仍被索取 → 置位 */

static void rng_callback(uint8_t *out, size_t n)
{
	if (g_script) {
		if (g_script_used + n <= g_script_len) {
			memcpy(out, g_script + g_script_used, n);
			g_script_used += n;
			return;
		}
		/* 脚本不够用：说明我们对后端的随机数消费模型判断错了。
		 * 记下来让 end() 报错，绝不能悄悄用真随机数糊过去。 */
		g_overrun = 1;
	}
	if (RAND_bytes(out, (int)n) != 1) {
		/* 拿不到随机数是不可恢复的错误，宁可崩也不能返回可预测数据 */
		abort();
	}
}

static void rng_install(void)
{
	OQS_randombytes_custom_algorithm(rng_callback);
}

void pqc_oqs_rng_init(void)
{
	pthread_once(&g_once, rng_install);
}

int pqc_oqs_rng_begin(const uint8_t *script, size_t len)
{
	if (!script || len == 0) {
		return -1;
	}
	pqc_oqs_rng_init();
	pthread_mutex_lock(&g_lock);
	g_script      = script;
	g_script_len  = len;
	g_script_used = 0;
	g_overrun     = 0;
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
	pthread_mutex_unlock(&g_lock);
	return overrun ? -1 : 0;
}
