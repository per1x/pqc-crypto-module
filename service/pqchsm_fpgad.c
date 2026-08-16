// pqchsm_fpgad —— 密码机服务 daemon：把标准接口的请求落到 FPGA 密码核上
//
//   pqchsm_fpgad [-f] [-lock]
//     -f      前台运行（板上调试用）
//     -lock   启动时置上私钥外泄闩锁：ML-KEM 的 dk 在**硬件里**再也送不出总线。
//             交付/演示形态用它；跑 ACVP 的 KeyGen 向量时不能用（那要核对 dk），
//             而闩锁只有重新装载位流才解得开。
//
// ============================================================================
// 【它在栈里的位置，以及为什么这一层不能省】
// ============================================================================
//   应用 → libsdfe（无状态）→ **本 daemon** → /dev/secmmio → EL3 SiP → FPGA
//
// 三个核都是"写 MODE → 灌输入 → START → 轮询 STATUS → 读输出"的**有状态序列**。
// 两个进程交错驱动同一个核，得到的是互相错位、但看起来完全合法的结果 ——
// 不报错、不崩溃，就是算错。所以硬件访问必须有一个串行化的单点，
// 而那个单点顺带就是放会话与句柄的地方。
//
// ============================================================================
// 【私钥：两种，处理方式不同，别混为一谈】
// ============================================================================
//   · **对称密钥**进 PL 的 key_vault。RTL 上没有通往总线的读路径 ——
//     装进去之后连本 daemon 也读不回来，只能按槽号使唤。
//
//   · **ML-KEM 的 dk 与 ML-DSA 的 sk** 现在也进 PL 的片内金库（各自一套槽），
//     KeyGen 只交出公钥与槽号。**本进程的内存里没有任何私钥** ——
//     句柄表里只有"哪个槽、什么参数集"两个字段。
//
//     ⚠️ 这一段以前写的是"dk 留在本进程内存里"，那是 dk_to_slot 之前的旧稿。
//     注释与代码倒挂在安全叙事上是**最贵**的一种：读注释的人会以为私钥出了
//     硬件而其实没有，或者反过来。改代码时这段必须跟着改。
//
//   · **句柄是会话内的。** 连接一断，两张句柄表清零，并且给两个 PQC 金库各发
//     一次 ZEROIZE —— 私钥不跨会话存活，既不在进程里、也不在硬件里。
//     少了这一条，A 连接生成的句柄 0，B 连接重连之后照样能拿来解封装/签名。
//
// ============================================================================
// 【为什么不直接复用 cli/pqchsmd.c】
// ============================================================================
// 那个 daemon 连着 keystore、PIN、槽位 FSM、KEK 包裹一整套，后端是 liboqs
// 软件实现。把它整个搬到板上要一并带起那套状态，与"证明硬件能被标准接口调用"
// 这件事无关。这里做一个小的、只管硬件的 daemon，协议形状与它一致
// （定长头 + 长度前缀），将来合并时不用改调用方。
// 复用清单见 docs/API.md §8。
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "wire.h"
#include "../board/kmod/secmmio_uapi.h"

/* ⚠️ 这几个码**走在线上**（pqcs_resp.status），所以它们其实是协议的一部分，
 *    却在这里和 sdfe.h 里各存了一份 —— 正是 wire.h 文件头警告的那种双份定义。
 *    对不上的症状是"客户端把成功当失败"，不是编译错误。
 *    改动任何一个都必须两边一起改；新增的 SDR_AUTHFAIL 也是。
 *    （彻底的做法是把它们挪进 wire.h，但那会让公开头文件 sdfe.h 依赖内部
 *      线格式头，是另一次接口取舍，不在这批里做。） */
#define SDR_OK          0x00000000u
#define SDR_UNKNOWERR   0x01000001u
#define SDR_COMMFAIL    0x01000003u
#define SDR_INARGERR    0x01000004u
#define SDR_KEYNOTEXIST 0x01000005u
#define SDR_AUTHFAIL    0x01000007u
#define SDR_HARDFAIL    0x01000006u
#define SDR_VERIFYFAIL  0x01000008u

#define PL_BASE   0x80000000UL
#define S_TRNG    0x00000
#define S_VAULT   0x10000
#define S_SYM     0x20000
#define S_MLKEM   0x30000
#define S_MLDSA   0x60000   /* 0x8006_0000 —— mldsa_axi（已落地，槽 6） */

/* ---- 寄存器（与各核的 localparam A_* 一致）---- */
#define TR_CTRL   (S_TRNG + 0x00)
#define TR_STATUS (S_TRNG + 0x04)
#define TR_RDATA  (S_TRNG + 0x08)
#define TR_VERSION (S_TRNG + 0x20)   /* 非零常量，用作"这条路通不通"的判据 */

/* 所有核的 VERSION 都是这个值（见各 *_axi.v 的 VERSION 参数）。
 * 它非零，这一点正是 RAZ/WI 之后"被拒 = 读回 0"这个判据成立的前提。 */
#define CORE_VERSION 0x00010000u
#define TRS_DVALID (1u << 1)
#define TRS_STARTUP (1u << 5)

#define V_SLOT    (S_VAULT + 0x0C)
#define V_KEYIN   (S_VAULT + 0x10)
#define V_SLOTC   (S_VAULT + 0x14)

#define SY_VER    (S_SYM + 0x00)
#define SY_STATUS (S_SYM + 0x08)
#define SY_ALG    (S_SYM + 0x0C)
#define SY_SLOT   (S_SYM + 0x10)
#define SY_CMD    (S_SYM + 0x14)
#define SY_DIN0   (S_SYM + 0x20)
#define SY_DOUT0  (S_SYM + 0x30)
/* sym_axi 的 status_word（sym_axi.v:277）：{kv_valid[3], key_ready[2],
 * op_done[1], busy[0]}。⚠️ 别按"从 0 开始数"想当然 —— bit0 是 busy 不是 done。
 * 第一版三个位全错了一位，症状是 SDFE_Encrypt 报"硬件运算失败"。 */
#define SYS_BUSY  (1u << 0)
#define SYS_DONE  (1u << 1)
#define SYS_KRDY  (1u << 2)
#define SYS_KVOK  (1u << 3)

#define MK_VER    (S_MLKEM + 0x00)
#define MK_CTRL   (S_MLKEM + 0x04)
#define MK_STATUS (S_MLKEM + 0x08)
#define MK_MODE   (S_MLKEM + 0x0C)
#define MK_INDATA (S_MLKEM + 0x10)
#define MK_INPTR  (S_MLKEM + 0x14)
#define MK_OUTDAT (S_MLKEM + 0x18)
#define MK_OUTLEN (S_MLKEM + 0x1C)
#define MKC_START 1u
#define MKC_INRST 4u
#define MKC_DKLOCK 0x10u                 /* CTRL[4]：一次性闩锁 */
#define MKC_ZEROIZE 2u                   /* CTRL[1]：擦金库（见 mlkem_axi.v A_CTRL） */
#define MK_KEYSTAT (S_MLKEM + 0x30)
#define MK_KEYPSET (S_MLKEM + 0x34)
/* MODE 里控制片内私钥金库的三个字段 */
#define MKM_DK_TO_SLOT   0x10u           /* KeyGen：dk 进金库，不出总线 */
#define MKM_DK_FROM_SLOT 0x20u           /* Decaps：dk 从金库取 */
#define MKM_SLOT(s)      (((s) & 15u) << 6)   /* MODE[9:6]，16 个槽 */
#define PL_KEY_SLOTS     16              /* 金库有 16 个槽（64 KB / 4096） */
#define MKS_DONE  (1u << 1)
#define MKS_PARER (1u << 5)
#define MKS_WIPING (1u << 4)   /* r_status 位序见 mlkem_axi.v:411 */

/* ---- ML-DSA（mldsa_axi @ 0x8006_0000）--------------------------------------
 *
 * 从机在槽 6，2026-08-17 起两种位流形态下都端到端跑通（board/logs/）。
 *
 * 与 mlkem_axi 的三处不同，都会咬人，写在这里免得照着 MK_* 抄错：
 *   ① 复位输入指针的办法不一样：ML-KEM 是 CTRL[2]（MKC_INRST），
 *      ML-DSA 是**写 IN_PTR = 0**，CTRL[1] 是另一件事（CLEAR，清整个状态）；
 *   ② 多了 OUT_PTR / MSG_LEN / CTX_LEN 三个 RW 寄存器；
 *   ③ STATUS 的位序完全不同 —— ML-KEM 的 param_err 在 bit5，
 *      ML-DSA 在 bit3，而 bit2 是 verify_ok。按 MK 的位掩码去读会读到别的位。
 */
