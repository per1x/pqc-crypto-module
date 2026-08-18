/* profile.c —— 构建形态的运行时那一半（编译期那一半在 kdr.c 的 #if 里） */
#include "pqchsm/profile.h"
#include "pqchsm/kdr.h"

const char *pqc_profile_name(void)
{
#if PQC_PROFILE == PQC_PROFILE_PRODUCTION
	return "PRODUCTION";
#else
	return "DEV";
#endif
}

int pqc_profile_is_production(void)
{
	return PQC_PROFILE == PQC_PROFILE_PRODUCTION;
}

int pqc_profile_startup_check(const char **why)
{
	const pqc_kdr_provider_t *p = pqc_kdr_get_provider();
	static const char *msg;

	if (!p) {
		msg = "没有可用的 KDR provider（生产形态下 stub 未编译进来，"
		      "而硬件 provider 没有安装成功）—— 拒绝启动";
		if (why) {
			*why = msg;
		}
		return -1;
	}

#if PQC_PROFILE == PQC_PROFILE_PRODUCTION
	/* 生产形态只认"有硬件保证的秘密根"。
	 *
	 * ⚠️ device-dna provider **不满足**这一条：它 device_bound=1、
	 *    hardware_backed=0。DNA 逐片不同（防克隆成立），但它不是秘密 ——
	 *    有 JTAG 的人直接读得到。把防克隆当成机密性是一种夸大，
	 *    这里用代码把它钉死。 */
	if (!p->hardware_backed) {
		msg = "PRODUCTION 形态要求 KDR 有硬件保证（不出芯片、不可读出），"
		      "而当前 provider 不是 —— 拒绝启动。"
		      "这块板上没有可用的秘密硬件根（eFUSE 已永久排除、"
		      "BBRAM 无电池、PUF 黑钥未通），见 docs/SECURITY.md。";
		if (why) {
			*why = msg;
		}
		return -1;
	}
	msg = "PRODUCTION：KDR 由硬件保证";
	if (why) {
		*why = msg;
	}
	return 0;
#else
	if (!p->hardware_backed) {
		/* 放行，但要说清楚。这句话是 DEV 形态下唯一提醒使用者
		 * "信任根不受硬件保护"的东西，别把它降级成 debug 日志。 */
		msg = p->device_bound
		      ? "DEV：KDR 绑定到设备（防克隆成立）但**不是秘密**"
		        "（hardware_backed=0）—— 不要按生产形态解读"
		      : "DEV：⚠️ KDR 是编译进二进制的**公开常量**，"
		        "密钥库的机密性等于零。仅供开发/演示。";
	} else {
		msg = "DEV：KDR 由硬件保证";
	}
	if (why) {
		*why = msg;
	}
	return 0;
#endif
}
