#include "ta_random.h"

#if defined(PQCHSM_TA_OPTEE)

#include <tee_api.h>

void pqchsm_randombytes(uint8_t *out, size_t len)
{
	if (len)
		TEE_GenerateRandom(out, len);
}

#elif defined(__APPLE__)

#include <stdlib.h>

void pqchsm_randombytes(uint8_t *out, size_t len)
{
	if (len)
		arc4random_buf(out, len);
}

#else

#include <sys/random.h>

void pqchsm_randombytes(uint8_t *out, size_t len)
{
	while (len) {
		ssize_t n = getrandom(out, len, 0);
		if (n > 0) {
			out += n;
			len -= (size_t)n;
		}
	}
}

#endif
