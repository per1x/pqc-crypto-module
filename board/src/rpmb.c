/* rpmb.c —— eMMC RPMB 的最小实现。接口与取舍见 rpmb.h。
 *
 * 帧格式来自 JESD84-B51 §6.6.22。三条注意事项，写错了都不会编译报错：
 *   ① 帧里所有多字节字段都是**大端**（跟这个项目其它地方相反）；
 *   ② MAC 覆盖的是帧的**后 284 字节**（从 data 开始到帧尾），不是整帧；
 *   ③ 认证写必须用**可靠写**（write_flag 的最高位），否则计数器不会推进，
 *      而 result 仍然可能是 0 —— 症状是"写成功了但计数器没动"。
 */
#define _GNU_SOURCE
#include "rpmb.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/major.h>   /* MMC_IOC_MULTI_CMD 的 _IOWR 用到 MMC_BLOCK_MAJOR */
#include <linux/mmc/ioctl.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MMC_READ_MULTIPLE_BLOCK  18
#define MMC_WRITE_MULTIPLE_BLOCK 25

/* 这几个位在内核的 <linux/mmc/core.h> 里，而那个头不是 uapi，
 * 交叉 sysroot 里没有 —— mmc-utils 也是自己抄一份。 */
#define MMC_RSP_PRESENT (1u << 0)
#define MMC_RSP_CRC     (1u << 2)
#define MMC_RSP_OPCODE  (1u << 4)
#define MMC_CMD_ADTC    (1u << 5)
#define MMC_RSP_SPI_S1  (1u << 7)
#define MMC_RSP_R1      (MMC_RSP_PRESENT | MMC_RSP_CRC | MMC_RSP_OPCODE)
#define MMC_RSP_SPI_R1  (MMC_RSP_SPI_S1)

/* 可靠写：write_flag 的第 31 位。见 ③。 */
#define MMC_WRITE_REL   (1u << 31)

struct rpmb_frame {
	uint8_t stuff[196];
	uint8_t key_mac[32];
	uint8_t data[256];
	uint8_t nonce[16];
	uint8_t write_counter[4];
	uint8_t addr[2];
	uint8_t block_count[2];
	uint8_t result[2];
	uint8_t req_resp[2];
};

/* MAC 覆盖范围：从 data 起到帧尾 */
#define MAC_OFF   (196 + 32)
#define MAC_LEN   (512 - MAC_OFF)

#define REQ_PROGRAM_KEY 0x0001
#define REQ_READ_CNT    0x0002
#define REQ_WRITE_DATA  0x0003
#define REQ_RESULT_READ 0x0005

static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t get_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

const char *rpmb_result_str(uint16_t r)
{
	switch (r & 0x7) {
	case 0x00: return "OK";
	case 0x01: return "General failure";
	case 0x02: return "Authentication failure（MAC 不对）";
	case 0x03: return "Counter failure";
	case 0x04: return "Address failure";
	case 0x05: return "Write failure";
	case 0x06: return "Read failure";
	case 0x07: return "认证密钥尚未烧写";
	default:   return "未定义";
	}
}

static void frame_mac(const uint8_t key[RPMB_KEY_LEN], const struct rpmb_frame *f,
                      uint8_t out[32])
{
	unsigned int n = 32;

	HMAC(EVP_sha256(), key, RPMB_KEY_LEN,
	     ((const uint8_t *)f) + MAC_OFF, MAC_LEN, out, &n);
}

/* 发一组命令。n_cmds ∈ {2,3}：
 *   2 —— 写请求帧 + 读响应帧（读计数器）
 *   3 —— 写请求帧（可靠写）+ 写"取结果"请求帧 + 读响应帧（写/烧密钥） */
static int do_ioc(const char *dev, int n_cmds,
                  const struct rpmb_frame *req,
                  const struct rpmb_frame *req2,
                  struct rpmb_frame *resp, int reliable)
{
	uint8_t buf[sizeof(struct mmc_ioc_multi_cmd) + 3 * sizeof(struct mmc_ioc_cmd)];
	struct mmc_ioc_multi_cmd *mioc = (struct mmc_ioc_multi_cmd *)buf;
	struct mmc_ioc_cmd *c;
	int fd, rc, i = 0;

	memset(buf, 0, sizeof buf);
	mioc->num_of_cmds = (uint64_t)n_cmds;

	c = &mioc->cmds[i++];
	c->write_flag = reliable ? (MMC_WRITE_REL | 1u) : 1u;
	c->opcode = MMC_WRITE_MULTIPLE_BLOCK;
	c->blksz = 512; c->blocks = 1;
	c->flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_ADTC;
	mmc_ioc_cmd_set_data((*c), req);

