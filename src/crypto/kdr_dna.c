/* kdr_dna.c —— 把密钥派生根绑到这颗芯片的 Device DNA
 *
 * ============================================================================
 * 【它给什么，不给什么 —— 这段必须先读】
 * ============================================================================
 * 给的是**防克隆**：DNA 逐片不同，所以 KDR 逐片不同，keystore 拷到另一块板上
 * 打不开。
 *
 * **不给机密性。DNA 不是秘密。** 有 JTAG 的人直接 `mrd 0xFFCA0050` 就读到了
 * —— 本项目自己就是这么先读到它的。所以：
 *
 *   · provider 的 `hardware_backed` **是 0**，不是 1。
 *   · 谁把它写成"硬件密钥根"，谁就重复了 H5/H6 那类夸大。
 *
 * 顺带一提：OP-TEE 的 HUK 在**当前这种未供给形态下**也退化成 SHA-256(Device DNA)，
 * 性质完全一样。所以"不做 TA、直接读 DNA"并没有比走 OP-TEE 弱。
 *
 * ⚠️ 但别把这句话记成"这颗片子的属性"—— 它是**形态的属性**。上游
 *    core/drivers/zynqmp_huk.c 会看 CSU STATUS 的 AUTH 位：位为 0 才走
 *    development HUK（纯 SHA-256(DNA)，只打一行 IMSG）；供给到位后 HUK 是
 *    SHA-256(DNA + 选中的 USER eFUSE) 再经 CSU AES-GCM 用 device key 加密，
 *    那是真正的硬件保护。弱的不是这颗硅，是"本项目红线不烧 eFUSE、
 *    因而认证启动与秘密根都未启用"这个形态。
 *    失败形态很阴险：即便烧了 PUF/eFUSE AES，只要认证启动没同时成立，
 *    AUTH 位仍为 0、HUK 仍退化，而一切"看起来跑通了"。
 *    详见 docs/reference/HSM-COMPARISON.md §4.2。
 *
 * ============================================================================
 * 【DNA 从哪来：两条路，别只做一条】
 * ============================================================================
 *   ① **库跑在板上**：直接经 /dev/secmmio 读 0xFFCA0050-5C（EL3 白名单里的
 *      只读窗口）。见 pqc_kdr_install_device_dna()。
 *   ② **库跑在远端主机上**（真实拓扑就是这样：PKCS#11 在主机，密码机在板上）：
 *      主机上根本没有 /dev/secmmio。DNA 经线格操作 OP_DEVICE_DNA 从 daemon
 *      取回，再交给 pqc_kdr_install_device_dna_raw()。
 *
 * 只做 ① 的话，这个功能在真实拓扑里等于不存在 —— 代码在仓库里、永远跑不到。
 * 所以 raw 那条**不在 #ifdef __linux__ 里**，Mac 上的主机也能用。
 *
 * ============================================================================
 * 【拿不到 DNA 就失败，绝不回退到常量】
 * ============================================================================
 * 回退会得到一个**看起来绑定了、实际上人人相同**的根 —— 比没有更糟，因为它让
 * "设备绑定已启用"变成一句没人会发现的假话。
 *
 * ============================================================================
 * 【换 KDR 的代价：既有 keystore 会打不开】
 * ============================================================================
 * 包裹密钥由 KDR 派生。换根 = 换包裹密钥 = 旧库解不开，而且**症状与被篡改
 * 完全一致**（完整性错误）。所以这里只提供安装函数，由调用方显式决定；
 * p11 里靠 PQCHSM_KDR=device-dna 打开，默认不开。迁移要先导出再导入。
 */
#include "pqchsm/kdr.h"
#include "pqchsm/kdf.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint8_t g_root[PQC_KDR_LEN];
static int     g_ready;

static int dna_derive(const char *label, const uint8_t *salt, size_t salt_len,
                      uint8_t *out, size_t out_len)
{
	if (!g_ready) {
		return -1;
	}
	return pqc_kdf(g_root, PQC_KDR_LEN, salt, salt_len, label, out, out_len);
}

static const pqc_kdr_provider_t g_dna = {
	.name = "device-dna(bound to this chip, NOT secret)",
	.derive = dna_derive,
	/* DNA 可被 JTAG 读出 —— 没有任何硬件机密性可言。故意写 0。 */
	.hardware_backed = 0,
	.device_bound    = 1,
};

