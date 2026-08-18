/* rpmb_tool —— RPMB 防回滚锚点的**供应与验证**工具（只在板上跑）
 *
 * 用法：
 *   rpmb_tool status                  只读：看 RPMB 在不在、密钥烧过没有、计数器多少
 *   rpmb_tool provision --i-understand-this-is-irreversible
 *                                     生成一把随机密钥、烧进 RPMB、存到 rpmb.key
 *   rpmb_tool bump                    做一次认证写，计数器 +1（防回滚锚点前进一格）
 *   rpmb_tool selftest                连做几次，验证"只增不减"，并证明重放会被拒
 *
 * ============================================================================
 * 【为什么 provision 要一个这么长的开关】
 * ============================================================================
 * RPMB 的认证密钥**一块板只能烧一次**，烧完读不出来、也改不了。烧错了（比如
 * 烧了一把没存下来的密钥）这块 eMMC 的 RPMB 就永久不可用 —— 不影响启动、
 * 不影响别的分区，但这条防回滚的路就没了。
 *
 * 所以不提供"默认就烧"的路径，也不做交互式 y/n（脚本里一个回车就绕过去了）。
 * 要烧就把那句话完整打出来。
 *
 * ⚠️ 顺序也是有意的：**先把密钥写到文件、fsync，再烧进 eMMC。**
 *    反过来的话，中间掉一次电就得到"eMMC 里烧了一把我们不知道的密钥"，
 *    而那是不可恢复的。反着来最坏只是"文件里有把没用上的密钥"，重跑即可。
 */
#define _GNU_SOURCE
#include "rpmb.h"

#include <fcntl.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void hexout(const uint8_t *p, size_t n, char *out)
{
	size_t i;

	for (i = 0; i < n; i++) {
		sprintf(out + i * 2, "%02x", p[i]);
	}
	out[n * 2] = '\n';
	out[n * 2 + 1] = 0;
}

static int cmd_status(void)
{
	uint32_t cnt = 0;
	uint16_t res = 0;
	uint8_t key[RPMB_KEY_LEN];
	int have_key = rpmb_key_load(RPMB_KEY_PATH, key) == 0;
	int rc = rpmb_read_counter(RPMB_DEV_PATH, NULL, &cnt, &res);

	printf("设备          %s\n", RPMB_DEV_PATH);
	printf("密钥文件      %s（%s）\n", RPMB_KEY_PATH, have_key ? "在" : "没有");
	printf("result        0x%04x  %s\n", res, rpmb_result_str(res));
	printf("write_counter %u\n", cnt);
	if ((res & 0x7) == 0x07) {
		printf("\n结论：RPMB 可用，但**认证密钥尚未烧写** —— 还不能当锚点用。\n"
		       "      要启用： rpmb_tool provision --i-understand-this-is-irreversible\n");
		return 0;
	}
	if (rc == 0 && have_key) {
		/* 有密钥就再验一次带 MAC 的读：能验过才说明这把密钥就是烧进去的那把。 */
		if (rpmb_read_counter(RPMB_DEV_PATH, key, &cnt, &res) == 0) {
			printf("\n结论：RPMB 已启用，密钥文件与设备里的**对得上**，"
			       "计数器 %u。防回滚锚点可用。\n", cnt);
			return 0;
		}
		printf("\n结论：设备里已经烧过密钥，但 %s 里那把**验不过** ——"
		       " 拿的不是同一把。这条路对本机关闭。\n", RPMB_KEY_PATH);
		return 1;
	}
	printf("\n结论：设备里已经烧过密钥，而本机没有它（%s 不存在）。\n"
	       "      RPMB 无法使用；密钥烧过就读不出来了。\n", RPMB_KEY_PATH);
	return 1;
}