#define MD_VER     (S_MLDSA + 0x00)
#define MD_CTRL    (S_MLDSA + 0x04)
/* ⚠️ MODE 是 0x08、STATUS 是 0x0C —— 这两个**曾经写反过**，而且症状极具
 *    误导性：VERSION(0x00)、IN_DATA(0x10)、IN_PTR(0x14) 都在错位区之外，
 *    所以"读得到版本号、灌数据后 IN_PTR 也对得上"全都成立，看起来这条路
 *    是通的。实际是 MODE 被写进了只读的 STATUS（写掉进空处，核从没收到过
 *    op/pset），而轮询读的"STATUS"其实是 MODE，永远不会出现 DONE。
 *    表现是"每次都等 done 超时"，与"核算不出来"分不开。
 *    改这里要对着 mldsa_axi.v 的寄存器映射逐行核，别照记忆写。 */
#define MD_MODE    (S_MLDSA + 0x08)
#define MD_STATUS  (S_MLDSA + 0x0C)
#define MD_INDATA  (S_MLDSA + 0x10)
#define MD_INPTR   (S_MLDSA + 0x14)
#define MD_OUTDAT  (S_MLDSA + 0x18)
#define MD_OUTPTR  (S_MLDSA + 0x1C)
#define MD_OUTLEN  (S_MLDSA + 0x20)
#define MD_MSGLEN  (S_MLDSA + 0x24)
#define MD_CTXLEN  (S_MLDSA + 0x28)
#define MD_KEYSTAT (S_MLDSA + 0x2C)   /* {sk_lock, slot_valid[7:0]} */

#define MDC_START  0x01u              /* CTRL[0] */
#define MDC_CLEAR  0x02u              /* CTRL[1] */
#define MDC_SKLOCK 0x10u              /* CTRL[4]：一次性闩锁 */
#define MDC_ZEROIZE 0x04u             /* CTRL[2]：擦金库（见 mldsa_axi.v A_CTRL） */

#define MDS_BUSY   (1u << 0)
#define MDS_DONE   (1u << 1)
#define MDS_VEROK  (1u << 2)
#define MDS_PARER  (1u << 3)
#define MDS_LENER  (1u << 4)
#define MDS_WIPING (1u << 6)   /* r_status 位序见 mldsa_axi.v:462 */

/* MODE：[1:0]=OP [3:2]=PSET [4]=SK_TO_SLOT [5]=SK_FROM_SLOT [9:6]=SLOT */
#define MDO_KEYGEN 0u
#define MDO_SIGN   1u
#define MDO_VERIFY 2u
#define MDM_SK_TO_SLOT   0x10u
#define MDM_SK_FROM_SLOT 0x20u
#define MDM_SLOT(s)      (((s) & 15u) << 6)
#define MLDSA_KEY_SLOTS  8            /* 签名私钥金库 8 个槽（比 ML-KEM 的 16 少） */

static int sec_fd = -1;
static int fg;

