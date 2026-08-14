// sdf_demo —— 一个**独立的应用程序**，像调用真正的密码机一样调用它
//
//   编译：cc -o sdf_demo sdf_demo.c -L. -lsdfe
//   本机运行：./sdf_demo
//   远程运行：./sdf_demo <板子IP> <口令> [端口]
//
// **本机和远程走的是同一段代码**：只有开设备那一行不同，从第 [1] 步开始
// 一个字都不变。这不是省事，这是要展示的性质本身 ——
// 调用方不需要知道密码机在本机还是在网络另一头。
//
// 这个文件里**没有任何硬件细节**：没有寄存器、没有 /dev/mem、没有 SMC。
// 它只认 sdfe.h 里那十来个函数。请求经
//   本程序 → libsdfe → pqchsm_fpgad → /dev/secmmio → EL3 SiP → FPGA 密码核
// 每一步都在另一侧，本程序一概不知道。
//
// 这正是"能像密码机一样被调用"的意思：换一块密码机、换一套硬件，
// 只要还实现这套接口，这个文件一个字都不用改。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdfe.h"

static void hex(const char *tag, const unsigned char *p, unsigned n)
{
	unsigned i;

	printf("  %-22s ", tag);
	for (i = 0; i < n && i < 16; i++)
		printf("%02x", p[i]);
	if (n > 16)
		printf("…（共 %u 字节）", n);
	printf("\n");
}

int main(int argc, char **argv)
{
	SDFE_HANDLE dev, ses;
	unsigned char rnd[32], ek[1600], ct[1600], ss1[32], ss2[32];
	unsigned char key[16], pt[16], enc[16], dec[16];
	unsigned int ek_len = sizeof ek, ct_len = sizeof ct;
	unsigned int ss1_len = sizeof ss1, ss2_len = sizeof ss2;
	unsigned int kh = 0;
	char info[160];
	int rv;

	printf("=== 应用程序：通过 SDF 风格接口使用密码机 ===\n\n");

	if (argc >= 3) {
		int port = argc >= 4 ? atoi(argv[3]) : 0;

		printf("[连接] 远程 %s:%d\n", argv[1], port ? port : 9797);
		rv = SDFE_OpenDeviceRemote(&dev, argv[1], port, argv[2]);
	} else {
		printf("[连接] 本机\n");
		rv = SDFE_OpenDevice(&dev);
	}
	if (rv != SDR_OK) {
		printf("打开设备失败：%s\n", SDFE_StrError(rv)); return 1;
	}
	if ((rv = SDFE_OpenSession(dev, &ses)) != SDR_OK) {
		printf("开会话失败：%s\n", SDFE_StrError(rv)); return 1;
	}

	SDFE_GetDeviceInfo(ses, info, sizeof info);
	printf("[设备] %s\n\n", info);

	/* ---- 1. 硬件随机数 ---- */
	printf("[1] SDFE_GenerateRandom —— 取自 PL 里的环振噪声源\n");
	if ((rv = SDFE_GenerateRandom(ses, 32, rnd)) != SDR_OK) {
		printf("  失败：%s\n", SDFE_StrError(rv)); return 1;
	}
	hex("32 字节随机数", rnd, 32);

	/* ---- 2. ML-KEM：生成密钥对，私钥只以句柄示人 ---- */
	printf("\n[2] SDFE_GenerateKeyPair_MLKEM(ML-KEM-768)\n");
	rv = SDFE_GenerateKeyPair_MLKEM(ses, SDFE_MLKEM_768, ek, &ek_len, &kh);
	if (rv != SDR_OK) { printf("  失败：%s\n", SDFE_StrError(rv)); return 1; }
	printf("  公钥 ek %u 字节，私钥句柄 = %u\n", ek_len, kh);
	hex("ek 开头", ek, ek_len);
	printf("  ← 注意：本程序**从头到尾没有拿到过私钥 dk**，只有这个句柄\n");

	/* ---- 3. 封装 / 解封装 ---- */
	printf("\n[3] SDFE_Encapsulate_MLKEM（用公钥）\n");
	rv = SDFE_Encapsulate_MLKEM(ses, SDFE_MLKEM_768, ek, ek_len,
				    ss1, &ss1_len, ct, &ct_len);
	if (rv != SDR_OK) { printf("  失败：%s\n", SDFE_StrError(rv)); return 1; }
	hex("共享密钥 K", ss1, ss1_len);
	hex("密文 c", ct, ct_len);

	printf("\n[4] SDFE_Decapsulate_MLKEM（用句柄，不是私钥）\n");
	rv = SDFE_Decapsulate_MLKEM(ses, kh, ct, ct_len, ss2, &ss2_len);
	if (rv != SDR_OK) { printf("  失败：%s\n", SDFE_StrError(rv)); return 1; }
	hex("解出的 K", ss2, ss2_len);

	if (ss1_len == ss2_len && !memcmp(ss1, ss2, ss1_len))
		printf("  ✅ 两边的共享密钥一致 —— 一次完整的 KEM 在 FPGA 上跑通\n");
	else {
		printf("  ❌ 共享密钥不一致\n"); return 1;
	}

	/* ---- 5. 对称：密钥进 key_vault，之后只按槽号使唤 ---- */
	printf("\n[5] SDFE_ImportKey + SDFE_Encrypt/Decrypt（SM4，密钥留在硬件）\n");
	memcpy(key, "\x01\x23\x45\x67\x89\xab\xcd\xef\xfe\xdc\xba\x98\x76\x54\x32\x10", 16);
	memcpy(pt,  "\x01\x23\x45\x67\x89\xab\xcd\xef\xfe\xdc\xba\x98\x76\x54\x32\x10", 16);
	if ((rv = SDFE_ImportKey(ses, 3, key, 16)) != SDR_OK) {
		printf("  装载密钥失败：%s\n", SDFE_StrError(rv)); return 1;
	}
	memset(key, 0, sizeof key);   /* 应用这边也抹掉：硬件里那份才算数 */
	printf("  密钥已进 key_vault 槽 3，应用侧已抹除\n");

	if ((rv = SDFE_Encrypt(ses, SDFE_ALG_SM4, 3, pt, enc)) != SDR_OK) {
		printf("  加密失败：%s\n", SDFE_StrError(rv)); return 1;
	}
	hex("SM4 密文", enc, 16);
	/* GB/T 32907 A.1 的标准答案 */
	{
		static const unsigned char want[16] = {
			0x68,0x1e,0xdf,0x34,0xd2,0x06,0x96,0x5e,
			0x86,0xb3,0xe9,0x4f,0x53,0x6e,0x42,0x46 };
		printf("  %s 与 GB/T 32907 A.1 %s\n",
		       memcmp(enc, want, 16) ? "❌" : "✅",
		       memcmp(enc, want, 16) ? "不一致" : "逐字节一致");
	}
	if ((rv = SDFE_Decrypt(ses, SDFE_ALG_SM4, 3, enc, dec)) != SDR_OK) {
		printf("  解密失败：%s\n", SDFE_StrError(rv)); return 1;
	}
	printf("  %s 解密回到原文\n", memcmp(dec, pt, 16) ? "❌" : "✅");

	SDFE_CloseSession(ses);
	SDFE_CloseDevice(dev);
	printf("\n=== 全部完成。本程序没有碰过任何寄存器。 ===\n");
	return 0;
}