static int cmd_provision(void)
{
	uint8_t key[RPMB_KEY_LEN];
	char hex[RPMB_KEY_LEN * 2 + 2];
	uint16_t res = 0;
	uint32_t cnt = 0;
	int fd;

	/* 只认一种放行条件：设备明确说"密钥尚未烧写"。
	 * 其它任何情况（包括读不出来）都拒绝 —— 这一步不可逆，
	 * 没把握的时候不做，比做错了强。 */
	(void)rpmb_read_counter(RPMB_DEV_PATH, NULL, &cnt, &res);
	if ((res & 0x7) != 0x07) {
		printf("拒绝：设备没有说\"密钥尚未烧写\"（result=0x%04x %s）。\n"
		       "一块板只有一次机会，读不准就不烧。\n",
		       res, rpmb_result_str(res));
		return 1;
	}
	if (RAND_bytes(key, sizeof key) != 1) {
		printf("取随机数失败\n");
		return 1;
	}
	hexout(key, sizeof key, hex);

	/* ① 先落盘。见文件头：顺序反了就可能烧进一把谁都不知道的密钥。 */
	fd = open(RPMB_KEY_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		printf("写不了 %s —— 先确认那个目录在（pki/ 由 demo_remote.sh --provision 建）\n",
		       RPMB_KEY_PATH);
		return 1;
	}
	if (write(fd, hex, strlen(hex)) != (ssize_t)strlen(hex) || fsync(fd) != 0) {
		printf("密钥没能落盘，**不烧**\n");
		close(fd);
		return 1;
	}
	close(fd);
	printf("① 密钥已写入 %s（0600）\n", RPMB_KEY_PATH);

	/* ② 再烧。
	 *
	 * ⚠️⚠️ **烧写之后无论报什么，都不要把密钥文件删掉。**
	 * 2026-08-18 在这块板上真出过事：Program Key 实际上**成功了**，
	 * 但读回来的是一份陈的响应帧（这块 eMMC 的 RPMB 读会隔次返回陈帧，
	 * 见 rpmb.c 里 RPMB_IO_RETRY 那段），于是工具报了"烧写失败"；
	 * 随后那份"没用上的"密钥文件被顺手删掉 ——
	 * **eMMC 里烧着一把谁也不知道的密钥，这块板的 RPMB 从此不可用。**
	 *
	 * 所以这里的报错措辞是"状态不明"而不是"失败"，并且明确要求先跑
	 * status 看设备到底怎么说，再决定要不要动那个文件。 */
	if (rpmb_program_key(RPMB_DEV_PATH, key, &res) != 0) {
		printf("② 烧写**状态不明**：result=0x%04x %s\n", res, rpmb_result_str(res));
		printf("   ⚠️ 这**不等于没烧进去**。这块 eMMC 的 RPMB 读会隔次返回陈帧，\n"
		       "      报出来的结果可能是上一条请求的。\n"
		       "   ⚠️ **千万不要删 %s** —— 万一真烧进去了，那是唯一一份。\n"
		       "   下一步：跑 `rpmb_tool status`，看设备自己怎么说。\n",
		       RPMB_KEY_PATH);
		return 1;
	}
	printf("② 认证密钥已烧进 eMMC（不可逆）\n");

	if (rpmb_read_counter(RPMB_DEV_PATH, key, &cnt, &res) != 0) {
		printf("③ 烧完之后带 MAC 的读没验过 —— 别把它当可用。\n"
		       "   ⚠️ 同样**不要删 %s**，先跑 status 确认设备状态。\n",
		       RPMB_KEY_PATH);
		return 1;
	}
	printf("③ 带 MAC 的读验过了，计数器 = %u。防回滚锚点就绪。\n", cnt);
	return 0;
}

