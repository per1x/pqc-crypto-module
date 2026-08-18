/* rpmb_probe.c —— 查这块板的 eMMC RPMB 到底能不能用来做防回滚锚点
 *
 * ============================================================================
 * 【要回答的是什么】
 * ============================================================================
 * keystore 现在的防回滚锚点是**另一个普通文件**（<keystore>.epoch）。它把
 * "换掉一个文件"提高到"必须一致地换掉两个文件"，仅此而已 —— 能写 SD 卡的
 * 攻击者两个一起换就绕过去了。真正的防回滚需要**攻击者写不了的单调存储**。
 *
 * eMMC 的 RPMB（Replay Protected Memory Block）正是这种东西：
 *   · 有一个**只增不减**的写计数器（Write Counter），硬件维护，无法回退；
 *   · 每一次写都必须带上用认证密钥算的 HMAC-SHA256，且必须带当前计数器值 ——
 *     录下来的写请求重放时计数器已经变了，MAC 对不上，直接被拒；
 *   · 认证密钥**一次性烧写**，烧完读不出来。
 *
 * 所以问题分两层，这个程序只回答第一层：
 *   ① 这块板的 eMMC 有没有 RPMB、有多大？              ← 读 EXT_CSD，纯只读
 *   ② 认证密钥烧过没有？                                ← 读写计数器，纯只读
 *
 * ⚠️ **本程序只读，不烧任何东西。** 认证密钥的烧写是不可逆的，那一步不该藏在
 *    一个叫 probe 的程序里。第二层的结论决定了后面还能不能做，见 docs/SECURITY.md。
 *
 * ============================================================================
 * 【判据】
 * ============================================================================
 * "读写计数器"这条请求不需要密钥就能发出去（响应里的 MAC 才需要密钥去验）。
 * 响应帧里的 result 字段就是答案：
 *   0x0000  正常 —— **密钥已经烧过了**，而且我们没有它
 *   0x0007  Authentication Key not yet programmed —— 密钥还没烧，这块板可用
 * 其它值按 eMMC 规范（JESD84-B51 §6.6.22）解释。
 *
 * 用法：rpmb_probe [/dev/mmcblk0rpmb]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/major.h>
#include <linux/mmc/ioctl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* eMMC 规范里的 RPMB 数据帧：512 字节，大端。字段顺序不能动。 */
struct rpmb_frame {
	uint8_t  stuff[196];
	uint8_t  key_mac[32];
	uint8_t  data[256];
	uint8_t  nonce[16];
	uint8_t  write_counter[4];
	uint8_t  addr[2];
	uint8_t  block_count[2];
	uint8_t  result[2];
	uint8_t  req_resp[2];
};

#define RPMB_REQ_READ_CNT   0x0002
#define RPMB_RESP_READ_CNT  0x0200

#define MMC_READ_MULTIPLE_BLOCK  18
#define MMC_WRITE_MULTIPLE_BLOCK 25

/* ⚠️ 这几个响应类型的位定义在内核的 <linux/mmc/core.h> 里，而那个头**不是
 *    uapi**，交叉 sysroot 里没有。mmc-utils 也是自己抄一份 —— 抄的时候要对着
 *    内核源码核，写错的症状不是编译错误，是 ioctl 回 EINVAL 或者卡住。 */
#define MMC_RSP_PRESENT (1u << 0)
#define MMC_RSP_CRC     (1u << 2)
#define MMC_RSP_OPCODE  (1u << 4)
#define MMC_CMD_ADTC    (1u << 5)
#define MMC_RSP_SPI_S1  (1u << 7)
#define MMC_RSP_R1      (MMC_RSP_PRESENT | MMC_RSP_CRC | MMC_RSP_OPCODE)
#define MMC_RSP_SPI_R1  (MMC_RSP_SPI_S1)

