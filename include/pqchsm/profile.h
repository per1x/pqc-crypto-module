/* pqchsm/profile.h —— 构建形态：DEV 还是 PRODUCTION
 *
 * ============================================================================
 * 【这个开关存在的理由】
 * ============================================================================
 * 本项目的信任根（KDR）有两个实现：
 *
 *   stub        —— src/crypto/kdr.c 里一段**编译进二进制的常量**，
 *                  字面量本身就写着 "PQC-HSM STUB KDR -- NOT SECRET!!"；
 *   device-dna  —— src/crypto/kdr_dna.c，根 = KDF(设备 DNA)。
 *                  它 device_bound=1（防克隆成立）但 **hardware_backed=0**
 *                  —— DNA 不是秘密，有 JTAG 的人直接读得到。
 *
 * 也就是说：**这块板上没有任何"有硬件保证的秘密根"**。eFUSE 被本项目永久
 * 排除（不可逆、只有一块板），BBRAM 加密启动是物理死结（VCC_BATT 无电池），
 * PUF 黑钥那条路也没走通。这些结论都写在 docs/SECURITY.md 里。
 *
 * 老代码的默认行为是"没装 provider 就悄悄用 stub"。那意味着**默认形态下
 * 密钥库的包裹密钥是一个公开常量** —— 拿到 SD 卡的人可以离线解开整个密钥库，
 * 而代码里没有任何一处会因此报错。这不是"原型阶段可以接受"，
 * 而是"没有人会发现"。
 *
 * 所以把它显式化成两个形态：
 *
 *   PQC_PROFILE_DEV （默认）
 *       允许 stub。演示、开发、跑 KAT 都走这个。stub 仍然会在启动时被
 *       pqc_profile_startup_check() 记一条明确的告警，不会假装自己是硬件根。
 *
 *   PQC_PROFILE_PRODUCTION
 *       ① **编译期**：stub 的那段常量根本不进二进制
 *          （kdr.c 里那一整段被 #if 掉，tools/check_profile.sh 扫符号验证）；
 *       ② **启动期**：pqc_profile_startup_check() 要求当前 provider
 *          hardware_backed != 0，否则**拒绝启动**。
 *
 * 于是"这块板做不到生产形态"这件事变成一条会当场失败的断言，
 * 而不是一句藏在文档里的话。
 *
 * 选形态：cmake -DPQC_PROFILE=PRODUCTION（默认 DEV）。
 */
#ifndef PQCHSM_PROFILE_H
#define PQCHSM_PROFILE_H

#ifdef __cplusplus
extern "C" {
#endif

#define PQC_PROFILE_DEV        0
#define PQC_PROFILE_PRODUCTION 1

#ifndef PQC_PROFILE
#define PQC_PROFILE PQC_PROFILE_DEV
#endif

/* "DEV" / "PRODUCTION" */
const char *pqc_profile_name(void);
int         pqc_profile_is_production(void);

/* 启动闸门。返回 0 放行；非 0 表示**必须拒绝启动**。
 *
 * *why 回填一句人话（永远非 NULL，即使返回 0 —— 那时是一条告警或 "ok"）。
 * 调用方应当把它原样打印出来：DEV 形态下那句告警是唯一提醒使用者
 * "现在的信任根是一个公开常量"的东西。 */
int pqc_profile_startup_check(const char **why);

#ifdef __cplusplus
}
#endif
#endif /* PQCHSM_PROFILE_H */
