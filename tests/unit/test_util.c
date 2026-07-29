#include "testlib.h"
#include "pqchsm/util.h"

int main(void)
{
	uint8_t buf[8];
	char hex[32];

	TCASE("hex_decode 正常");
	CHECK_EQ_INT(pqc_hex_decode("00ff10", (size_t)-1, buf, sizeof(buf)), 3);
	CHECK_EQ_INT(buf[0], 0x00);
	CHECK_EQ_INT(buf[1], 0xff);
	CHECK_EQ_INT(buf[2], 0x10);

	TCASE("hex_decode 大写与显式长度");
	CHECK_EQ_INT(pqc_hex_decode("AABBCC", 6, buf, sizeof(buf)), 3);
	CHECK_EQ_INT(buf[0], 0xaa);
	CHECK_EQ_INT(buf[2], 0xcc);

	TCASE("hex_decode 非法输入必须失败");
	CHECK_EQ_INT(pqc_hex_decode("abc", (size_t)-1, buf, sizeof(buf)), -1);   /* 奇数长度 */
	CHECK_EQ_INT(pqc_hex_decode("zz", (size_t)-1, buf, sizeof(buf)), -1);    /* 非 hex */
	CHECK_EQ_INT(pqc_hex_decode("00112233445566778899", (size_t)-1, buf, sizeof(buf)), -1); /* 溢出 */
	CHECK_EQ_INT(pqc_hex_decode(NULL, 0, buf, sizeof(buf)), -1);

	TCASE("hex_encode 往返");
	uint8_t src[4] = { 0xde, 0xad, 0xbe, 0xef };
	CHECK_EQ_INT(pqc_hex_encode(src, 4, hex, sizeof(hex)), 0);
	CHECK(strcmp(hex, "deadbeef") == 0);
	CHECK_EQ_INT(pqc_hex_decode(hex, (size_t)-1, buf, sizeof(buf)), 4);
	CHECK_EQ_MEM(buf, src, 4);

	TCASE("hex_encode 缓冲不足必须失败");
	CHECK_EQ_INT(pqc_hex_encode(src, 4, hex, 8), -1);   /* 需要 9 字节 */

	TCASE("常量时间比较");
	uint8_t a[4] = { 1, 2, 3, 4 }, b[4] = { 1, 2, 3, 4 }, c[4] = { 1, 2, 3, 5 };
	CHECK_EQ_INT(pqc_ct_equal(a, b, 4), 1);
	CHECK_EQ_INT(pqc_ct_equal(a, c, 4), 0);
	CHECK_EQ_INT(pqc_ct_equal(a, c, 3), 1);     /* 只比前 3 字节 */
	CHECK_EQ_INT(pqc_ct_equal(NULL, b, 4), 0);

	/* ：中间值用后即清。这里验证清零真的落到了内存上
	 * —— 这类测试在 Python 里写不出来，也是选 C 的理由之一。 */
	TCASE("secure_zero 确实清零");
	uint8_t secret[64];
	memset(secret, 0xa5, sizeof(secret));
	pqc_secure_zero(secret, sizeof(secret));
	int all_zero = 1;
	for (size_t i = 0; i < sizeof(secret); i++) {
		if (secret[i] != 0) {
			all_zero = 0;
		}
	}
	CHECK(all_zero);
	pqc_secure_zero(NULL, 0);   /* 不应崩溃 */

	return test_report("test_util");
}
