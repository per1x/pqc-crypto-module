// pqchsm_fpgad —— 密码机服务 daemon：把标准接口的请求落到 FPGA 密码核上
//
//   pqchsm_fpgad [-f]        -f 前台运行（板上调试用）
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
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "wire.h"
#include "../board/kmod/secmmio_uapi.h"

#define SDR_OK          0x00000000u
#define SDR_UNKNOWERR   0x01000001u
#define SDR_COMMFAIL    0x01000003u
#define SDR_INARGERR    0x01000004u
#define SDR_KEYNOTEXIST 0x01000005u
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

static uint32_t rd(unsigned off)
{
	uint32_t v = 0;

	if (hw_rd(off, &v))
		logf_("读 0x%08lx 被 EL3 白名单拒", PL_BASE + off);
	return v;
}
static void wr(unsigned off, uint32_t v) { (void)hw_wr(off, v); }

/* ---- TRNG ---- */
static int trng_bytes(uint8_t *out, uint32_t n)
{
	uint32_t got = 0, w;
	long spin;

	wr(TR_CTRL, 1);
	for (spin = 0; spin < 20000000L; spin++)
		if (rd(TR_STATUS) & TRS_STARTUP)
			break;
	if (!(rd(TR_STATUS) & TRS_STARTUP))
		return -1;
	while (got < n) {
		for (spin = 0; spin < 20000000L; spin++)
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
static int mlkem_run(uint32_t mode, uint32_t pset,
		     const uint8_t *in, uint32_t in_len,
		     uint8_t *out, uint32_t out_cap, uint32_t *out_len)
{
	uint32_t i, n, st;
	long spin;

	wr(MK_MODE, mode | (pset << 2));
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
#define MAX_KEYS 16
static struct { int used; uint32_t pset, dk_len; uint8_t dk[3200]; } keys[MAX_KEYS];

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
	for (spin = 0; spin < 200000L; spin++)
		if (rd(SY_STATUS) & SYS_KRDY)
			break;
	if (!(rd(SY_STATUS) & SYS_KRDY))
		return -2;
	for (i = 0; i < 4; i++)
		wr(SY_DIN0 + 4 * i,
		   ((uint32_t)in[4*i] << 24) | ((uint32_t)in[4*i+1] << 16) |
		   ((uint32_t)in[4*i+2] << 8) | in[4*i+3]);
	wr(SY_CMD, 2);
	for (spin = 0; spin < 200000L; spin++)
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
		if (mlkem_run(0, q->a0, seed, 64, buf, sizeof buf, &n))
			return SDR_HARDFAIL;
		if (n != eklen + dklen)
			return SDR_HARDFAIL;
		for (h = 0; h < MAX_KEYS && keys[h].used; h++)
			;
		if (h == MAX_KEYS)
			return SDR_UNKNOWERR;
		keys[h].used = 1;
		keys[h].pset = q->a0;
		keys[h].dk_len = dklen;
		memcpy(keys[h].dk, buf + eklen, dklen);
		/* 回给应用的**只有** ek 和句柄；dk 留在这里 */
		memcpy(out, &h, 4);
		memcpy(out + 4, buf, eklen);
		*out_len = 4 + eklen;
		memset(buf, 0, sizeof buf);
		memset(seed, 0, sizeof seed);
		logf_("KEYGEN pset=%u → 句柄 %u，ek %u 字节（dk %u 字节留在 daemon）",
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
		if (mlkem_run(1, q->a0, in, 32 + eklen, buf, sizeof buf, &n))
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
		if (q->len != ctlen || dklen + ctlen > sizeof in)
			return SDR_INARGERR;
		/* dk 从句柄表取，应用没见过它 */
		memcpy(in, keys[h].dk, dklen);
		memcpy(in + dklen, pay, ctlen);
		if (mlkem_run(2, keys[h].pset, in, dklen + ctlen,
			      out, PQCS_MAXPAY, &n))
			return SDR_HARDFAIL;
		memset(in, 0, sizeof in);
		*out_len = n;
		logf_("DECAPS 句柄 %u → K %u 字节", h, n);
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
	int srv, i;
	struct sockaddr_un sa;
	char st[64] = {0};
	FILE *f;

	for (i = 1; i < argc; i++)
		if (!strcmp(argv[i], "-f"))
			fg = 1;
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
	logf_("就绪：%s（每一笔核访问都经 EL3 发出）", PQCS_SOCK_PATH);

	for (;;) {
		int c = accept(srv, NULL, NULL);
		struct pqcs_req q;
		struct pqcs_resp rp;
		static uint8_t pay[PQCS_MAXPAY], out[PQCS_MAXPAY];

		if (c < 0)
			continue;
		logf_("客户端连上");
		/* 一次一个连接、一条一条处理 —— 硬件序列必须串行化（见文件头） */
		while (read_all(c, &q, sizeof q) == 0) {
			if (q.magic != PQCS_MAGIC || q.len > PQCS_MAXPAY)
				break;
			if (q.len && read_all(c, pay, q.len))
				break;
			rp.magic = PQCS_MAGIC;
			rp.status = handle_op(&q, pay, out, &rp.len);
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