static const char *result_str(uint16_t r)
{
	switch (r & 0x7) {
	case 0x00: return "OK —— **认证密钥已经烧过**";
	case 0x01: return "General failure";
	case 0x02: return "Authentication failure（MAC 不对）";
	case 0x03: return "Counter failure";
	case 0x04: return "Address failure";
	case 0x05: return "Write failure";
	case 0x06: return "Read failure";
	case 0x07: return "**认证密钥尚未烧写**（Authentication Key not yet programmed）";
	default:   return "未定义";
	}
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/mmcblk0rpmb";
	struct rpmb_frame req, resp;
	struct mmc_ioc_multi_cmd *mioc;
	struct mmc_ioc_cmd *cmd;
	uint8_t buf[sizeof(struct mmc_ioc_multi_cmd) + 2 * sizeof(struct mmc_ioc_cmd)];
	int fd, rc;
	uint16_t result, rr;
	uint32_t cnt;

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "打不开 %s：%s\n", dev, strerror(errno));
		return 2;
	}

	memset(&req, 0, sizeof req);
	memset(&resp, 0, sizeof resp);
	req.req_resp[0] = (uint8_t)(RPMB_REQ_READ_CNT >> 8);
	req.req_resp[1] = (uint8_t)(RPMB_REQ_READ_CNT & 0xff);

	memset(buf, 0, sizeof buf);
	mioc = (struct mmc_ioc_multi_cmd *)buf;
	mioc->num_of_cmds = 2;

	/* ① 把请求帧写进 RPMB 分区。这是 RPMB 的"提问"，不写任何用户数据。 */
	cmd = &mioc->cmds[0];
	cmd->write_flag = 1;
	cmd->opcode     = MMC_WRITE_MULTIPLE_BLOCK;
	cmd->blksz      = 512;
	cmd->blocks     = 1;
	cmd->flags      = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_ADTC;
	mmc_ioc_cmd_set_data((*cmd), &req);

	/* ② 把响应帧读回来 */
	cmd = &mioc->cmds[1];
	cmd->write_flag = 0;
	cmd->opcode     = MMC_READ_MULTIPLE_BLOCK;
	cmd->blksz      = 512;
	cmd->blocks     = 1;
	cmd->flags      = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_ADTC;
	mmc_ioc_cmd_set_data((*cmd), &resp);

	rc = ioctl(fd, MMC_IOC_MULTI_CMD, mioc);
	close(fd);
	if (rc < 0) {
		fprintf(stderr, "MMC_IOC_MULTI_CMD 失败：%s\n", strerror(errno));
		fprintf(stderr, "（EPERM 多半是没有 CAP_SYS_RAWIO；ENOTTY 说明这个内核"
		                "没开 CONFIG_MMC_BLOCK_IOCTL 那条路）\n");
		return 3;
	}

	rr     = (uint16_t)((resp.req_resp[0] << 8) | resp.req_resp[1]);
	result = (uint16_t)((resp.result[0] << 8) | resp.result[1]);
	cnt    = ((uint32_t)resp.write_counter[0] << 24) |
	         ((uint32_t)resp.write_counter[1] << 16) |
	         ((uint32_t)resp.write_counter[2] << 8)  |
	         (uint32_t)resp.write_counter[3];

	printf("设备        %s\n", dev);
	printf("req_resp    0x%04x（期望 0x%04x）\n", rr, RPMB_RESP_READ_CNT);
	printf("result      0x%04x  %s%s\n", result, result_str(result),
	       (result & 0x80) ? "（计数器已到顶）" : "");
	printf("write_counter 0x%08x（%u）\n", cnt, cnt);
	printf("\n结论：%s\n",
	       (result & 0x7) == 0x07
	       ? "RPMB 存在且**认证密钥尚未烧写** —— 技术上可以用它做防回滚锚点，"
	         "但烧写不可逆，是一次产品决策。"
	       : (result & 0x7) == 0x00
	       ? "RPMB 存在且**密钥已经烧过**。我们没有那把密钥，也读不出来 —— "
	         "在不知道密钥的前提下无法写 RPMB，这条路对本项目关闭。"
	       : "RPMB 响应异常，见上面的 result。");
	return 0;
}
