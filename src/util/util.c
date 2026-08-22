#include "pqchsm/util.h"

#include "pqchsm/hwrng.h"

#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

void pqc_secure_zero(void *p, size_t n)
{
	if (p && n) {
		OPENSSL_cleanse(p, n);
	}
}

int pqc_ct_equal(const void *a, const void *b, size_t n)
{
	const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
	uint8_t diff = 0;
	if (!x || !y) {
		return 0;
	}
	for (size_t i = 0; i < n; i++) {
		diff |= (uint8_t)(x[i] ^ y[i]);
	}
	return diff == 0;
}

int pqc_random_bytes(uint8_t *out, size_t n)
{
	if (!out || n == 0 || n > INT_MAX) {
		return -1;
	}
	/* 装了**真硬件**熵源就走它，**并且不回退**：取不到就返回错误，让上层
	 * 决定停机还是降级。静默回退到 OpenSSL 会让"熵来自硬件"变成一句假话，
	 * 而调用方无从知道。没装硬件源时照旧走软件源。
	 *
	 * ⚠️ **判据是 hwrng_is_hardware()，不是 hwrng_available()。**（PS-25）
	 * 两者的差别正好落在最坏的地方：`hwrng_available()` 对**桩** transport
	 * 也返回真（它只问"装了没有"），而桩的字节来自软件。于是老写法在装了
	 * 桩的进程里会：
	 *   ① 绕开 OpenSSL 的 RAND_bytes，改用桩那条**没有经过任何熵源评估**的
	 *      路径去产密钥材料；
	 *   ② 同时让上层"我们走的是硬件熵源"这句话看起来成立。
	 * 两条叠起来就是"以为更强、其实更弱，而且没人看得出来"。
	 * is_hardware 是 transport 自己如实填的那一位（见 hwrng.h），桩填 0。 */
	if (hwrng_is_hardware()) {
		return hwrng_bytes(out, n) == HWRNG_OK ? 0 : -1;
	}
	return RAND_bytes(out, (int)n) == 1 ? 0 : -1;
}

void *pqc_secure_alloc(size_t n)
{
	if (n == 0) {
		return NULL;
	}
	void *p = malloc(n);
	if (!p) {
		return NULL;
	}
	/* 先清零再锁页。顺序不是随意的：glibc 把 mlock 声明成读 const void *，
	 * 反过来写（先 mlock 后 memset）会让 GCC 报 -Wmaybe-uninitialized
	 * ——它说的是"被指内存未初始化"，不是指针未初始化。
	 * 这是在 aarch64 Linux + GCC 12 上跑回归才发现的，clang 不报。 */
	memset(p, 0, n);
	/* best-effort：macOS/容器里常因 RLIMIT_MEMLOCK 失败，不作为错误。
	 * 真正不可妥协的是 free 时的清零。 */
	(void)mlock(p, n);
	return p;
}

void pqc_secure_free(void *p, size_t n)
{
	if (!p) {
		return;
	}
	pqc_secure_zero(p, n);
	(void)munlock(p, n);
	free(p);
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

long pqc_hex_decode(const char *hex, size_t hex_len, uint8_t *out, size_t out_cap)
{
	if (!hex) {
		return -1;
	}
	if (hex_len == (size_t)-1) {
		hex_len = strlen(hex);
	}
	if (hex_len % 2u) {
		return -1;
	}
	size_t n = hex_len / 2u;
	if (n > out_cap) {
		return -1;
	}
	for (size_t i = 0; i < n; i++) {
		int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
		if (hi < 0 || lo < 0) {
			return -1;
		}
		out[i] = (uint8_t)((hi << 4) | lo);
	}
	return (long)n;
}

int pqc_hex_encode(const uint8_t *in, size_t n, char *out, size_t out_cap)
{
	static const char d[] = "0123456789abcdef";
	if (!in || !out || out_cap < 2 * n + 1) {
		return -1;
	}
	for (size_t i = 0; i < n; i++) {
		out[2 * i]     = d[in[i] >> 4];
		out[2 * i + 1] = d[in[i] & 0x0f];
	}
	out[2 * n] = '\0';
	return 0;
}
