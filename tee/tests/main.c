/* main.c —— native 测试入口 */
#include <stdio.h>

int test_fips202(void);
int test_kdf(void);
int test_wrap(void);
int test_pqc(void);

int main(void)
{
	int fails = 0;

	fails += test_fips202();
	fails += test_kdf();
	fails += test_wrap();
	fails += test_pqc();

	if (fails) {
		printf("\n%d FAILURES\n", fails);
		return 1;
	}
	printf("\nALL PASS\n");
	return 0;
}
