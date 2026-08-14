/* hwrng.c —— 硬件熵源驱动
 *
 * 严格照 docs/REGISTERS.zh-CN.md 的行为契约实现。那份文档是 PL 侧
 * trng_axi 与这里的共同约定，RTL 侧由 test_trng_axi.py 验证，软件侧由
 * tests/unit/test_hwrng.c 验证 —— 两边验的是同一张表。
 *
 * 【三条不能省的纪律】
 *
 * 1. **读 RDATA 之前必须先查 STATUS.DATA_VALID。** 空读会返回 0，而 0 是最糟
 *    的"随机数"。契约里留了 UNDERRUN 锁存位，就是为了事后抓住不查就读的驱动。
 *
 * 2. **取完一批之后复查 UNDERRUN 与 ALARM，任一置位则整批作废。** 不是"把出错
 *    之后的部分丢掉"，是整批清零 —— 告警是**锁存**的电平，只说明"这段时间里
 *    发生过"，不说明发生在第几个字上。既然定位不了，就只能全丢。
 *
 * 3. **绝不回退到软件随机源。** 拿不到硬件熵就返回错误，让上层决定停机还是
 *    降级。静默回退会让"熵来自硬件"变成一句假话，而调用方无从知道 ——
 *    这是密码机里最不能含糊的一条。
 *
 * 【为什么尾部字节直接丢弃】
 * 取 n 字节时最后可能多出 1~3 个字节。不缓存它们：留一小撮"上次剩下的随机数"
 * 在进程内存里，收益是每次调用省 3 个字节，代价是熵源与消费者之间多了一个
 * 有生命周期的状态（谁来清？zeroize 的时候清不清？fork 之后呢？）。
 * 这笔账不划算，丢掉更省事也更安全。
 */
#include "pqchsm/hwrng.h"

#include <pthread.h>
#include <string.h>

#include "pqchsm/util.h"

/* 轮询上限。真器件暖机要 STARTUP_SAMPLES×DECIM = 8192 个时钟（100 MHz 下
 * 约 82 µs），一个 rate 块要 1088 个样本。这里给的是**很宽的**上限：
 * 它要区分的是"慢"和"坏了"，不是精确计时。到了上限就报 TIMEOUT，
 * 不无限等 —— 硬件卡住时驱动跟着挂死，比报错难查得多。 */
#define HWRNG_POLL_LIMIT 100000

static const hwrng_transport_t *g_tr;
static pthread_mutex_t          g_lock = PTHREAD_MUTEX_INITIALIZER;

static inline uint32_t rd(uint32_t off)
{
	return g_tr->read_reg(off);
}

static inline void wr(uint32_t off, uint32_t val)
{
	g_tr->write_reg(off, val);
}

void hwrng_set_transport(const hwrng_transport_t *t)
{
	pthread_mutex_lock(&g_lock);
	g_tr = t;
	if (g_tr) {
		wr(HWRNG_REG_CTRL, HWRNG_CTRL_ENABLE);
	}
	pthread_mutex_unlock(&g_lock);
}

const hwrng_transport_t *hwrng_get_transport(void)
{
	return g_tr;
}

int hwrng_available(void)
{
	return g_tr != NULL;
}

int hwrng_is_hardware(void)
{
	return g_tr && g_tr->is_hardware;
}

uint32_t hwrng_status(void)
{
	if (!g_tr) {
		return 0;
	}
	pthread_mutex_lock(&g_lock);
	uint32_t s = rd(HWRNG_REG_STATUS);
	pthread_mutex_unlock(&g_lock);
	return s;
}

void hwrng_zeroize(void)
{
	if (!g_tr) {
		return;
	}
	pthread_mutex_lock(&g_lock);
	wr(HWRNG_REG_CTRL, HWRNG_CTRL_ENABLE | HWRNG_CTRL_ZEROIZE);
	pthread_mutex_unlock(&g_lock);
}

void hwrng_clear_alarm(void)
{
	if (!g_tr) {
		return;
	}
	pthread_mutex_lock(&g_lock);
	wr(HWRNG_REG_CTRL, HWRNG_CTRL_ENABLE | HWRNG_CTRL_CLEAR_ALARM);
	pthread_mutex_unlock(&g_lock);
}

