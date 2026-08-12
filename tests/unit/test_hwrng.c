/* test_hwrng.c —— 硬件熵源驱动的契约测试
 *
 * 验的是 docs/trng-register-map.zh-CN.md 里那份行为契约的**软件侧**。
 * RTL 侧由 hardware/tb/cocotb/test_trng_axi.py 验同一份契约。两边对着同一张
 * 表各写各的，接口层面的分歧在无板阶段就会暴露 —— 这正是先定寄存器表、
 * 再写假外设那套做法的收益。
 *
 * 注意这里**不测熵**。transport 是软件模型，FIFO 由 OpenSSL 填，
 * 测出来的"随机性"只反映 OpenSSL，与 PL 里那颗 TRNG 毫无关系。
 * 真正的熵评估只能在硅上做，见 docs/fpga-进展.md 里那节警告。
 */
#include "testlib.h"

#include <string.h>

#include "hal/hwrng_stub.h"
#include "pqchsm/hwrng.h"
#include "pqchsm/util.h"

int main(void)
{
	uint8_t buf[64];
	uint8_t zero[64] = {0};

	TCASE("没装 transport 时如实报不可用");
	hwrng_set_transport(NULL);
	CHECK(!hwrng_available());
	CHECK(!hwrng_is_hardware());
	CHECK_EQ_INT(hwrng_bytes(buf, 16), HWRNG_ERR_ABSENT);
	CHECK_EQ_INT(hwrng_selftest(), HWRNG_ERR_ABSENT);

	TCASE("装上软件模型：自检通过，但不许自称硬件");
	hwrng_stub_reset();
	hwrng_set_transport(hwrng_transport_stub());
	CHECK(hwrng_available());
	/* 审计报告里"熵来自硬件"这句话的依据就是这一位，不能含糊 */
	CHECK(!hwrng_is_hardware());

	TCASE("启动健康检测没过之前 READY 不拉高");
	CHECK(!(hwrng_status() & HWRNG_ST_STARTUP_DONE));
	CHECK(!(hwrng_status() & HWRNG_ST_READY));
	CHECK_EQ_INT(hwrng_selftest(), HWRNG_OK);   /* 自检会等到暖机结束 */
	CHECK(hwrng_status() & HWRNG_ST_STARTUP_DONE);
	CHECK(hwrng_status() & HWRNG_ST_READY);

	TCASE("取字节");
	uint8_t a[64], b[64];
	CHECK_EQ_INT(hwrng_bytes(a, sizeof(a)), HWRNG_OK);
	CHECK_EQ_INT(hwrng_bytes(b, sizeof(b)), HWRNG_OK);
	CHECK(memcmp(a, b, sizeof(a)) != 0);
	/* 全零是最典型的"设备没在工作"的样子，单独兜一下 */
	CHECK(memcmp(a, zero, sizeof(a)) != 0);

	TCASE("非 4 的倍数：尾部字节要处理对");
	uint8_t odd[7];
	CHECK_EQ_INT(hwrng_bytes(odd, sizeof(odd)), HWRNG_OK);
	CHECK_EQ_INT(hwrng_bytes(odd, 1), HWRNG_OK);
	CHECK_EQ_INT(hwrng_bytes(NULL, 4), HWRNG_ERR_ARG);
	CHECK_EQ_INT(hwrng_bytes(odd, 0), HWRNG_ERR_ARG);

	/* 契约里最要紧的一条。告警是**锁存的电平**，只说明"这段时间里发生过"，
	 * 定位不到是哪个字出的问题，所以只能整批作废 —— 不是"把出错之后的
	 * 部分丢掉"。 */
	TCASE("告警：整批作废且缓冲区清零");
	memset(buf, 0xAA, sizeof(buf));
	hwrng_stub_force_alarm(1);
	CHECK_EQ_INT(hwrng_bytes(buf, sizeof(buf)), HWRNG_ERR_ALARM);
	CHECK_EQ_MEM(buf, zero, sizeof(buf));   /* 不能留半桶数据让调用方误用 */
	CHECK(hwrng_status() & HWRNG_ST_ALARM);
	CHECK(!(hwrng_status() & HWRNG_ST_READY));

	/* 上一条走的是"一开始就告警"的早退路径。这一条才是复查那条纪律：
	 * 告警发生在最后一个字之后，此时前面 8 个字已经拷进调用方的缓冲区了，
	 * 只有取完之后再读一次 STATUS 才能发现。 */
	TCASE("取到最后才告警：靠取完复查发现，整批照样作废");
	hwrng_stub_force_alarm(0);
	hwrng_clear_alarm();
	CHECK_EQ_INT(hwrng_selftest(), HWRNG_OK);
	memset(buf, 0xAA, sizeof(buf));
	hwrng_stub_alarm_after(8);            /* 32 字节正好 8 个字 */
	CHECK_EQ_INT(hwrng_bytes(buf, 32), HWRNG_ERR_ALARM);
	CHECK_EQ_MEM(buf, zero, 32);          /* 已经拷进去的那 8 个字也要抹掉 */

	TCASE("清告警之后启动检测重跑，不是清了就能接着用");
	hwrng_clear_alarm();
	CHECK(!(hwrng_status() & HWRNG_ST_STARTUP_DONE));
	CHECK_EQ_INT(hwrng_selftest(), HWRNG_OK);
	CHECK_EQ_INT(hwrng_bytes(buf, sizeof(buf)), HWRNG_OK);

	TCASE("池子空了要超时报错，不能死等也不能硬读 RDATA");
	hwrng_stub_starve(1);
	memset(buf, 0xAA, sizeof(buf));
	CHECK_EQ_INT(hwrng_bytes(buf, sizeof(buf)), HWRNG_ERR_TIMEOUT);
	CHECK_EQ_MEM(buf, zero, sizeof(buf));
	/* 驱动查了 DATA_VALID 才读，所以不该踩出 UNDERRUN */
	CHECK(!(hwrng_status() & HWRNG_ST_UNDERRUN));
	hwrng_stub_starve(0);
	CHECK_EQ_INT(hwrng_bytes(buf, sizeof(buf)), HWRNG_OK);

	TCASE("ZEROIZE：擦池、重跑启动检测");
	hwrng_zeroize();
	CHECK(!(hwrng_status() & HWRNG_ST_STARTUP_DONE));
	CHECK(!(hwrng_status() & HWRNG_ST_READY));
	CHECK_EQ_INT(hwrng_selftest(), HWRNG_OK);
	CHECK_EQ_INT(hwrng_bytes(buf, sizeof(buf)), HWRNG_OK);

	TCASE("pqc_random_bytes 真的走硬件熵源");
	const hwrng_transport_t *tr = hwrng_get_transport();
	uint32_t before = tr->read_reg(HWRNG_REG_WORDS);
	CHECK_EQ_INT(pqc_random_bytes(buf, 32), 0);
	uint32_t after = tr->read_reg(HWRNG_REG_WORDS);
	CHECK_EQ_INT(after - before, 8);   /* 32 字节正好弹 8 个字 */

	TCASE("熵源故障时绝不回退到软件源");
	hwrng_stub_force_alarm(1);
	CHECK_EQ_INT(pqc_random_bytes(buf, 32), -1);

	TCASE("卸掉 transport 后回到软件源");
	hwrng_set_transport(NULL);
	CHECK_EQ_INT(pqc_random_bytes(buf, 32), 0);

	/* 这是进程级全局状态，别留给后面 */
	hwrng_set_transport(NULL);
	return test_report("hwrng");
}
