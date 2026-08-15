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
//   · **ML-KEM 的 dk** 当前会随 KeyGen 一起从硬件出来（ACVP 核对的需要）。
//     本 daemon 把它留在自己的进程内存里，只给应用一个句柄。
//     所以从**应用**视角看私钥不出接口；但它确实出了**硬件**。
//     这两句话不一样，文档里分开写（docs/API.md §4）。
//     进程退出前把 dk 表抹掉，减少驻留时间。
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

#define PL_BASE   0x80000000UL
#define S_TRNG    0x00000
#define S_VAULT   0x10000
#define S_SYM     0x20000
#define S_MLKEM   0x30000

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
#define MK_KEYSTAT (S_MLKEM + 0x30)
#define MK_KEYPSET (S_MLKEM + 0x34)
/* MODE 里控制片内私钥金库的三个字段 */
#define MKM_DK_TO_SLOT   0x10u           /* KeyGen：dk 进金库，不出总线 */
#define MKM_DK_FROM_SLOT 0x20u           /* Decaps：dk 从金库取 */
#define MKM_SLOT(s)      (((s) & 15u) << 6)   /* MODE[9:6]，16 个槽 */
#define PL_KEY_SLOTS     16              /* 金库有 16 个槽（64 KB / 4096） */
#define MKS_DONE  (1u << 1)
#define MKS_PARER (1u << 5)

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

static void keys_wipe(void)
{
	memset(keys, 0, sizeof keys);
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

/* ---- 一条请求 ---- */
static uint32_t handle_op(const struct pqcs_req *q, const uint8_t *pay,
			  uint8_t *out, uint32_t *out_len)
{
	*out_len = 0;

	switch (q->op) {
	case OP_PING: {
		int n = snprintf((char *)out, PQCS_MAXPAY,
				 "pqchsm_fpgad on FPGA  mlkem=0x%08x sym=0x%08x",
				 rd(MK_VER), rd(SY_VER));
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
		wr(MK_CTRL, MKC_DKLOCK);
		if (hw_rd(MK_KEYSTAT, &ks) || !(ks & (1u << 16))) {
			logf_("置私钥闩锁失败（KEYSTAT=0x%08x）—— 拒绝启动，"
			      "免得对外声称私钥出不来而其实出得来", ks);
			return 2;
		}
		logf_("私钥外泄闩锁已置上：KeyGen 不会再把 dk 送出总线");
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
		logf_("客户端断开");
	}
	keys_wipe();
	return 0;
}