	if (n_cmds == 3) {
		c = &mioc->cmds[i++];
		c->write_flag = 1;
		c->opcode = MMC_WRITE_MULTIPLE_BLOCK;
		c->blksz = 512; c->blocks = 1;
		c->flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_ADTC;
		mmc_ioc_cmd_set_data((*c), req2);
	}

	c = &mioc->cmds[i++];
	c->write_flag = 0;
	c->opcode = MMC_READ_MULTIPLE_BLOCK;
	c->blksz = 512; c->blocks = 1;
	c->flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_ADTC;
	mmc_ioc_cmd_set_data((*c), resp);

	fd = open(dev ? dev : RPMB_DEV_PATH, O_RDWR);
	if (fd < 0) {
		return -1;
	}
	rc = ioctl(fd, MMC_IOC_MULTI_CMD, mioc);
	close(fd);
	return rc < 0 ? -2 : 0;
}

/* 响应帧的 req_resp 必须是"这条请求对应的响应码"。
 *
 * ⚠️ 这个校验不是形式主义，是实测逼出来的（2026-08-18）：这块板上
 *    **同一条读请求交替返回正确的响应帧与一份陈的帧**，陈的那份
 *    result=0x0000、counter=0，看起来像"密钥已经烧过、计数器是 0"。
 *    照着它判断的后果是灾难性的：provision 会以为密钥烧过而拒绝，
 *    或者反过来在一次陈帧上判定"烧成功了"。
 *
 *    判据只能是响应帧自己的 req_resp（陈帧里那一项对不上），
 *    不能是 result —— result 恰恰是被污染的那个字段。
 *    对得上才认，对不上就重发。 */
#define RPMB_IO_RETRY 8

int rpmb_read_counter(const char *dev, const uint8_t key[RPMB_KEY_LEN],
                      uint32_t *counter, uint16_t *result)
{
	struct rpmb_frame req, resp;
	uint8_t want[32];
	int tries;

	for (tries = 0; tries < RPMB_IO_RETRY; tries++) {
		memset(&req, 0, sizeof req);
		memset(&resp, 0, sizeof resp);
		put_be16(req.req_resp, REQ_READ_CNT);
		if (RAND_bytes(req.nonce, sizeof req.nonce) != 1) {
			return -1;
		}
		/* ⚠️ ioctl 失败也要重试，不能直接返回。
		 *
		 * 这块 eMMC 的 RPMB **隔次**会让 ioctl 回 EILSEQ（与陈帧是同一个
		 * 毛病的两副面孔）。第一版只在 req_resp 对不上时重试，ioctl 失败
		 * 直接 return -2 —— 于是 rpmb_tool status 里"先无密钥读一次、
		 * 再带密钥读一次"的写法，第二次稳定落在坏的那一半上，
		 * **每次都把"设备 I/O 抖了一下"报成"密钥不对"**。
		 *
		 * 这正是当初毁掉密钥的同一类错误：把 I/O 故障当成设备状态。
		 * 所以这里把两种失败一视同仁地重试，只有重试用尽才认输，
		 * 而且认输的返回码（-6）与"MAC 不匹配"（-4）严格分开 ——
		 * **调用方永远不该把 -6 说成"密钥不对"**。 */
		if (do_ioc(dev, 2, &req, NULL, &resp, 0) != 0) {
			continue;
		}
		if (get_be16(resp.req_resp) == 0x0200) {
			break;      /* 这是这条请求的响应，不是陈帧 */
		}
	}
	if (tries == RPMB_IO_RETRY) {
		return -6;          /* 一直拿不到对得上的响应帧 */
	}
	if (result) {
		*result = get_be16(resp.result);
	}
	if (counter) {
		*counter = get_be32(resp.write_counter);
	}
	if ((get_be16(resp.result) & 0x7) != 0) {
		return -3;      /* 设备自己说不行，MAC 也就不用看了 */
	}
	if (key) {
		/* 验两件事，缺一不可：
		 *   · MAC 对 —— 证明这条响应确实来自持有密钥的那块 eMMC；
		 *   · nonce 回来的是我们刚发的那个 —— 否则录一条旧响应重放即可
		 *     让我们看到一个更小的计数器，防回滚当场失效。 */
		frame_mac(key, &resp, want);
		if (memcmp(want, resp.key_mac, 32) != 0) {
			return -4;
		}
		if (memcmp(req.nonce, resp.nonce, sizeof req.nonce) != 0) {
			return -5;
		}
	}
	return 0;
}

