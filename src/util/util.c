#include "pqchsm/util.h"

#include <openssl/crypto.h>
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
