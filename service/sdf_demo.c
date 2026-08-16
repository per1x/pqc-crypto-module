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

	/* ---- 7. 工作模式：CBC / CTR / CFB / OFB，对标准向量 ----
	 *
	 * 判据是 **NIST SP 800-38A 的官方向量**（AES-128 的四种模式都在附录 F 里）
	 * 与同一套输入下 SM4 的对照值。四种模式全部走同一个硬件分组核，
	 * 密钥装进 key_vault 之后一个字节都没有回到应用侧。
	 *
	 * ⚠️ 说准硬件做了什么：**分组变换在硬件里，链接与异或在 daemon 里**。
	 *    RTL 里没有模式状态机。把这说成"硬件 CBC"就是 overclaim。
	 */
	{
		static const unsigned char PT[64] = {
			0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a,
			0xae,0x2d,0x8a,0x57,0x1e,0x03,0xac,0x9c,0x9e,0xb7,0x6f,0xac,0x45,0xaf,0x8e,0x51,
			0x30,0xc8,0x1c,0x46,0xa3,0x5c,0xe4,0x11,0xe5,0xfb,0xc1,0x19,0x1a,0x0a,0x52,0xef,
			0xf6,0x9f,0x24,0x45,0xdf,0x4f,0x9b,0x17,0xad,0x2b,0x41,0x7b,0xe6,0x6c,0x37,0x10 };
		static const unsigned char IV0[16] = {
			0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
			0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f };
		static const unsigned char IVC[16] = {
			0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,
			0xf8,0xf9,0xfa,0xfb,0xfc,0xfd,0xfe,0xff };
		static const unsigned char AESK[16] = {
			0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
			0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c };
		static const unsigned char SM4K[16] = {
			0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
			0xfe,0xdc,0xba,0x98,0x76,0x54,0x32,0x10 };
		/* 期望值：AES 的四条是 SP 800-38A 附录 F 的官方向量 */
		static const char *const EXP[8] = {
	"7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b273bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7",
	"874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee",
	"3b3fd92eb72dad20333449f8e83cfb4ac8a64537a0b3a93fcde3cdad9f1ce58b26751f67a3cbb140b1808cf187a4f4dfc04b05357c5d1c0eeac4c66f9ff7f2e6",
	"3b3fd92eb72dad20333449f8e83cfb4a7789508d16918f03f53c52dac54ed8259740051e9c5fecf64344f7a82260edcc304c6528f659c77866a510d9c1d6ae5e",
	"784626c834ab18614677eb2074f2c5575146022d81cd18fef9bc1a1fd3a64d61102a1897c5f04a7b15e433733daf080f51284344ea0da9383f85b20ee99c3a94",
	"35e35825ac852f2b185d6b9bb4ea6f9d201ec3e66740adc7c540716c2f5a49952911a86a7841287429b6412dd677e359a2cf6977ee5c7a440920bb4826dc10f9",
	"6d59228313e6f73bc3b08993923bee401543be4d922e2c5e72e518de66199f9062841492941a99e8b2cd5497e396f71067f7cff4046b57037e3a3c1eabf798d5",
	"6d59228313e6f73bc3b08993923bee405dc2c81ba980f6e1ffe88338988c66716b8f840e2c55e339d515c53f3eba0c0dc18d6c80a7a6f02c56df4bf12452cc3f" };
		static const struct { uint32_t alg, mode; const char *name; int ctr_iv; }
		CASES[8] = {
			{ SDFE_ALG_AES128, SDFE_MODE_CBC, "AES-128-CBC", 0 },
			{ SDFE_ALG_AES128, SDFE_MODE_CTR, "AES-128-CTR", 1 },
			{ SDFE_ALG_AES128, SDFE_MODE_CFB, "AES-128-CFB", 0 },
			{ SDFE_ALG_AES128, SDFE_MODE_OFB, "AES-128-OFB", 0 },
			{ SDFE_ALG_SM4,    SDFE_MODE_CBC, "SM4-CBC",     0 },
			{ SDFE_ALG_SM4,    SDFE_MODE_CTR, "SM4-CTR",     1 },
			{ SDFE_ALG_SM4,    SDFE_MODE_CFB, "SM4-CFB",     0 },
			{ SDFE_ALG_SM4,    SDFE_MODE_OFB, "SM4-OFB",     0 } };
		static unsigned char ct[64], back[64];
		char got[129];
		int i, k;

		printf("\n[7] SDFE_EncryptMode/DecryptMode（CBC/CTR/CFB/OFB，密钥留在硬件）\n");
		/* 两把密钥各占一个槽；装进去之后应用侧抹掉 */
		{
			unsigned char tmp[16];

			memcpy(tmp, AESK, 16);
			if ((rv = SDFE_ImportKey(ses, 4, tmp, 16)) != SDR_OK) {
				printf("  装 AES 密钥失败：%s\n", SDFE_StrError(rv)); return 1;
			}
			memcpy(tmp, SM4K, 16);
			if ((rv = SDFE_ImportKey(ses, 5, tmp, 16)) != SDR_OK) {
				printf("  装 SM4 密钥失败：%s\n", SDFE_StrError(rv)); return 1;
			}
			memset(tmp, 0, sizeof tmp);
		}

		for (i = 0; i < 8; i++) {
			uint32_t slot = (CASES[i].alg == SDFE_ALG_SM4) ? 5u : 4u;
			const unsigned char *iv = CASES[i].ctr_iv ? IVC : IV0;

			rv = SDFE_EncryptMode(ses, CASES[i].alg, CASES[i].mode, slot,
			                      iv, PT, sizeof PT, ct);
			if (rv != SDR_OK) {
				printf("  %s 加密失败：%s\n", CASES[i].name,
				       SDFE_StrError(rv)); return 1;
			}
			for (k = 0; k < 64; k++)
				sprintf(got + 2 * k, "%02x", ct[k]);
			if (strcmp(got, EXP[i])) {
				printf("  ❌ %s 与标准向量不一致\n     算出 %s\n     应当 %s\n",
				       CASES[i].name, got, EXP[i]);
				return 1;
			}
			rv = SDFE_DecryptMode(ses, CASES[i].alg, CASES[i].mode, slot,
			                      iv, ct, sizeof PT, back);
			if (rv != SDR_OK || memcmp(back, PT, sizeof PT)) {
				printf("  ❌ %s 解密回不到原文（%s）\n", CASES[i].name,
				       SDFE_StrError(rv));
				return 1;
			}
			printf("  ✅ %-12s 64 字节对上标准向量，解密回到原文\n",
			       CASES[i].name);
		}
		memset(ct, 0, sizeof ct);
		memset(back, 0, sizeof back);
	}

	/* ---- 8. 反证：句柄是**会话内**的，断开就作废 ----
	 * 上面第 [2] 步拿到的句柄 kh 在本连接内一直可用。现在断开重连，
	 * 同一个句柄必须**不再可用** —— 否则任何人连上来都能使唤上一个人的私钥。
	 * daemon 那边断连时清句柄表并给两个 PQC 金库各发一次 ZEROIZE，
	 * 所以私钥既不在进程里、也不在硬件里。 */
	printf("\n[8] 反证：断开重连之后，上一会话的句柄必须失效\n");
	SDFE_CloseSession(ses);
	SDFE_CloseDevice(dev);
	if (argc >= 3)
		rv = SDFE_OpenDeviceRemote(&dev, argv[1],
		                           argc >= 4 ? atoi(argv[3]) : 0, argv[2]);
	else
		rv = SDFE_OpenDevice(&dev);
	if (rv != SDR_OK) { printf("  重连失败：%s\n", SDFE_StrError(rv)); return 1; }
	if ((rv = SDFE_OpenSession(dev, &ses)) != SDR_OK) {
		printf("  开会话失败：%s\n", SDFE_StrError(rv)); return 1;
	}
	{
		unsigned char ss3[32];
		uint32_t ss3_len = sizeof ss3;

		rv = SDFE_Decapsulate_MLKEM(ses, kh, ct, ct_len, ss3, &ss3_len);
		if (rv == SDR_OK) {
			printf("  ❌ 旧句柄 %u 在新连接里**仍然可用** —— 会话隔离没生效\n", kh);
			return 1;
		}
		printf("  ✅ 旧句柄 %u 已失效（%s）\n", kh, SDFE_StrError(rv));
		memset(ss3, 0, sizeof ss3);
	}

	SDFE_CloseSession(ses);
	SDFE_CloseDevice(dev);
	printf("\n=== 全部完成。本程序没有碰过任何寄存器。 ===\n");
	return 0;
}