/* 等 READY。调用方持锁。 */
static int wait_ready_locked(void)
{
	for (long i = 0; i < HWRNG_POLL_LIMIT; i++) {
		uint32_t s = rd(HWRNG_REG_STATUS);
		if (s & HWRNG_ST_ALARM) {
			return HWRNG_ERR_ALARM;
		}
		if (s & HWRNG_ST_READY) {
			return HWRNG_OK;
		}
	}
	return HWRNG_ERR_TIMEOUT;
}

int hwrng_selftest(void)
{
	if (!g_tr) {
		return HWRNG_ERR_ABSENT;
	}
	pthread_mutex_lock(&g_lock);

	int rc = HWRNG_OK;

	if (rd(HWRNG_REG_VERSION) != HWRNG_VERSION_EXPECTED) {
		rc = HWRNG_ERR_VERSION;
		goto out;
	}

	/* 参数回读比对。"改了 RTL 参数忘了改驱动"这类不一致最阴险的地方在于
	 * 一切看起来都正常 —— 健康检测照跑，只是判据已经不是标准要求的那个了。
	 * 所以要在初始化时就把它撞出来。 */
	uint32_t p0 = rd(HWRNG_REG_PARAM0);
	uint32_t p1 = rd(HWRNG_REG_PARAM1);
	uint32_t p2 = rd(HWRNG_REG_PARAM2);

	if (((p0 >> 24) & 0xffu) != HWRNG_EXPECT_DECIM ||
	    ((p0 >> 16) & 0xffu) != HWRNG_EXPECT_NUM_RO ||
	    ((p0 >> 8)  & 0xffu) != HWRNG_EXPECT_RATE_LANES ||
	    ( p0        & 0xffu) != HWRNG_EXPECT_OUT_LANES ||
	    ((p1 >> 16) & 0xffffu) != HWRNG_EXPECT_APT_CUTOFF ||
	    ( p1        & 0xffffu) != HWRNG_EXPECT_RCT_CUTOFF ||
	    ((p2 >> 16) & 0xffffu) != HWRNG_EXPECT_STARTUP_SAMPLES ||
	    ( p2        & 0xffffu) != HWRNG_EXPECT_APT_WINDOW) {
		rc = HWRNG_ERR_PARAM;
		goto out;
	}

	wr(HWRNG_REG_CTRL, HWRNG_CTRL_ENABLE);
	rc = wait_ready_locked();

out:
	pthread_mutex_unlock(&g_lock);
	return rc;
}

int hwrng_bytes(uint8_t *out, size_t n)
{
	if (!out || n == 0) {
		return HWRNG_ERR_ARG;
	}
	if (!g_tr) {
		return HWRNG_ERR_ABSENT;
	}

	pthread_mutex_lock(&g_lock);

	int rc = wait_ready_locked();
	if (rc != HWRNG_OK) {
		goto out;
	}

	size_t done = 0;
	while (done < n) {
		/* 纪律 1：先查 DATA_VALID，再读 RDATA */
		long i;
		uint32_t s = 0;
		for (i = 0; i < HWRNG_POLL_LIMIT; i++) {
			s = rd(HWRNG_REG_STATUS);
			if (s & HWRNG_ST_ALARM) {
				rc = HWRNG_ERR_ALARM;
				goto out;
			}
			if (s & HWRNG_ST_DATA_VALID) {
				break;
			}
		}
		if (i == HWRNG_POLL_LIMIT) {
			rc = HWRNG_ERR_TIMEOUT;
			goto out;
		}

		uint32_t w = rd(HWRNG_REG_RDATA);
		uint8_t  b[4];
		b[0] = (uint8_t)(w & 0xffu);
		b[1] = (uint8_t)((w >> 8) & 0xffu);
		b[2] = (uint8_t)((w >> 16) & 0xffu);
		b[3] = (uint8_t)((w >> 24) & 0xffu);

		size_t take = (n - done < 4) ? (n - done) : 4;
		memcpy(out + done, b, take);
		done += take;
		/* 尾部没用上的字节当场清掉，理由见文件头 */
		pqc_secure_zero(b, sizeof(b));
	}

	/* 纪律 2：复查。ALARM 与 UNDERRUN 都是锁存的，只说明"这段时间里发生过"，
	 * 定位不到具体是哪个字，所以只能整批作废。 */
	uint32_t s = rd(HWRNG_REG_STATUS);
	if (s & (HWRNG_ST_ALARM | HWRNG_ST_UNDERRUN)) {
		rc = HWRNG_ERR_ALARM;
	}

out:
	if (rc != HWRNG_OK) {
		pqc_secure_zero(out, n);
	}
	pthread_mutex_unlock(&g_lock);
	return rc;
}