static void logf_(const char *fmt, ...)
{
	char ts[32];
	time_t t = time(NULL);
	struct tm tm;
	va_list ap;

	localtime_r(&t, &tm);
	strftime(ts, sizeof ts, "%H:%M:%S", &tm);
	fprintf(stderr, "[%s] ", ts);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

/* ---- 远程口令 ---- */
static uint8_t token[PQCS_TOKEN_MAX + 1];
static size_t  token_len;
static int     tcp_srv = -1;

/* 定长比较，不因第一个不同的字节在哪儿而提前返回。
 *
 * 口令是短的、可以被反复试的，而"第几个字节开始不对"这种时间差在局域网上
 * 是测得出来的 —— 一次朴素的 memcmp 就把 128 字节的搜索空间降成 128×256 次
 * 尝试。这一行的代价是零，不写才是需要解释的那个选择。 */
static int ct_eq(const uint8_t *a, const uint8_t *b, size_t n)
{
	uint8_t d = 0;
	size_t i;

	for (i = 0; i < n; i++)
		d |= (uint8_t)(a[i] ^ b[i]);
	return d == 0;
}

/* ---- 硬件访问：每一笔都经 EL3 ---- */
static int hw_rd(unsigned off, uint32_t *v)
{
	struct secmmio_op op = { .addr = (uint32_t)(PL_BASE + off), .val = 0 };

	if (ioctl(sec_fd, SECMMIO_RD, &op) < 0)
		return -1;
	*v = op.val;
	return 0;
}

static int hw_wr(unsigned off, uint32_t v)
{
	struct secmmio_op op = { .addr = (uint32_t)(PL_BASE + off), .val = v };

	return ioctl(sec_fd, SECMMIO_WR, &op) < 0 ? -1 : 0;
}

/* 本次请求里硬件访问是否已经失败过。
 *
 * ============================================================================
 * 【为什么需要这个标志：一次真实的失控】
 * ============================================================================
 * 原来的 rd() 把 ioctl 失败吞掉、返回 0，而下面每个等待都是
 * `for (spin = 0; spin < 20000000L; spin++) if (rd(...) & BIT) break;`。
 *
 * 于是硬件一旦不可达（实测：BOOT.BIN 里的 BL31 没带那个 SiP，每一笔 SMC
 * 都被拒），每个等待都要空转两千万次、**每一次都写一行日志** —— 一个
 * 本该立刻返回"硬件失败"的请求，变成了刷屏 + 跑满一个核。
 *
 * 这不是"错误处理不够漂亮"，是**演示形态的可用性问题**：板子看着活着、
 * SSH 进得去，但 load 常驻 1.0、日志把 SD 卡写满，而真正的原因藏在
 * 两千万行重复里。
 *
 * 所以：**失败要粘住，并且立刻停下。**
 *   · 任何一笔读写失败都置位；
 *   · 所有自旋等待都以它为退出条件 —— 硬件没了就别再等了；
 *   · 每个请求开头清零、结尾检查，把 SDR_HARDFAIL 如实报给调用方；
 *   · 日志每个请求最多一行。
 */
static int hw_fault;

static uint32_t rd(unsigned off)
{
	uint32_t v = 0;

	if (hw_rd(off, &v)) {
		if (!hw_fault)          /* 每个请求只记一行，不刷屏 */
			logf_("读 0x%08lx 失败（EL3 拒绝或 SiP 不存在），"
			      "本次请求判为硬件失败", PL_BASE + off);
		hw_fault = 1;
	}
	return v;
}
static void wr(unsigned off, uint32_t v)
{
	if (hw_wr(off, v)) {
		if (!hw_fault)
			logf_("写 0x%08lx 失败（EL3 拒绝或 SiP 不存在），"
			      "本次请求判为硬件失败", PL_BASE + off);
		hw_fault = 1;
	}
}

/* ---- TRNG ---- */
static int trng_bytes(uint8_t *out, uint32_t n)
{
	uint32_t got = 0, w;
	long spin;

	wr(TR_CTRL, 1);
	for (spin = 0; spin < 20000000L && !hw_fault; spin++)
		if (rd(TR_STATUS) & TRS_STARTUP)
			break;
	if (!(rd(TR_STATUS) & TRS_STARTUP))
		return -1;
	while (got < n) {
		for (spin = 0; spin < 20000000L && !hw_fault; spin++)
			if (rd(TR_STATUS) & TRS_DVALID)
				break;
		if (!(rd(TR_STATUS) & TRS_DVALID))
			return -1;
		w = rd(TR_RDATA);
		for (int i = 0; i < 4 && got < n; i++)
			out[got++] = (uint8_t)(w >> (8 * i));
	}
	return 0;
}

/* ---- ML-KEM 长度（由 param_set 算出，与 RTL 一致）---- */
static void mlkem_len(uint32_t pset, uint32_t *ek, uint32_t *dk, uint32_t *ct)
{
	uint32_t k = pset == 0 ? 2 : pset == 1 ? 3 : 4;
	uint32_t du = pset == 2 ? 11 : 10, dv = pset == 2 ? 5 : 4;

	*ek = 384 * k + 32;
	*dk = 768 * k + 96;
	*ct = 32 * (du * k + dv);
}

/* 通用：灌输入 → 启动 → 等完成 → 取输出 */
static int mlkem_run(uint32_t mode, uint32_t pset, uint32_t mode_extra,
		     const uint8_t *in, uint32_t in_len,
		     uint8_t *out, uint32_t out_cap, uint32_t *out_len)
{
	uint32_t i, n, st;
	long spin;

	wr(MK_MODE, mode | (pset << 2) | mode_extra);
	wr(MK_CTRL, MKC_INRST);
	for (i = 0; i < in_len; i++)
		wr(MK_INDATA, in[i]);
	if (rd(MK_INPTR) != in_len) {
		logf_("IN_PTR 对不上：%u vs %u", rd(MK_INPTR), in_len);
		return -1;
	}
	wr(MK_CTRL, MKC_START);
	for (spin = 0; spin < 3000000L; spin++) {
		st = rd(MK_STATUS);
		if (st & MKS_PARER) {
			logf_("硬件拒绝了非法参数（mode=%u pset=%u）", mode, pset);
			return -2;
		}
		if (st & MKS_DONE)
			break;
	}
	if (!(rd(MK_STATUS) & MKS_DONE))
		return -3;
	n = rd(MK_OUTLEN);
	if (n > out_cap)
		return -4;
	for (i = 0; i < n; i++)
		out[i] = (uint8_t)rd(MK_OUTDAT);
	*out_len = n;
	return 0;
}

/* ---- ML-DSA 长度（FIPS 204，与 RTL 一致）---- */
static void mldsa_len(uint32_t pset, uint32_t *pk, uint32_t *sk, uint32_t *sig)
{
	static const uint32_t PK[3]  = { 1312, 1952, 2592 };
	static const uint32_t SK[3]  = { 2560, 4032, 4896 };
	static const uint32_t SIG[3] = { 2420, 3309, 4627 };

	*pk  = PK[pset];
	*sk  = SK[pset];
	*sig = SIG[pset];
}

/* 等 done 等多久。
 *
 * ============================================================================
 * 【为什么这里不能照抄 mlkem_run 的 3000000】
 * ============================================================================
 * ML-KEM 的每条运算都是定长无分支的：耗时只取决于参数集，抖动很小，
 * 一个写死的自旋数够用。ML-DSA 的 Sign 不是 —— 它有**拒绝采样循环**：
 * 每轮重采 y、算 w、试 z/r₀/ct₀ 的范数，任一项越界就整轮重来。
 * 期望轮数是个位数，但分布有长尾，偶尔十几轮是**正常的、不是故障**。
 *
 * 拿 ML-KEM 的量级去卡它，症状是"平时好好的，偶尔报一次硬件失败"，
 * 参数集越大越容易踩到。这种偶发会被当成硬件不稳定去查 —— 方向从一开始就错了。
 *
 * 另一头也不能无限等：daemon 是单线程的，一条卡住的请求把后面所有人一起挡住
 * （与文件里 TCP 超时那段是同一个可用性论证）。所以给**墙钟期限**而不是自旋数：
 * 自旋数换算成时间要依赖"一笔 ioctl+SMC 多快"，那个数随平台变，写死了等于
 * 把超时长度交给运气。
 *
 * 下面这些系数是**上限**，不是耗时估计：它们唯一的作用是保证硬件不吭声时
 * 我们会停下来。真实耗时等从机落地后实测，届时按量出来的数收紧。
 */
static long mldsa_timeout_ms(uint32_t op, uint32_t pset, uint32_t in_len)
{
	long base = (op == MDO_SIGN) ? 10000L : 2000L;
	long mul  = (long)pset + 1;            /* 44/65/87 → 1/2/3 */

	/* μ = H(tr‖M') 要把整条消息吸进 SHAKE：长消息是实打实的时间。 */
	return base * mul + (long)in_len / 8;
}

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* 通用：写 MODE/长度 → 灌输入 → 脉冲 START → 等 done → 取输出。
 * 形状照 mlkem_run，差别都在寄存器约定上（见 MD_* 那段的三条）。
 *
 * 返回：0 成功；-1 IN_PTR 对不上；-2 硬件拒绝参数/长度；-3 超时；-4 输出放不下。
 * verify_ok 只在 op == MDO_VERIFY 时有意义。 */
static int mldsa_run(uint32_t op, uint32_t pset, uint32_t mode_extra,
		     const uint8_t *in, uint32_t in_len,
		     uint32_t msg_len, uint32_t ctx_len,
		     uint8_t *out, uint32_t out_cap, uint32_t *out_len,
		     int *verify_ok)
{
	uint32_t i, n, st = 0;
	long deadline;

	if (verify_ok)
		*verify_ok = 0;
	wr(MD_MODE, op | (pset << 2) | mode_extra);
	wr(MD_CTRL, MDC_CLEAR);
	wr(MD_INPTR, 0);            /* ⚠️ 不是 CTRL 的某个位 —— 见 MD_* 那段第 ① 条 */
	wr(MD_OUTPTR, 0);
	wr(MD_MSGLEN, msg_len);
	wr(MD_CTXLEN, ctx_len);
	for (i = 0; i < in_len && !hw_fault; i++)
		wr(MD_INDATA, in[i]);
	if (hw_fault)
		return -3;
	if (rd(MD_INPTR) != in_len) {
		logf_("IN_PTR 对不上：%u vs %u", rd(MD_INPTR), in_len);
		return -1;
	}
	wr(MD_CTRL, MDC_START);

	/* 每 4096 次才看一次表：取时间本身也是个系统调用，
	 * 每轮都取会把等待循环变成"主要在读时钟"。 */
	deadline = now_ms() + mldsa_timeout_ms(op, pset, in_len);
	for (i = 0; !hw_fault; i++) {
		st = rd(MD_STATUS);
		if (st & (MDS_PARER | MDS_LENER)) {
			logf_("硬件拒绝了这次请求（op=%u pset=%u STATUS=0x%08x，"
			      "param_err=%d len_err=%d）", op, pset, st,
			      !!(st & MDS_PARER), !!(st & MDS_LENER));
			return -2;
		}
		if (st & MDS_DONE)
			break;
		if ((i & 0xFFF) == 0xFFF && now_ms() > deadline) {
			logf_("等 ML-DSA done 超时（op=%u pset=%u，上限 %ld ms）",
			      op, pset, mldsa_timeout_ms(op, pset, in_len));
			return -3;
		}
	}
	if (hw_fault || !(st & MDS_DONE))
		return -3;
	if (verify_ok)
		*verify_ok = (st & MDS_VEROK) ? 1 : 0;
	n = rd(MD_OUTLEN);
	if (n > out_cap)
		return -4;
	for (i = 0; i < n && !hw_fault; i++)
		out[i] = (uint8_t)rd(MD_OUTDAT);
	if (hw_fault)
		return -3;
	*out_len = n;
	return 0;
}

/* ---- 句柄表：dk 留在这里，不给应用 ---- */
/* ============================================================================
 * 【句柄表：现在它里面没有私钥了】
 * ============================================================================
 * 以前这里存着 dk 本身（3200 字节一把）—— 因为 KeyGen 会把 ek‖dk 一起从
 * OUT_DATA 交出来，dk 必须落到某个地方，而"某个地方"就是本进程的堆。
 * 于是"私钥不出**接口**"成立（应用只拿到句柄），"私钥不出**硬件**"不成立。
 *
 * 现在 dk 整个留在 PL 的片内金库里，本表只记"哪个槽、什么参数集"。
 * 句柄数因此从 16 降到 4（金库槽数），这是实打实的容量代价，
 * 换来的是**这个进程的内存里再也没有私钥**。
 */
#define MAX_KEYS PL_KEY_SLOTS
static struct { int used; uint32_t pset; } keys[MAX_KEYS];

/* 签名私钥的句柄表。**与上面那张分开**，不是重复代码：
 * 两个核各有自己的金库，槽号空间互不相干（ML-KEM 16 个、ML-DSA 8 个）。
 * 合成一张表就得再引一个"这是哪个核"的字段，而那个字段一旦弄错，
 * 后果是拿签名槽的号去 ML-KEM 的金库里取私钥 —— 恰恰是句柄不做间接映射
 * 要避免的那类错。两张表、各自槽号即句柄，对不上的可能性为零。 */
static struct { int used; uint32_t pset; } dsa_keys[MLDSA_KEY_SLOTS];

static void keys_wipe(void)
{
	memset(keys, 0, sizeof keys);
	memset(dsa_keys, 0, sizeof dsa_keys);
}

/* ============================================================================
 * 【会话结束：句柄失效，硬件里的私钥也擦掉】
 * ============================================================================
 * daemon **一次只服务一个连接**（硬件序列必须串行化，见文件头），所以"会话"
 * 与"连接"是一回事，隔离用不着 owner 字段：连接一断把两张表清零就够了。
 *
 * ⚠️ 但只清表是不够的 —— 表清了，**私钥还在 PL 的金库槽里**。下一个连接
 * 拿不到句柄（`used == 0` 就被拒），可它只要自己做一次 KeyGen 就会拿到同一个
 * 槽号，而"上一个人的私钥还在那儿"这件事本身就不该成立。所以这里同时给两个
 * PQC 金库各发一次 ZEROIZE。
 *
 * ⚠️ **一次性闩锁不受影响**：mldsa_axi / mlkem_axi 的 SK_LOCK 都明确写着
 * "ZEROIZE 不清它" —— 擦秘密不等于撤防线。交付形态开机置上的那道闩锁，
 * 会一直在。
 *
 * 对称密钥的 key_vault **不在这里擦**：它是应用显式 ImportKey 装进去的，
 * 生命周期由调用方决定（PKCS#11 那侧有对象销毁语义），而 PQC 私钥是本
 * daemon 自己生成的、只在会话内有意义。两者的归属不同，处理也就不同。
 */
/* ============================================================================
 * 【⚠️ 擦除没落地就写下一笔 = 丢板子。等它落下来，而且只能用读来等】
 * ============================================================================
 * 两个从机在 WIPING 期间对**写**一律回 SLVERR（那是对的：静默丢弃更危险）。
 * 但这条路上的写是从 **EL3** 发出的，而 BL31 里没有任何东西接得住总线错误 ——
 * SLVERR 以 SError 回来，发 SMC 的那个核**当场卡死**，板子只能断电。
 *
 * 实测栽过一次（2026-08-17）：session_end 发完 ZEROIZE 立刻返回，客户端断开
 * 之后**马上重连**，新连接的第一笔写正落在还在擦的 ML-KEM 上 —— 板子硬挂，
 * ping 不通，只能断电。
 *
 * 所以这里必须等到 WIPING 落下来再返回。**等的方式只能是读**：读被拒是同步
 * 外部中止，接得住；写被拒是 posted 的，接不住。这条不是风格，是这块板上
 * "能不能远程恢复"的分界线。
 *
 * 擦一遍 64 KB 金库约 8192 拍 @ ~150 MHz ≈ 55 µs，ML-DSA 那边 16 KB 更短；
 * 自旋上限给到远大于它的数，超时就如实记一笔并放弃（不再写任何东西）。
 */
static int wait_not_wiping(unsigned reg, uint32_t bit, const char *who)
{
	long spin;

	for (spin = 0; spin < 200000L; spin++) {
		uint32_t st = rd(reg);

		if (hw_fault)
			return -1;          /* 读都失败了，更不能去写 */
		if (!(st & bit))
			return 0;
	}
	logf_("%s 的 WIPING 一直不落 —— **不再写任何寄存器**（写被拒会 SError）", who);
	return -1;
}

static void session_end(void)
{
	keys_wipe();
	/* 发擦除之前也要确认它没在擦：上一次的擦除还没完就再写一次 CTRL，
	 * 同样是往 WIPING 里写。 */
	if (wait_not_wiping(MK_STATUS, MKS_WIPING, "ML-KEM") == 0)
		wr(MK_CTRL, MKC_ZEROIZE);
	if (wait_not_wiping(MD_STATUS, MDS_WIPING, "ML-DSA") == 0)
		wr(MD_CTRL, MDC_ZEROIZE);
	/* ⚠️ 关键的一步：**等擦完再返回**。下一个连接的第一笔写不能落在 WIPING 上。 */
	wait_not_wiping(MK_STATUS, MKS_WIPING, "ML-KEM");
	wait_not_wiping(MD_STATUS, MDS_WIPING, "ML-DSA");
}

/* ---- 对称 ---- */
static int sym_block(uint32_t alg, uint32_t slot, int dec,
		     const uint8_t *in, uint8_t *out)
{
	long spin;
	int i;

	wr(SY_ALG, alg | (dec ? 4u : 0u));
	wr(SY_SLOT, slot);
	if (!(rd(SY_STATUS) & SYS_KVOK))
		return -1;
	wr(SY_CMD, 1);
	for (spin = 0; spin < 200000L && !hw_fault; spin++)
		if (rd(SY_STATUS) & SYS_KRDY)
			break;
	if (!(rd(SY_STATUS) & SYS_KRDY))
		return -2;
	for (i = 0; i < 4; i++)
		wr(SY_DIN0 + 4 * i,
		   ((uint32_t)in[4*i] << 24) | ((uint32_t)in[4*i+1] << 16) |
		   ((uint32_t)in[4*i+2] << 8) | in[4*i+3]);
	wr(SY_CMD, 2);
	for (spin = 0; spin < 200000L && !hw_fault; spin++)
		if (rd(SY_STATUS) & SYS_DONE)
			break;
	if (!(rd(SY_STATUS) & SYS_DONE))
		return -3;
	for (i = 0; i < 4; i++) {
		uint32_t w = rd(SY_DOUT0 + 4 * i);

		out[4*i]   = (uint8_t)(w >> 24);
		out[4*i+1] = (uint8_t)(w >> 16);
		out[4*i+2] = (uint8_t)(w >> 8);
		out[4*i+3] = (uint8_t)w;
	}
	return 0;
}

/* ============================================================================
 * 【工作模式：链接在这里，分组变换在硬件里】
 * ============================================================================
 * sym_axi 只做单分组变换（16 字节进、16 字节出，密钥按槽号使唤）。CBC/CTR/
 * CFB/OFB 的链接、计数、异或都在本函数里，每个分组仍然各走一次硬件。
 *
 * 于是"密钥不出金库"这条性质在模式下**原样成立** —— 本进程从头到尾没有密钥
 * 字节，只有 IV 与数据。而"硬件做的是什么"必须说准：**是分组变换，不是模式**。
 *
 * ⚠️ CTR / OFB / CFB 三种只用**加密方向**的分组变换，解密也一样 ——
 *    把 dec 传给硬件会算出完全错误的密钥流。这是最容易抄错的一处：
 *    "解密就把 dec 置上"对 ECB/CBC 成立，对这三种是错的。
 *
 * 返回 0 成功；<0 硬件失败。
 */
static void ctr_inc(uint8_t ctr[16])
{
	int i;

	/* 整 128 位大端自增（SP 800-38A 的 standard incrementing function，
	 * 全宽版本）。回绕在 2^128 个分组之后，够不着。 */
	for (i = 15; i >= 0; i--)
		if (++ctr[i])
			break;
}

static int sym_crypt(uint32_t alg, uint32_t slot, int dec, uint32_t mode,
		     const uint8_t iv[16], const uint8_t *in, uint8_t *out,
		     uint32_t len)
{
	uint8_t fb[16], ks[16];
	uint32_t off;

	memcpy(fb, iv, 16);
	for (off = 0; off < len; off += 16) {
		uint32_t n = (len - off >= 16) ? 16u : (len - off);
		uint32_t i;

		switch (mode) {
		case PQCS_MODE_ECB:
			if (sym_block(alg, slot, dec, in + off, out + off))
				return -1;
			break;

		case PQCS_MODE_CBC:
			if (!dec) {
				uint8_t blk[16];

				for (i = 0; i < 16; i++)
					blk[i] = in[off + i] ^ fb[i];
				if (sym_block(alg, slot, 0, blk, out + off))
					return -1;
				memcpy(fb, out + off, 16);
			} else {
				uint8_t prev[16], blk[16];

				/* 先把密文分组存下来：out 可能与 in 同一块内存，
				 * 解出来的明文会把它盖掉，而它正是下一轮的反馈。 */
				memcpy(prev, in + off, 16);
				if (sym_block(alg, slot, 1, prev, blk))
					return -1;
				for (i = 0; i < 16; i++)
					out[off + i] = blk[i] ^ fb[i];
				memcpy(fb, prev, 16);
			}
			break;

		case PQCS_MODE_CTR:
			/* 密钥流 = E(counter)，加解密同一条路 */
			if (sym_block(alg, slot, 0, fb, ks))
				return -1;
			for (i = 0; i < n; i++)
				out[off + i] = in[off + i] ^ ks[i];
			ctr_inc(fb);
			break;

		case PQCS_MODE_OFB:
			if (sym_block(alg, slot, 0, fb, ks))
				return -1;
			memcpy(fb, ks, 16);         /* 反馈的是密钥流本身 */
			for (i = 0; i < n; i++)
				out[off + i] = in[off + i] ^ ks[i];
			break;

		case PQCS_MODE_CFB: {
			uint8_t ct[16];

			if (sym_block(alg, slot, 0, fb, ks))
				return -1;
			/* 反馈的是**密文**：加密时是刚算出来的，解密时是输入。
			 * 同样先存一份，防 in/out 同址。 */
			for (i = 0; i < n; i++)
				ct[i] = (dec ? in[off + i]
					     : (uint8_t)(in[off + i] ^ ks[i]));
			for (i = 0; i < n; i++)
				out[off + i] = (uint8_t)(dec ? (in[off + i] ^ ks[i])
							     : ct[i]);
			if (n == 16)
				memcpy(fb, ct, 16);
			break;
		}
		default:
			return -1;
		}
	}
	/* 反馈寄存器与密钥流是**密钥相关**的中间量，用完就抹。 */
	memset(fb, 0, sizeof fb);
	memset(ks, 0, sizeof ks);
	return 0;
}

/* ---- 一条请求 ---- */
static uint32_t handle_op(const struct pqcs_req *q, const uint8_t *pay,
			  uint8_t *out, uint32_t *out_len)
{
	*out_len = 0;

	switch (q->op) {
	case OP_PING: {
		/* 版本串把**每个核**的 VERSION 都报出来，这是"装上的是哪一版
		 * bitstream"唯一不靠猜的判据：各核的 VERSION 是非零常量
		 * (0x00010000)，而不存在的核经防火墙读回来是 0（RAZ）。
		 *
		 * mldsa 这一项是后加的。没有它的时候，"新 bitstream 装上了吗"
		 * 只能靠 fpga_manager 的 state，而那个只说明"装了个东西"，
		 * 不说明装的是哪一个 —— 两版 bitstream 的 state 都是 operating。 */
		int n = snprintf((char *)out, PQCS_MAXPAY,
				 "pqchsm_fpgad on FPGA  mlkem=0x%08x sym=0x%08x mldsa=0x%08x",
				 rd(MK_VER), rd(SY_VER), rd(MD_VER));
		*out_len = (uint32_t)n;
		return SDR_OK;
	}

	case OP_RANDOM:
		if (q->a0 == 0 || q->a0 > PQCS_MAXPAY)
			return SDR_INARGERR;
		if (trng_bytes(out, q->a0))
			return SDR_HARDFAIL;
		*out_len = q->a0;
		logf_("RANDOM %u 字节（PL 环振噪声源）", q->a0);
		return SDR_OK;

	case OP_MLKEM_KEYGEN: {
		uint32_t eklen, dklen, ctlen, n, h;
		uint8_t seed[64], buf[4900];

		if (q->a0 > 2)
			return SDR_INARGERR;
		mlkem_len(q->a0, &eklen, &dklen, &ctlen);
		/* d/z 取自**硬件**熵源 —— 真密码机就该这样，不用软件 PRNG */
		if (trng_bytes(seed, 64))
			return SDR_HARDFAIL;
		/* 先挑槽：句柄**就是**金库的槽号，一一对应。
		 * 不做映射表是有意的 —— 多一层间接就多一处可能对不上的地方，
		 * 而这里对不上的后果是"用别人的私钥解自己的密文"。 */
		for (h = 0; h < MAX_KEYS && keys[h].used; h++)
			;
		if (h == MAX_KEYS)
			return SDR_UNKNOWERR;

		/* DK_TO_SLOT：dk 直接写进 PL 的金库，**不从 OUT_DATA 出来**。
		 * 所以下面收到的 n 应当恰好是 ek 的长度 —— 这一条要断言，
		 * 它是"私钥没出硬件"在软件侧唯一能自己核对的证据。 */
		if (mlkem_run(0, q->a0, MKM_DK_TO_SLOT | MKM_SLOT(h),
			      seed, 64, buf, sizeof buf, &n))
			return SDR_HARDFAIL;
		if (n != eklen) {
			logf_("KEYGEN 返回 %u 字节，应当恰好是 ek 的 %u ——"
			      " dk 可能仍然出了总线，拒绝这次结果", n, eklen);
			return SDR_HARDFAIL;
		}
		keys[h].used = 1;
		keys[h].pset = q->a0;
		memcpy(out, &h, 4);
		memcpy(out + 4, buf, eklen);
		*out_len = 4 + eklen;
		memset(buf, 0, sizeof buf);
		memset(seed, 0, sizeof seed);
		logf_("KEYGEN pset=%u → 句柄/槽 %u，ek %u 字节"
		      "（dk %u 字节留在片内金库，本进程没见过它）",
		      q->a0, h, eklen, dklen);
		return SDR_OK;
	}

	case OP_MLKEM_ENCAPS: {
		uint32_t eklen, dklen, ctlen, n;
		uint8_t in[1700], buf[1700];

		if (q->a0 > 2)
			return SDR_INARGERR;
		mlkem_len(q->a0, &eklen, &dklen, &ctlen);
		if (q->len != eklen || eklen + 32 > sizeof in)
			return SDR_INARGERR;
		/* m 也来自硬件熵源 */
		if (trng_bytes(in, 32))
			return SDR_HARDFAIL;
		memcpy(in + 32, pay, eklen);
		if (mlkem_run(1, q->a0, 0, in, 32 + eklen, buf, sizeof buf, &n))
			return SDR_HARDFAIL;
		if (n != 32 + ctlen)
			return SDR_HARDFAIL;
		memcpy(out, buf, n);
		*out_len = n;
		logf_("ENCAPS pset=%u → K 32 + c %u 字节", q->a0, ctlen);
		return SDR_OK;
	}

	case OP_MLKEM_DECAPS: {
		uint32_t eklen, dklen, ctlen, n, h = q->a0;
		uint8_t in[4800];

		if (h >= MAX_KEYS || !keys[h].used)
			return SDR_KEYNOTEXIST;
		mlkem_len(keys[h].pset, &eklen, &dklen, &ctlen);
		if (q->len != ctlen || ctlen > sizeof in)
			return SDR_INARGERR;
		/* **只送密文**。dk 由 PL 从自己的金库里取，一个字节都不经过总线，
		 * 也不经过本进程 —— 这里连一个能放 dk 的缓冲区都不存在了。 */
		memcpy(in, pay, ctlen);
		if (mlkem_run(2, keys[h].pset,
			      MKM_DK_FROM_SLOT | MKM_SLOT(h),
			      in, ctlen, out, PQCS_MAXPAY, &n))
			return SDR_HARDFAIL;
		memset(in, 0, sizeof in);
		*out_len = n;
		logf_("DECAPS 句柄/槽 %u → K %u 字节（dk 全程在片内）", h, n);
		return SDR_OK;
	}

	/* ========================================================================
	 * 【ML-DSA：从机在槽 6，这条路已在真硬件上跑通】
	 * ========================================================================
	 * 2026-08-17：0x8006_0000 上是 mldsa_axi，下面三条在两种位流形态下都端到端
	 * 跑通了（三个参数集各一遍 KeyGen/Sign/Verify，见 board/logs/）。
	 * 硬件不可达时它们仍然如实失败 —— 不是回落到软件。本进程一行密码运算都不做。
	 *
	 * ⚠️ 这条路能通，前提是 **BL31 的 SiP 白名单里有槽 6**。少了那一行，
	 * 送检形态下每一笔访问都被 EL3 拒，而症状是"位流装好了却什么都读不出来"，
	 * 极易误判成位流或 RTL 的问题。见 boot/atf/patch_atf_secmmio.py 的槽表。
	 *
	 * 关于 ctx（FIPS 204 的 M' = [0,|ctx|]‖ctx‖msg）：
	 * 已定的寄存器约定里，输入字节流是 "sk‖msg"（Sign）和 "pk‖sig‖msg"
	 * （Verify），**没有给 ctx 字节留位置**，只有一个 CTX_LEN 寄存器。
	 * 那就只能有两种做法：
	 *   ① 猜一个位置（比如塞在 msg 前面）并照它送；
	 *   ② 明确拒绝非空 ctx，等从机落地后与 RTL 对齐再放开。
	 * 选 ②。猜错的后果不是报错，是**签在了另一条 M' 上** —— 签名合法、
	 * 验得过、却不是调用方以为的那条消息，而且软件侧无从发现。
	 * PKCS#11 那条路径本来就恒传 ctx_len=0，所以这条限制今天不挡任何人。
	 */
	case OP_MLDSA_KEYGEN: {
		uint32_t pklen, sklen, siglen, n, h;
		uint8_t xi[32];

		if (q->a0 > 2)
			return SDR_INARGERR;
		mldsa_len(q->a0, &pklen, &sklen, &siglen);
		/* ξ 取自**硬件**熵源，与 ML-KEM 的 d/z 同一条纪律 */
		if (trng_bytes(xi, 32))
			return SDR_HARDFAIL;
		for (h = 0; h < MLDSA_KEY_SLOTS && dsa_keys[h].used; h++)
			;
		if (h == MLDSA_KEY_SLOTS)
			return SDR_UNKNOWERR;

		/* SK_TO_SLOT：sk 进片内金库，不从 OUT_DATA 出来。
		 * 于是 OUT_LEN 应当恰好是 pk 的长度 —— 这一条要断言，
		 * 它是"私钥没出硬件"在软件侧唯一能自己核对的证据。 */
		if (mldsa_run(MDO_KEYGEN, q->a0, MDM_SK_TO_SLOT | MDM_SLOT(h),
			      xi, 32, 0, 0, out + 4, PQCS_MAXPAY - 4, &n, NULL)) {
			memset(xi, 0, sizeof xi);
			return SDR_HARDFAIL;
		}
		memset(xi, 0, sizeof xi);
		if (n != pklen) {
			logf_("ML-DSA KEYGEN 返回 %u 字节，应当恰好是 pk 的 %u ——"
			      " sk 可能仍然出了总线，拒绝这次结果", n, pklen);
			return SDR_HARDFAIL;
		}
		dsa_keys[h].used = 1;
		dsa_keys[h].pset = q->a0;
		memcpy(out, &h, 4);
		*out_len = 4 + pklen;
		logf_("ML-DSA KEYGEN pset=%u → 句柄/槽 %u，pk %u 字节"
		      "（sk %u 字节留在片内金库，本进程没见过它）",
		      q->a0, h, pklen, sklen);
		return SDR_OK;
	}

	case OP_MLDSA_SIGN: {
		uint32_t pklen, sklen, siglen, n, h = q->a0;

		if (h >= MLDSA_KEY_SLOTS || !dsa_keys[h].used)
			return SDR_KEYNOTEXIST;
		if (q->a1 != 0)
			return SDR_INARGERR;   /* 非空 ctx：见上面那段 */
		mldsa_len(dsa_keys[h].pset, &pklen, &sklen, &siglen);
		/* 送的是 **rnd(32) ‖ msg**，不是只送 msg。
		 *
		 * ⚠️ 这里原来只送 msg，注释还写着"只送消息" —— 那是 RTL 加进
		 *    rnd/ctx **之前**的旧约定。RTL 现在的输入排布是
		 *        [sk，仅当不走金库] ‖ rnd(32) ‖ ctx ‖ msg
		 *    走金库时 START 的长度门槛是 32+ctx_len+msg_len。只送 msg 的话
		 *    IN_PTR 差 32 字节，**START 必被 LEN_ERR 拒、根本不启动**——
		 *    也就是说签名在真硬件上一次都成功不了。
		 *    仿真侧发现不了它：cocotb 的用例是照 RTL 的排布喂的，
		 *    而 daemon 这条路当时没有硬件可跑。
		 *
		 * rnd 取自 **PL 的环振噪声源**（FIPS 204 的 hedged 模式）。
		 * 不用全零：全零是确定性签名，它把签名变成消息的纯函数，
		 * 少了一层对故障注入与侧信道的防护。确定性只在对 ACVP 的
		 * 确定性向量时才需要，那条路走的是 mldsa_hwtest，不是这里。
		 *
		 * sk 仍然一个字节都不经过总线、也不经过本进程 —— 这里连一个能放
		 * sk 的缓冲区都不存在，rnd 是另一回事。 */
		if (32u + q->len > PQCS_MAXPAY)
			return SDR_INARGERR;
		{
			uint8_t *sbuf = malloc(32u + q->len);
			int rv;
			if (!sbuf)
				return SDR_UNKNOWERR;
			if (trng_bytes(sbuf, 32)) {
				free(sbuf);
				logf_("ML-DSA SIGN 取 rnd 失败（TRNG）");
				return SDR_HARDFAIL;
			}
			memcpy(sbuf + 32, pay, q->len);
			rv = mldsa_run(MDO_SIGN, dsa_keys[h].pset,
				       MDM_SK_FROM_SLOT | MDM_SLOT(h),
				       sbuf, 32u + q->len, q->len, 0,
				       out, PQCS_MAXPAY, &n, NULL);
			memset(sbuf, 0, 32u + q->len);
			free(sbuf);
			if (rv)
				return SDR_HARDFAIL;
		}
		if (n != siglen) {
			logf_("ML-DSA SIGN 返回 %u 字节，应为 %u", n, siglen);
			return SDR_HARDFAIL;
		}
		*out_len = n;
		logf_("ML-DSA SIGN 句柄/槽 %u，msg %u 字节 → sig %u 字节"
		      "（sk 全程在片内）", h, q->len, n);
		return SDR_OK;
	}

	case OP_MLDSA_VERIFY: {
		uint32_t pklen, sklen, siglen, n = 0, msg_len;
		int ok = 0;

		if (q->a0 > 2)
			return SDR_INARGERR;
		if (q->a1 != 0)
			return SDR_INARGERR;   /* 非空 ctx：见上面那段 */
		mldsa_len(q->a0, &pklen, &sklen, &siglen);
		if (q->len < pklen + siglen)
			return SDR_INARGERR;
		msg_len = q->len - pklen - siglen;
		/* 验签只用公钥，载荷 pk‖sig‖msg 与硬件要的字节流逐字节一致，
		 * 直接把 pay 送下去，不多抄一遍。 */
		if (mldsa_run(MDO_VERIFY, q->a0, 0, pay, q->len, msg_len, 0,
			      out, PQCS_MAXPAY, &n, &ok))
			return SDR_HARDFAIL;
		*out_len = 0;
		logf_("ML-DSA VERIFY pset=%u msg %u 字节 → %s",
		      q->a0, msg_len, ok ? "通过" : "不通过");
		/* 验不过是**结果**不是故障，但线上仍然要用一个非 SDR_OK 的码回它：
		 * 只有这样，"忘了看返回值的载荷"才不会被当成验过了。
		 * 老客户端不认识这个码，会走 != SDR_OK 的分支 —— fail-closed。 */
		return ok ? SDR_OK : SDR_VERIFYFAIL;
	}

	case OP_IMPORT_KEY: {
		uint8_t k32[32];
		uint32_t i;

		if (q->a0 > 7 || q->len == 0 || q->len > 32)
			return SDR_INARGERR;
		memset(k32, 0, sizeof k32);
		memcpy(k32, pay, q->len);
		wr(V_SLOT, q->a0);
		wr(V_SLOTC, 1);                       /* BEGIN */
		for (i = 0; i < 8; i++)
			wr(V_KEYIN,
			   ((uint32_t)k32[4*i] << 24) | ((uint32_t)k32[4*i+1] << 16) |
			   ((uint32_t)k32[4*i+2] << 8) | k32[4*i+3]);
		wr(V_SLOTC, 2);                       /* COMMIT */
		memset(k32, 0, sizeof k32);
		logf_("IMPORT_KEY 槽 %u（%u 字节）—— 进 key_vault，再也读不回来",
		      q->a0, q->len);
		return SDR_OK;
	}

	case OP_SYM_BLOCK: {
		uint32_t slot = q->a1 & 0xFF, dec = (q->a1 >> 8) & 1;

		if (q->len != 16)
			return SDR_INARGERR;
		/* ⚠️ alg 和 slot 必须在这里挡住，因为 **RTL 是静默截断的**：
		 * sym_axi 里是 `alg <= wr_data[1:0]`、槽号 3 位。于是
		 *   alg=4 → 按 0（AES-128）算，alg=7 → 按 3（SM3）算，
		 *   槽 200 → 按槽 0 算。
		 * 不报错、不卡住，**只是算的不是你要的东西** —— 这是最坏的一类
		 * 失败：调用方拿到一个看着完全正常的密文，而它来自另一个算法
		 * 或另一把密钥。
		 *
		 * alg 只放 0/1/2（AES-128 / AES-256 / SM4）。RTL 的 2'd3 是 SM3，
		 * 那是个杂凑，走分组密码这条路（装密钥 → 出 16 字节密文）本身
		 * 就没有意义，不能从这个接口进去。
		 *
		 * 这一条也顺带把 OP_IMPORT_KEY 与本 case 的不一致抹平了 ——
		 * 那边一直校验 slot<=7，这边以前没有。 */
		if (q->a0 > 2 || slot > 7)
			return SDR_INARGERR;
		if (sym_block(q->a0, slot, (int)dec, pay, out))
			return SDR_HARDFAIL;
		*out_len = 16;
		logf_("SYM_BLOCK alg=%u 槽=%u %s", q->a0, slot,
		      dec ? "解密" : "加密");
		return SDR_OK;
	}
	/* ---- 工作模式：一次调用处理整段数据 ----------------------------------
	 * 逐分组走一次 socket 也能实现，但那是每 16 字节一次往返；而且模式状态
	 * （反馈寄存器/计数器）会落到调用方手里，调用方一旦复用就是密钥流复用。
	 * 放在这里，状态出不了本进程，用完就抹。 */
	case OP_SYM_CRYPT: {
		uint32_t alg  = q->a0 & 0xFFu;
		uint32_t dec  = (q->a0 >> 8) & 1u;
		uint32_t mode = (q->a0 >> 16) & 0xFFu;
		uint32_t slot = q->a1 & 0xFFu;
		uint32_t dlen;

		/* 同 OP_SYM_BLOCK：alg/slot 必须在这里挡住，RTL 是静默截断的。 */
		if (alg > 2 || slot > 7 || mode > PQCS_MODE_OFB)
			return SDR_INARGERR;
		if (q->len < 16)                 /* 至少要有 IV */
			return SDR_INARGERR;
		dlen = q->len - 16u;
		if (dlen == 0)
			return SDR_INARGERR;
		/* ECB/CBC 不做填充，长度必须对齐；其余三种任意长度。 */
		if ((mode == PQCS_MODE_ECB || mode == PQCS_MODE_CBC) && (dlen % 16u))
			return SDR_INARGERR;
		if (dlen > PQCS_MAXPAY)
			return SDR_INARGERR;

		if (sym_crypt(alg, slot, (int)dec, mode, pay, pay + 16, out, dlen))
			return SDR_HARDFAIL;
		*out_len = dlen;
		logf_("SYM_CRYPT alg=%u 模式=%u %s 槽 %u，%u 字节（密钥没出金库）",
		      alg, mode, dec ? "解密" : "加密", slot, dlen);
		return SDR_OK;
	}


	default:
		return SDR_INARGERR;
	}
}

static int read_all(int fd, void *p, size_t n)
{
	uint8_t *b = p;
	size_t got = 0;

	while (got < n) {
		ssize_t r = read(fd, b + got, n - got);

		if (r <= 0)
			return -1;
		got += (size_t)r;
	}
	return 0;
}

static int write_all(int fd, const void *p, size_t n)
{
	const uint8_t *b = p;
	size_t put = 0;

	while (put < n) {
		ssize_t r = write(fd, b + put, n - put);

		if (r <= 0)
			return -1;
		put += (size_t)r;
	}
	return 0;
}

int main(int argc, char **argv)
{
	int srv, i, want_lock = 0;
	struct sockaddr_un sa;
	char st[64] = {0};
	FILE *f;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-f"))
			fg = 1;
		else if (!strcmp(argv[i], "-lock"))
			want_lock = 1;
	}
	(void)fg;

	/* 闸门：先确认 PL 已 programmed 再发第一笔 SMC（这一步不碰 PL 总线）。
	 * PL 不在就发 SMC 的话，那笔访问会在 EL3 上出错，而 BL31 里没有处理它的
	 * 东西 —— 发 SMC 的核当场卡死。 */
	f = fopen("/sys/class/fpga_manager/fpga0/state", "r");
	if (!f || !fgets(st, sizeof st, f)) {
		logf_("读不到 fpga_manager state"); return 2;
	}
	fclose(f);
	if (!strstr(st, "operating")) {
		logf_("PL 不是 operating（%s），拒绝启动", st); return 2;
	}

	sec_fd = open("/dev/secmmio", O_RDWR);
	if (sec_fd < 0) { logf_("打不开 /dev/secmmio：%s", strerror(errno)); return 2; }
	if (ioctl(sec_fd, SECMMIO_ARM) < 0) { logf_("SECMMIO_ARM 失败"); return 2; }

	/* ---- 启动自检：读一个核的 VERSION，确认这条路真的通到硬件 ----
	 *
	 * 少了这一步，"daemon 起来了" 只说明它打开了几个文件描述符。
	 * 实测踩过：BOOT.BIN 里的 BL31 没带 SiP，/dev/secmmio 照样存在
	 * （模块自己建的节点），daemon 照常监听，开机脚本照常报 READY=yes，
	 * 直到第一个客户端连上来才发现一笔都发不出去。
	 *
	 * 判据用 VERSION 而不是"ioctl 没报错"：它是个非零常量，
	 * 读到它等于证明了整条链（SMC → EL3 白名单 → AXI → 核）都是通的。 */
	{
		uint32_t ver = 0;

		hw_fault = 0;
		if (hw_rd(TR_VERSION, &ver) || ver != CORE_VERSION) {
			logf_("自检失败：读 TRNG VERSION 得到 0x%08x（应为 0x%08x）。"
			      "多半是当前 BOOT.BIN 的 BL31 没有那个 SiP —— "
			      "拒绝启动，免得对外假装就绪。", ver, CORE_VERSION);
			return 2;
		}
		logf_("自检通过：TRNG VERSION = 0x%08x，EL3 通路可用", ver);
	}

	/* ---- 可选：把私钥外泄闩锁置上（-lock）----
	 *
	 * 置上之后 ML-KEM 的 KeyGen **在硬件里**就不再把 dk 送出总线，
	 * 无论谁怎么写 MODE 寄存器 —— 包括本 daemon 自己。这把"私钥留在片内"
	 * 从一句实现承诺变成一条硬件性质。
	 *
	 * **默认不置**，因为 ACVP 的 KeyGen 向量要核对 dk：那是出厂验证必须做的
	 * 事，而闩锁一旦置上只有重新装载位流才能解开。交付/演示形态由
	 * hsm-boot.sh 传 -lock；跑 KAT 时不传。这个取舍写在 docs 里。 */
	if (want_lock) {
		uint32_t ks = 0;

		hw_fault = 0;
		/* 上一个 daemon 可能刚发过 ZEROIZE 就退出了 —— 那时金库还在擦，
		 * 而擦除期间写会被拒，被拒的写从 EL3 回来是 SError（丢板子）。
		 * 先用读等它落下来，等不到就干脆不置闩锁、如实拒绝启动。 */
		if (wait_not_wiping(MK_STATUS, MKS_WIPING, "ML-KEM") ||
		    wait_not_wiping(MD_STATUS, MDS_WIPING, "ML-DSA")) {
			logf_("金库还在擦，拒绝启动 —— 这时候写 CTRL 会 SError");
			return 2;
		}
		wr(MK_CTRL, MKC_DKLOCK);
		if (hw_rd(MK_KEYSTAT, &ks) || !(ks & (1u << 16))) {
			logf_("置私钥闩锁失败（KEYSTAT=0x%08x）—— 拒绝启动，"
			      "免得对外声称私钥出不来而其实出得来", ks);
			return 2;
		}
		logf_("私钥外泄闩锁已置上：KeyGen 不会再把 dk 送出总线");

		/* ML-DSA 那把闩锁同理，但**只在从机确实存在时才置**。
		 *
		 * 0x8006_0000 在旧位流/旧白名单下可能是读不到的。对着读不到的地址写
		 * 会在 EL3 上被拒，而这个启动分支把任何一笔失败都判成"拒绝启动" ——
		 * 于是加这几行的直接后果会是**板子起不来**，跟 ML-DSA 有没有毫无关系。
		 *
		 * 所以先用 VERSION 探一下：读得到约定的常量才认为有从机。
		 * 探测走 hw_rd 而不是 rd()，因为后者会把失败粘进 hw_fault，
		 * 让"没有这个从机"这件正常的事污染掉后面的判断。 */
		uint32_t dver = 0;

		if (hw_rd(MD_VER, &dver) == 0 && dver == CORE_VERSION) {
			uint32_t dks = 0;

			wr(MD_CTRL, MDC_SKLOCK);
			if (hw_rd(MD_KEYSTAT, &dks) || !(dks & (1u << 8))) {
				logf_("置 ML-DSA 私钥闩锁失败（KEYSTAT=0x%08x）—— 拒绝启动",
				      dks);
				return 2;
			}
			logf_("ML-DSA 私钥外泄闩锁已置上");
		} else {
			logf_("0x%08lx 上没有 mldsa_axi（VERSION=0x%08x）——"
			      " 跳过 ML-DSA 闩锁。**签名这条路今天不在硬件上**",
			      PL_BASE + S_MLDSA, dver);
		}
	}

	/* ---- 远程口令：读得到就开 TCP，读不到就**不开** ----
	 *
	 * fail-closed 是有意的：一个能驱动密码机的端口裸奔在内网上，
	 * 比"远程功能用不了"糟得多。所以没有口令文件时，daemon 照常
	 * 提供本机 UNIX socket，只是不监听 TCP，并在日志里说清楚原因。 */
	{
		int tf = open(PQCS_TOKEN_PATH, O_RDONLY);

		if (tf >= 0) {
			ssize_t n = read(tf, token, sizeof token - 1);

			close(tf);
			while (n > 0 && (token[n-1] == '\n' || token[n-1] == '\r'))
				n--;            /* 文件末尾的换行不算口令的一部分 */
			if (n >= 8) {
				token_len = (size_t)n;
				token[n] = 0;
			} else {
				logf_("口令文件短于 8 字节，当作没有；不监听 TCP");
			}
		} else {
			logf_("没有 %s，不监听 TCP（只提供本机 socket）", PQCS_TOKEN_PATH);
		}
	}

	signal(SIGPIPE, SIG_IGN);
	unlink(PQCS_SOCK_PATH);
	srv = socket(AF_UNIX, SOCK_STREAM, 0);
	if (srv < 0) { logf_("socket: %s", strerror(errno)); return 1; }
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	strncpy(sa.sun_path, PQCS_SOCK_PATH, sizeof sa.sun_path - 1);
	if (bind(srv, (struct sockaddr *)&sa, sizeof sa) < 0) {
		logf_("bind: %s", strerror(errno)); return 1;
	}
	chmod(PQCS_SOCK_PATH, 0600);
	listen(srv, 8);

	if (token_len) {
		struct sockaddr_in in4;
		int one = 1;

		tcp_srv = socket(AF_INET, SOCK_STREAM, 0);
		if (tcp_srv >= 0) {
			setsockopt(tcp_srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
			memset(&in4, 0, sizeof in4);
			in4.sin_family = AF_INET;
			in4.sin_addr.s_addr = htonl(INADDR_ANY);
			in4.sin_port = htons(PQCS_TCP_PORT);
			if (bind(tcp_srv, (struct sockaddr *)&in4, sizeof in4) < 0 ||
			    listen(tcp_srv, 8) < 0) {
				logf_("TCP 监听失败：%s", strerror(errno));
				close(tcp_srv); tcp_srv = -1;
			} else {
				logf_("TCP 监听 :%d（需要口令）", PQCS_TCP_PORT);
			}
		}
	}
	logf_("就绪：%s（每一笔核访问都经 EL3 发出）", PQCS_SOCK_PATH);

	for (;;) {
		int c, is_tcp = 0, authed = 0;
		struct pqcs_req q;
		struct pqcs_resp rp;
		static uint8_t pay[PQCS_MAXPAY], out[PQCS_MAXPAY];
		fd_set rfds;
		int mx = srv;

		/* 两个监听口一起等。**一次只处理一个连接** —— 硬件序列必须
		 * 串行化（见文件头），并发在这里没有任何好处，只会带来交错。 */
		FD_ZERO(&rfds);
		FD_SET(srv, &rfds);
		if (tcp_srv >= 0) {
			FD_SET(tcp_srv, &rfds);
			if (tcp_srv > mx) mx = tcp_srv;
		}
		if (select(mx + 1, &rfds, NULL, NULL, NULL) < 0)
			continue;

		if (tcp_srv >= 0 && FD_ISSET(tcp_srv, &rfds)) {
			struct sockaddr_in peer;
			socklen_t pl = sizeof peer;

			c = accept(tcp_srv, (struct sockaddr *)&peer, &pl);
			is_tcp = 1;
			if (c >= 0)
				logf_("远程连上 %s:%d", inet_ntoa(peer.sin_addr),
				      ntohs(peer.sin_port));
		} else {
			c = accept(srv, NULL, NULL);
			if (c >= 0)
				logf_("本机客户端连上");
		}
		if (c < 0)
			continue;

		if (is_tcp) {
			/* 超时是**可用性**要求，不是安全要求：服务是单线程的，
			 * 一个连上就不说话（或者网络中断）的远程客户端，会把
			 * 后面所有人一起挡住。演示里这表现为"密码机忽然没反应了"。
			 * 有了超时，最坏情况是卡 10 秒而不是永远。 */
			struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
			int one = 1;

			setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
			setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
			setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		}
		/* 一次一个连接、一条一条处理 —— 硬件序列必须串行化（见文件头） */
		while (read_all(c, &q, sizeof q) == 0) {
			if (q.magic != PQCS_MAGIC || q.len > PQCS_MAXPAY)
				break;
			if (q.len && read_all(c, pay, q.len))
				break;
			rp.magic = PQCS_MAGIC;

			/* ---- TCP：先认口令，认过之前什么都不给做 ---- */
			if (is_tcp && !authed) {
				if (q.op != OP_AUTH) {
					logf_("远程未认证就发 op=%u，断开", q.op);
					break;
				}
				rp.len = 0;
				if (q.len == token_len && q.len <= PQCS_MAXPAY &&
				    ct_eq(pay, token, token_len)) {
					authed = 1;
					rp.status = SDR_OK;
					logf_("远程认证通过");
				} else {
					rp.status = SDR_AUTHFAIL;
					logf_("远程口令不对，断开");
				}
				write_all(c, &rp, sizeof rp);
				if (!authed)
					break;
				continue;
			}
			/* 本机 socket 是 0600 的，能连上就已经是 root；
			 * 对它要求口令没有增加任何东西，只会让本机工具更难用。 */

			/* 每个请求独立判定硬件是否可用：清零 → 处理 → 检查。
			 * 不这么做的话，一次失败会永久污染后面所有请求；
			 * 而硬件确实可能恢复（比如运行时重新装载了位流）。 */
			hw_fault = 0;
			/* ⚠️ 处理任何请求之前先确认两个 PQC 从机都不在 WIPING。
			 * handle_op 里第一笔就可能是写，而**写被拒 = SError = 丢板子**
			 * （见 wait_not_wiping 上面那段）。这两笔是读，代价可以忽略，
			 * 换来的是"不会因为上一个人刚断开就把板子写死"。 */
			if (wait_not_wiping(MK_STATUS, MKS_WIPING, "ML-KEM") ||
			    wait_not_wiping(MD_STATUS, MDS_WIPING, "ML-DSA")) {
				rp.status = SDR_HARDFAIL;
				rp.len = 0;
				if (write_all(c, &rp, sizeof rp))
					break;
				continue;
			}
			hw_fault = 0;
			rp.status = handle_op(&q, pay, out, &rp.len);
			if (hw_fault && rp.status == SDR_OK) {
				/* handle_op 里那些 rd() 返回的 0 是假数据，
				 * 照它算出来的结果**必须**判失败 ——
				 * 悄悄返回一个基于全 0 的"成功"是最坏的结局。 */
				rp.status = SDR_HARDFAIL;
				rp.len = 0;
			}
			if (write_all(c, &rp, sizeof rp))
				break;
			if (rp.len && write_all(c, out, rp.len))
				break;
		}
		close(c);
		/* ⚠️ 会话结束的清理必须在**这里**，不能放在 for(;;) 之后。
		 * 那个位置以前有一句 keys_wipe()，而外层循环没有任何 break /
		 * return / exit —— 它是**不可达代码**，一次都没执行过。
		 * 死代码写在安全清理这种地方，比没写更糟：审的人会以为清了。 */
		hw_fault = 0;
		session_end();
		hw_fault = 0;
		logf_("客户端断开（句柄表已清，两个 PQC 金库已擦）");
	}
	return 0;
}
