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

	/* ---- 6. ML-DSA：三个参数集各走一遍 KeyGen → Sign → Verify ----
	 *
	 * 与 ML-KEM 那段同一个口径：**应用从头到尾没有见过 sk**，KeyGen 只回
	 * pk 和一个槽号，签名按槽号使唤。
	 *
	 * ⚠️ 这一段以前不在 demo 里，而"不在 demo 里"曾经掩盖了两个真 bug：
	 *    daemon 把 mldsa 的 MODE/STATUS 写反（每次等 done 超时），以及
	 *    BL31 白名单没有槽 6（送检形态下这条路根本不可达）。两个都只有
	 *    **端到端真跑一次**才现形 —— 单元测试、KAT 程序都绕开了这条路。
	 *
	 * 判据取"验得过 + 改一个字节就验不过"两条：只验前一半的话，
	 * 一个恒返回 OK 的实现照样能过。 */
	{
		static const unsigned int PKL[3] = { 1312, 1952, 2592 };
		static const unsigned int SGL[3] = { 2420, 3309, 4627 };
		static const char *const NAME[3] = {
			"ML-DSA-44", "ML-DSA-65", "ML-DSA-87" };
		static unsigned char pk[2592], sig[4627];
		const unsigned char msg[] = "pqc-hsm end-to-end signature";
		unsigned int pset;

		printf("\n[6] SDFE_GenerateKeyPair/Sign/Verify_MLDSA"
		       "（三个参数集，sk 全程在片内）\n");
		for (pset = 0; pset < 3; pset++) {
			unsigned int pk_len = sizeof pk, sig_len = sizeof sig;
			unsigned int kh2 = 0;

			rv = SDFE_GenerateKeyPair_MLDSA(ses, pset, pk, &pk_len, &kh2);
			if (rv != SDR_OK) {
				printf("  %s KeyGen 失败：%s\n", NAME[pset],
				       SDFE_StrError(rv)); return 1;
			}
			if (pk_len != PKL[pset]) {
				printf("  ❌ %s pk 长度 %u，应当是 %u\n", NAME[pset],
				       pk_len, PKL[pset]); return 1;
			}
			rv = SDFE_Sign_MLDSA(ses, kh2, msg, (unsigned int)sizeof msg - 1,
			                     NULL, 0, sig, &sig_len);
			if (rv != SDR_OK) {
				printf("  %s Sign 失败：%s\n", NAME[pset],
				       SDFE_StrError(rv)); return 1;
			}
			if (sig_len != SGL[pset]) {
				printf("  ❌ %s σ 长度 %u，应当是 %u\n", NAME[pset],
				       sig_len, SGL[pset]); return 1;
			}
			rv = SDFE_Verify_MLDSA(ses, pset, pk, pk_len, msg,
			                       (unsigned int)sizeof msg - 1,
			                       NULL, 0, sig, sig_len);
			if (rv != SDR_OK) {
				printf("  ❌ %s 自己签的验不过：%s\n", NAME[pset],
				       SDFE_StrError(rv)); return 1;
			}
			/* 反证：动一个字节就必须验不过 */
			sig[sig_len / 2] ^= 0x01;
			rv = SDFE_Verify_MLDSA(ses, pset, pk, pk_len, msg,
			                       (unsigned int)sizeof msg - 1,
			                       NULL, 0, sig, sig_len);
			if (rv == SDR_OK) {
				printf("  ❌ %s 改了一个字节仍然验得过 —— 验签是假的\n",
				       NAME[pset]); return 1;
			}
			sig[sig_len / 2] ^= 0x01;
			printf("  ✅ %-10s 槽 %u：pk %u 字节、σ %u 字节，验得过；"
			       "改一字节即验不过\n", NAME[pset], kh2, pk_len, sig_len);
		}
	}

	SDFE_CloseSession(ses);
	SDFE_CloseDevice(dev);
	printf("\n=== 全部完成。本程序没有碰过任何寄存器。 ===\n");
	return 0;
}
