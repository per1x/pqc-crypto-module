/* pqchsm/selftest.h —— 上电自测与错误状态
 *
 * FIPS 140-3（ISO/IEC 19790）与 GM/T 0028 都要求：模块在提供任何密码服务之前
 * 先对每类算法做已知答案测试；任一项失败即进入错误状态，此后拒绝一切密码运算，
 * 直到重新自测通过。这一层就是那个要求的实现。
 *
 * 【何时运行】
 * 第一次调用 pqc.h 里任何一个密码运算时自动跑一遍（惰性上电自测），也可以由
 * 调用方显式调用 pqc_self_test() 提前触发。自测本身不受错误状态阻断，
 * 因此从错误状态恢复的唯一途径就是再跑一次并通过。
 *
 * 【测什么】
 * 每类算法一条已知答案测试。期望值的来源写在 src/util/selftest.c 的注释里：
 * ML-KEM 与 ML-DSA 取自 NIST ACVP 向量，AES-256-GCM 取自 GCM 规范的公开测试用例，
 * SHA3-256 与 KMAC256 取自 FIPS 202 / SP 800-185 的公开示例。没有一条是从本模块
 * 自己的输出反推出来的 —— 那样的"自测"只能证明模块与自己一致。
 *
 * 【覆盖范围】
 * 自测针对模块内的算法实现（liboqs / OpenSSL 那条路径）。加速器 transport
 * 之间的一致性由 tests/unit/test_accel.c 与 test_accel_axi.c 逐字节比对保证，
 * 不在本层重复。
 */
#ifndef PQCHSM_SELFTEST_H
#define PQCHSM_SELFTEST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	PQC_ST_SHA3    = 0,   /* SHA3-256，FIPS 202 */
	PQC_ST_KMAC    = 1,   /* KMAC256，SP 800-185 */
	PQC_ST_AES_GCM = 2,   /* AES-256-GCM，FIPS 197 + SP 800-38D */
	PQC_ST_ML_KEM  = 3,   /* ML-KEM-768 密钥生成，FIPS 203 */
	PQC_ST_ML_DSA  = 4,   /* ML-DSA-65 密钥生成，FIPS 204 */
	PQC_ST__COUNT  = 5,
} pqc_selftest_id_t;

const char *pqc_selftest_name(pqc_selftest_id_t id);

/* 跑一遍完整自测。返回失败项的位图：0 表示全部通过。
 * 任一项失败即置模块为错误状态；全部通过则清除错误状态。 */
uint32_t pqc_self_test(void);

/* 模块当前是否可提供密码服务（自测已通过且未处于错误状态） */
int pqc_self_test_passed(void);

/* 上一次自测的失败位图 */
uint32_t pqc_self_test_failures(void);

/* 强制进入或退出错误状态。存在的意义是让测试能够验证"错误状态确实会阻断
 * 密码运算"—— 没有这条，错误状态是否真的生效就无从断言。 */
void pqc_self_test_force_error(int on);

#ifdef __cplusplus
}
#endif
#endif /* PQCHSM_SELFTEST_H */