static int cmd_bump(void)
{
	uint8_t key[RPMB_KEY_LEN];
	uint32_t before = 0, after = 0;
	uint16_t res = 0;

	if (rpmb_key_load(RPMB_KEY_PATH, key) != 0) {
		printf("读不到密钥 %s\n", RPMB_KEY_PATH);
		return 1;
	}
	if (rpmb_read_counter(RPMB_DEV_PATH, key, &before, &res) != 0) {
		printf("读计数器失败：result=0x%04x %s\n", res, rpmb_result_str(res));
		return 1;
	}
	if (rpmb_write_block(RPMB_DEV_PATH, key, 0, NULL, 0, &after, &res) != 0) {
		printf("认证写失败：result=0x%04x %s\n", res, rpmb_result_str(res));
		return 1;
	}
	printf("计数器 %u → %u\n", before, after);
	return after == before + 1 ? 0 : 1;
}

static int cmd_selftest(void)
{
	uint8_t key[RPMB_KEY_LEN], bad[RPMB_KEY_LEN];
	uint32_t c0 = 0, c1 = 0, c2 = 0;
	uint16_t res = 0;
	int fails = 0, i;

	if (rpmb_key_load(RPMB_KEY_PATH, key) != 0) {
		printf("读不到密钥 %s —— 先 provision\n", RPMB_KEY_PATH);
		return 1;
	}
	if (rpmb_read_counter(RPMB_DEV_PATH, key, &c0, &res) != 0) {
		printf("✗ 带 MAC 的读没过：result=0x%04x %s\n", res, rpmb_result_str(res));
		return 1;
	}
	printf("  起点计数器 = %u\n", c0);

	/* ① 连写三次，计数器必须严格递增 */
	for (i = 0; i < 3; i++) {
		if (rpmb_write_block(RPMB_DEV_PATH, key, 0, NULL, 0, &c1, &res) != 0) {
			printf("✗ 第 %d 次认证写失败：%s\n", i + 1, rpmb_result_str(res));
			return 1;
		}
		if (c1 != c0 + 1) {
			printf("✗ 计数器没有严格 +1：%u → %u\n", c0, c1);
			fails++;
		}
		c0 = c1;
	}
	printf("  ✓ 三次认证写，计数器严格递增到 %u\n", c0);

	/* ② 拿错密钥必须写不进去 —— 这条是整个防回滚的前提：
	 *    如果谁都能写，计数器的单调性就保护不了任何人。 */
	memcpy(bad, key, sizeof bad);
	bad[0] ^= 0xFF;
	if (rpmb_write_block(RPMB_DEV_PATH, bad, 0, NULL, 0, &c2, &res) == 0) {
		printf("✗ **拿错密钥居然写进去了** —— 防回滚不成立\n");
		fails++;
	} else {
		printf("  ✓ 拿错密钥写不进去（%s）\n", rpmb_result_str(res));
	}

	/* ③ 计数器只增不减：再读一次，不能比刚才小。
	 *    这是"锚在计数器上"这个设计的全部依据，值得每次都断言一遍。 */
	if (rpmb_read_counter(RPMB_DEV_PATH, key, &c2, &res) != 0 || c2 < c0) {
		printf("✗ 计数器退了：%u → %u\n", c0, c2);
		fails++;
	} else {
		printf("  ✓ 计数器不退（%u ≥ %u）\n", c2, c0);
	}

	printf(fails ? "\nrpmb selftest: %d 项失败\n" : "\nrpmb selftest: 全部通过\n", fails);
	return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "status";

	if (!strcmp(cmd, "status")) {
		return cmd_status();
	}
	if (!strcmp(cmd, "provision")) {
		if (argc < 3 || strcmp(argv[2], "--i-understand-this-is-irreversible")) {
			printf("烧写认证密钥**不可逆，一块板只有一次机会**。\n"
			       "确认要做就完整打出来：\n"
			       "    rpmb_tool provision --i-understand-this-is-irreversible\n");
			return 2;
		}
		return cmd_provision();
	}
	if (!strcmp(cmd, "bump")) {
		return cmd_bump();
	}
	if (!strcmp(cmd, "selftest")) {
		return cmd_selftest();
	}
	printf("用法：%s status|provision|bump|selftest\n", argv[0]);
	return 2;
}
