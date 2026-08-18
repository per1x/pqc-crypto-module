/* rpmb_verify —— 只读确证：这把密钥是不是设备里烧着的那把
 *
 * 只发一条 RPMB「读写计数器」请求，用给定密钥校验响应的 HMAC 与 nonce 回显。
 * **不写数据、不推进计数器、不改变任何持久状态**（JESD84-B51 §6.6.22）。
 *
 * 密钥从 **stdin** 读（64 个十六进制字符），不走命令行 —— 命令行会进 ps 输出。
 * 也不落任何文件：这次排查的前提是 SD 上那个已释放的块不能被覆盖。
 *
 *   echo -n <64hex> | rpmb_verify [/dev/mmcblk0rpmb]
 */
#include "rpmb.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : RPMB_DEV_PATH;
	char hex[80];
	uint8_t key[RPMB_KEY_LEN];
	uint32_t cnt = 0;
	uint16_t res = 0;
	int i, rc;

	if (!fgets(hex, sizeof hex, stdin)) {
		printf("stdin 上没读到密钥\n");
		return 2;
	}
	for (i = 0; i < RPMB_KEY_LEN; i++) {
		unsigned v;

		if (sscanf(hex + i * 2, "%2x", &v) != 1) {
			printf("不是 64 个十六进制字符\n");
			return 2;
		}
		key[i] = (uint8_t)v;
	}

	printf("设备 %s —— 只发 Read Write Counter（不写数据、不推进计数器）\n", dev);
	rc = rpmb_read_counter(dev, key, &cnt, &res);
	printf("返回码 %d，result=0x%04x（%s），write_counter=%u\n",
	       rc, res, rpmb_result_str(res), cnt);

	if (rc == 0) {
		printf("\n✅ **MAC 与 nonce 都校验通过 —— 这就是设备里烧着的那把密钥。**\n");
		return 0;
	}
	if (rc == -4) {
		printf("\n❌ MAC 对不上 —— 不是这把。\n");
	} else if (rc == -5) {
		printf("\n❌ nonce 回显不对 —— 拿到的是陈帧，重试仍未拿到本次响应。\n");
	} else if (rc == -6) {
		printf("\n❌ 一直拿不到 req_resp=0x0200 的响应帧（陈帧问题）。\n");
	} else {
		printf("\n❌ 失败（见上面的返回码/result）。\n");
	}
	return 1;
}