/* DNA 只有 128 位且**是公开的**，直接当 KDR 有两个问题：长度不够，以及
 * "根等于一个人人可读的值"。所以过一次 KDF 做域分隔与扩展。
 * 这**不会**让它变成秘密 —— 知道 DNA 的人照样能算出同一个根。它只是让根
 * 有正确的长度和用途分隔。这一句别删。 */
static int set_root_from_dna(const uint8_t *raw, size_t len)
{
	size_t  i;
	uint8_t acc = 0, all = 0xFFu;

	if (!raw || len == 0) {
		return -1;
	}
	/* 全 0 / 全 F 的健全性检查。这两种值是"没读到"最常见的形态：
	 * EL3 拒绝时返回 0，ioctl 号对不上或总线飘了常见全 F。
	 * 拿它们当根会得到一个所有板子相同的"设备绑定"。 */
	for (i = 0; i < len; i++) {
		acc |= raw[i];
		all &= raw[i];
	}
	if (acc == 0u || all == 0xFFu) {
		return -1;
	}
	if (pqc_kdf(raw, len, NULL, 0, "pqc-hsm/kdr-from-device-dna",
	            g_root, PQC_KDR_LEN) != 0) {
		return -1;
	}
	g_ready = 1;
	return 0;
}

int pqc_kdr_install_device_dna_raw(const uint8_t *dna, size_t len)
{
	if (set_root_from_dna(dna, len) != 0) {
		return -1;
	}
	pqc_kdr_set_provider(&g_dna);
	return 0;
}

#if defined(__linux__)

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* ⚠️ 这几行是 board/kmod/secmmio_uapi.h 的**第二份定义**，本来该直接 include
 *    那个头。不 include 的理由：它 include <linux/ioctl.h>，会把 src/ 这一层
 *    从"可移植 C"拖成"只能在 Linux 上编"。
 *
 *    代价是真实的：两处一旦对不上，症状是"读出垃圾"而不是编译错误。
 *    兜底就是上面 set_root_from_dna 里那道全 0 / 全 F 检查 —— ioctl 号错时
 *    最常见的结果正是这两种。 */
#define SECMMIO_MAGIC_ 'S'
#define SECMMIO_ARM_   _IO(SECMMIO_MAGIC_, 0)
#define SECMMIO_RD_    _IOWR(SECMMIO_MAGIC_, 1, struct secmmio_op_)
struct secmmio_op_ { uint32_t addr; uint32_t val; };

#define DNA_LO 0xFFCA0050u
#define DNA_HI 0xFFCA005Cu

static int read_dna_local(uint8_t out[16])
{
	int      fd, i = 0;
	uint32_t a;

	fd = open("/dev/secmmio", O_RDWR);
	if (fd < 0) {
		return -1;
	}
	if (ioctl(fd, SECMMIO_ARM_) < 0) {
		close(fd);
		return -1;
	}
	for (a = DNA_LO; a <= DNA_HI; a += 4, i += 4) {
		struct secmmio_op_ op;
		op.addr = a;
		op.val  = 0;
		if (ioctl(fd, SECMMIO_RD_, &op) < 0) {
			close(fd);
			return -1;
		}
		out[i + 0] = (uint8_t)(op.val >> 24);
		out[i + 1] = (uint8_t)(op.val >> 16);
		out[i + 2] = (uint8_t)(op.val >> 8);
		out[i + 3] = (uint8_t)(op.val);
	}
	close(fd);
	return 0;
}

const pqc_kdr_provider_t *pqc_kdr_provider_device_dna(void)
{
	uint8_t raw[16];

	if (g_ready) {
		return &g_dna;
	}
	if (read_dna_local(raw) != 0) {
		return NULL;
	}
	if (set_root_from_dna(raw, sizeof(raw)) != 0) {
		return NULL;
	}
	return &g_dna;
}

int pqc_kdr_install_device_dna(void)
{
	const pqc_kdr_provider_t *p = pqc_kdr_provider_device_dna();

	if (!p) {
		return -1;
	}
	pqc_kdr_set_provider(p);
	return 0;
}

#else  /* !__linux__ —— 主机侧只有 raw 那条路（DNA 从线格取回） */

const pqc_kdr_provider_t *pqc_kdr_provider_device_dna(void)
{
	return g_ready ? &g_dna : NULL;
}

int pqc_kdr_install_device_dna(void)
{
	return -1;
}

#endif