int rpmb_write_block(const char *dev, const uint8_t key[RPMB_KEY_LEN],
                     uint16_t addr, const uint8_t *data, size_t data_len,
                     uint32_t *new_counter, uint16_t *result)
{
	struct rpmb_frame req, req2, resp;
	uint32_t cnt = 0;
	uint16_t r = 0;
	uint8_t want[32];

	if (!key) {
		return -1;
	}
	/* 认证写必须带**当前**计数器，这正是它防重放的原因：
	 * 录下来的写请求换个时刻再放，计数器已经变了，MAC 就对不上。 */
	if (rpmb_read_counter(dev, key, &cnt, &r) != 0) {
		if (result) {
			*result = r;
		}
		return -2;
	}

	memset(&req, 0, sizeof req);
	memset(&req2, 0, sizeof req2);
	memset(&resp, 0, sizeof resp);
	if (data && data_len) {
		memcpy(req.data, data, data_len > sizeof req.data ? sizeof req.data : data_len);
	}
	put_be32(req.write_counter, cnt);
	put_be16(req.addr, addr);
	put_be16(req.block_count, 1);
	put_be16(req.req_resp, REQ_WRITE_DATA);
	frame_mac(key, &req, req.key_mac);

	put_be16(req2.req_resp, REQ_RESULT_READ);

	if (do_ioc(dev, 3, &req, &req2, &resp, 1) != 0) {
		return -3;
	}
	/* 同样要认响应码：写的响应是 0x0300。见 rpmb_read_counter 上面那段。
	 * 写这边**不重发** —— 重发一次认证写会把计数器多推一格，
	 * 而计数器正是我们要当锚点的那个东西。宁可这次失败。 */
	if (get_be16(resp.req_resp) != 0x0300) {
		return -6;
	}
	if (result) {
		*result = get_be16(resp.result);
	}
	if ((get_be16(resp.result) & 0x7) != 0) {
		return -4;
	}
	/* 响应里的计数器是**写完之后**的值。再验一次 MAC —— 这个值将来会被
	 * 当成防回滚锚点写进 keystore，不能来自一条没验过的帧。 */
	frame_mac(key, &resp, want);
	if (memcmp(want, resp.key_mac, 32) != 0) {
		return -5;
	}
	if (new_counter) {
		*new_counter = get_be32(resp.write_counter);
	}
	return 0;
}

int rpmb_program_key(const char *dev, const uint8_t key[RPMB_KEY_LEN],
                     uint16_t *result)
{
	struct rpmb_frame req, req2, resp;

	if (!key) {
		return -1;
	}
	memset(&req, 0, sizeof req);
	memset(&req2, 0, sizeof req2);
	memset(&resp, 0, sizeof resp);
	memcpy(req.key_mac, key, RPMB_KEY_LEN);
	put_be16(req.req_resp, REQ_PROGRAM_KEY);
	put_be16(req2.req_resp, REQ_RESULT_READ);

	if (do_ioc(dev, 3, &req, &req2, &resp, 1) != 0) {
		return -2;
	}
	/* 烧密钥的响应是 0x0100。**这一条尤其不能马虎**：拿一份陈帧当结果，
	 * 会把"烧失败"读成"烧成功"（或者反过来），而这件事不可逆。 */
	if (get_be16(resp.req_resp) != 0x0100) {
		return -4;
	}
	if (result) {
		*result = get_be16(resp.result);
	}
	return (get_be16(resp.result) & 0x7) == 0 ? 0 : -3;
}

int rpmb_key_load(const char *path, uint8_t key[RPMB_KEY_LEN])
{
	FILE *f = fopen(path, "rb");
	uint8_t raw[128];
	size_t n;

	if (!f) {
		return -1;
	}
	n = fread(raw, 1, sizeof raw, f);
	fclose(f);
	if (n == RPMB_KEY_LEN) {
		memcpy(key, raw, RPMB_KEY_LEN);
		return 0;
	}
	/* 也认十六进制：肉眼可读的密钥文件在排查时省很多事，
	 * 而这把密钥的机密性本来就只由文件权限保证（见 rpmb.h）。 */
	if (n >= RPMB_KEY_LEN * 2) {
		size_t i;

		for (i = 0; i < RPMB_KEY_LEN; i++) {
			unsigned v;

			if (sscanf((const char *)raw + i * 2, "%2x", &v) != 1) {
				return -2;
			}
			key[i] = (uint8_t)v;
		}
		return 0;
	}
	return -3;
}
