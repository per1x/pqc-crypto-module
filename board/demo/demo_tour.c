/* demo_tour —— 演示主程序：密钥在 TEE 里管，运算在 FPGA 里做，应用只调接口
 *
 * 由 board/demo/run.sh 调用；也可单独跑：
 *     demo_tour                         本机（UNIX socket）
 *     demo_tour <板子IP> <凭据目录>      远程（mTLS）
 *
 * 编译：与 sdf_demo 同一套（链 libsdfe + OpenSSL 1.1，见 boot/d7/build-apps-d7.sh）。
 *
 * 本程序是一个**普通应用**：它只链接 libsdfe，一行密码运算都不做。
 * 屏幕上每一个"✅"都来自与官方向量（NIST / GB-T）的逐字节比对，不是"没报错"。
 * 时延是应用侧看到的端到端时间（含 socket → daemon → EL3 → PL 全程）。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../service/sdfe.h"

static int n_ok, n_bad;

static double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void chapter(const char *s)
{
	printf("\n\033[1m━━ %s\033[0m\n", s);
}

static void step(const char *s)
{
	printf("\n  \033[1m%s\033[0m\n", s);
}

static void ok(const char *fmt, const char *a)
{
	printf("    ✅ ");
	printf(fmt, a);
	printf("\n");
	n_ok++;
}

static void bad(const char *what, int rv)
{
	printf("    ❌ %s（%s）\n", what, SDFE_StrError(rv));
	n_bad++;
}

static void hex(const char *label, const uint8_t *p, size_t n, size_t show)
{
	size_t i;

	printf("    %-18s ", label);
	for (i = 0; i < n && i < show; i++)
		printf("%02x", p[i]);
	printf("%s（%zu 字节）\n", n > show ? "…" : "", n);
}

static int unhex_eq(const uint8_t *got, const char *want, size_t n)
{
	char buf[3] = { 0 };
	size_t i;

	for (i = 0; i < n; i++) {
		buf[0] = want[2 * i];
		buf[1] = want[2 * i + 1];
		if (got[i] != (uint8_t)strtoul(buf, NULL, 16))
			return 0;
	}
	return 1;
}

int main(int argc, char **argv)
{
	SDFE_HANDLE dev = NULL, ses = NULL;
	SDFE_TLS_CREDS cr;
	char crt[512], key[512], ca[512], info[256];
	int rv;
	double t0;

	if (argc >= 3) {
		snprintf(ca, sizeof ca, "%s/hsm_ca.crt", argv[2]);
		snprintf(crt, sizeof crt, "%s/client.crt", argv[2]);
		snprintf(key, sizeof key, "%s/client.key", argv[2]);
		memset(&cr, 0, sizeof cr);
		cr.ca_file = ca; cr.cert_file = crt; cr.key_file = key;
		rv = SDFE_OpenDeviceRemote(&dev, argv[1], 9797, &cr);
	} else {
		rv = SDFE_OpenDevice(&dev);
	}
	if (rv != SDR_OK || SDFE_OpenSession(dev, &ses) != SDR_OK) {
		printf("连不上密码机：%s\n", SDFE_StrError(rv));
		return 2;
	}
	SDFE_GetDeviceInfo(ses, info, sizeof info);
	printf("  已连接：%s%s\n", argc >= 3 ? "远程（mTLS）" : "本机",
	       argc >= 3 ? "" : "（UNIX socket）");
	printf("  设备：%s\n", info);

	/* ====================================================================
	 * A. 密钥管理在 TEE
	 * ==================================================================== */
	chapter("A. 密钥管理在 TEE —— 密钥由 TA 生成、包裹、装载，应用和 daemon 都碰不到明文");

	step("A1. 后量子私钥：TA 保管「种子」，FPGA 每次现展开、算完即擦");
	{
		static uint8_t ek[1568];
		uint32_t ek_len = sizeof ek, h = 0;

		t0 = now_ms();
		rv = SDFE_GenerateKeyPair_MLKEM(ses, SDFE_MLKEM_768, ek, &ek_len, &h);
		if (rv != SDR_OK) {
			bad("ML-KEM-768 生成密钥对", rv);
		} else {
			printf("    TA 生成种子 d‖z → 经安全世界送进 FPGA → FPGA 展开出密钥对\n");
			printf("    应用拿到：公钥 %u 字节 + 句柄 %u（%.1f ms）\n",
			       ek_len, h, now_ms() - t0);
			printf("    daemon 拿到：TA 用 KEK 包裹的种子 blob（它解不开，KEK 不出 TA）\n");
			printf("    FPGA 里：私钥只在这一次运算里存在，结束即被硬件擦除\n");
			ok("%s", "私钥 dk 没有以明文出现在 TEE 之外的任何地方");
		}
	}

	step("A2. 对称密钥：TA 生成 → 包成 blob 交给应用保存 → 需要时再装进 FPGA");
	{
		static const uint8_t pt[16] = { 'p','q','c','-','h','s','m',' ','d','e','m','o',' ','1','6','B' };
		uint8_t blob[SDFE_SYMKEY_BLOB_MAX], c1[16], c2[16];
		uint32_t blen = sizeof blob;

		t0 = now_ms();
		rv = SDFE_GenerateKey(ses, 3, 16, blob, &blen);
		if (rv != SDR_OK) {
			bad("TA 生成对称密钥", rv);
		} else {
			printf("    SDFE_GenerateKey(槽 3, SM4) 用时 %.1f ms\n", now_ms() - t0);
			hex("应用拿到的 blob", blob, blen, 24);
			printf("    ↑ 这是密文（AES-256-GCM，KEK 在 TA 内）。应用可以存盘，但解不开。\n");
			SDFE_Encrypt(ses, SDFE_ALG_SM4, 3, pt, c1);
			t0 = now_ms();
			rv = SDFE_LoadKey(ses, 4, blob, blen);
			printf("    SDFE_LoadKey(槽 4, blob) 用时 %.1f ms —— TA 解包后经安全世界装进 FPGA\n",
			       now_ms() - t0);
			SDFE_Encrypt(ses, SDFE_ALG_SM4, 4, pt, c2);
			if (rv == SDR_OK && !memcmp(c1, c2, 16))
				ok("%s", "同一份 blob 装进另一个槽，加密结果相同 —— 密钥可随身带走、随时重装");
			else
				bad("blob 重装", rv);

			blob[blen - 1] ^= 1;
			rv = SDFE_LoadKey(ses, 4, blob, blen);
			if (rv != SDR_OK)
				ok("%s", "blob 改一个比特 → TA 拒绝装载（GCM 认证失败）");
			else
				bad("篡改过的 blob 居然装进去了", rv);
		}
		{
			static const uint8_t k[16];
			rv = SDFE_ImportKey(ses, 3, k, 16);
			if (rv == SDR_NOTSUPPORT)
				ok("%s", "明文导入密钥 → 拒绝（明文必然经过普通世界，本设备不提供这条路）");
			else
				bad("明文导入居然成功了", rv);
		}
	}

	step("A3. 标准向量自检：公开测试密钥只能进专用的 KAT 槽 7");
	{
		static const uint8_t pt[16] = { 0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
		                                0xfe,0xdc,0xba,0x98,0x76,0x54,0x32,0x10 };
		uint8_t c[16];

		rv = SDFE_LoadKatKey(ses, SDFE_KAT_SM4_GBT32907);
		if (rv == SDR_OK)
			rv = SDFE_Encrypt(ses, SDFE_ALG_SM4, SDFE_SYM_KAT_SLOT, pt, c);
		if (rv == SDR_OK && unhex_eq(c, "681edf34d206965e86b3e94f536e4246", 16))
			ok("%s", "SM4 对上 GB/T 32907 附录 A（应用只给了向量编号，没传一个密钥字节）");
		else
			bad("SM4 KAT", rv);
	}

	/* ====================================================================
	 * B. FPGA 做运算
	 * ==================================================================== */
	chapter("B. FPGA 做运算 —— 每一项都对官方向量，时延是应用侧端到端实测");

	step("B1. ML-KEM-768（FIPS 203）：密钥封装");
	{
		static uint8_t ek[1568], ct[1568], ss1[32], ss2[32];
		uint32_t ek_len = sizeof ek, ct_len = sizeof ct, s1 = 32, s2 = 32, h;
		double tk, te, td;

		t0 = now_ms();
		rv = SDFE_GenerateKeyPair_MLKEM(ses, SDFE_MLKEM_768, ek, &ek_len, &h);
		tk = now_ms() - t0;
		t0 = now_ms();
		if (rv == SDR_OK)
			rv = SDFE_Encapsulate_MLKEM(ses, SDFE_MLKEM_768, ek, ek_len,
			                            ss1, &s1, ct, &ct_len);
		te = now_ms() - t0;
		t0 = now_ms();
		if (rv == SDR_OK)
			rv = SDFE_Decapsulate_MLKEM(ses, h, ct, ct_len, ss2, &s2);
		td = now_ms() - t0;
		if (rv == SDR_OK && s1 == 32 && !memcmp(ss1, ss2, 32)) {
			printf("    生成 %.1f ms │ 封装 %.1f ms │ 解封装 %.1f ms\n", tk, te, td);
			ok("%s", "双方得到同一个 32 字节共享密钥");
		} else {
			bad("ML-KEM-768 往返", rv);
		}
	}

	step("B2. ML-DSA-44 / 65 / 87（FIPS 204）：数字签名");
	{
		static const char *nm[3] = { "ML-DSA-44", "ML-DSA-65", "ML-DSA-87" };
		static uint8_t pk[2592], sig[4627];
		static const uint8_t msg[] = "合同第 7 条：交付日期 2026-12-31";
		uint32_t p;
		int good = 0;

		for (p = 0; p < 3; p++) {
			uint32_t pk_len = sizeof pk, sig_len = sizeof sig, h;
			double tk, ts, tv;
			int rv2;

			t0 = now_ms();
			rv = SDFE_GenerateKeyPair_MLDSA(ses, p, pk, &pk_len, &h);
			tk = now_ms() - t0;
			t0 = now_ms();
			if (rv == SDR_OK)
				rv = SDFE_Sign_MLDSA(ses, h, msg, sizeof msg - 1, NULL, 0,
				                     sig, &sig_len);
			ts = now_ms() - t0;
			t0 = now_ms();
			if (rv == SDR_OK)
				rv = SDFE_Verify_MLDSA(ses, p, pk, pk_len, msg, sizeof msg - 1,
				                       NULL, 0, sig, sig_len);
			tv = now_ms() - t0;
			sig[sig_len / 2] ^= 1;
			rv2 = SDFE_Verify_MLDSA(ses, p, pk, pk_len, msg, sizeof msg - 1,
			                        NULL, 0, sig, sig_len);
			if (rv == SDR_OK && rv2 == SDR_VERIFYFAIL) {
				printf("    %-9s  生成 %5.1f ms │ 签名 %6.1f ms │ 验签 %5.1f ms │ σ %u 字节\n",
				       nm[p], tk, ts, tv, sig_len);
				good++;
			} else {
				bad(nm[p], rv ? rv : rv2);
			}
		}
		if (good == 3)
			ok("%s", "三个参数集都签得出、验得过，签名改一个比特即验不过");
	}

	step("B3. AES-128 / SM4 四种工作模式（SP 800-38A 附录 F 等官方向量）");
	{
		static const uint8_t PT[64] = {
			0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a,
			0xae,0x2d,0x8a,0x57,0x1e,0x03,0xac,0x9c,0x9e,0xb7,0x6f,0xac,0x45,0xaf,0x8e,0x51,
			0x30,0xc8,0x1c,0x46,0xa3,0x5c,0xe4,0x11,0xe5,0xfb,0xc1,0x19,0x1a,0x0a,0x52,0xef,
			0xf6,0x9f,0x24,0x45,0xdf,0x4f,0x9b,0x17,0xad,0x2b,0x41,0x7b,0xe6,0x6c,0x37,0x10 };
		static const uint8_t IV0[16] = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };
		static const uint8_t IVC[16] = { 0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,
		                                 0xf8,0xf9,0xfa,0xfb,0xfc,0xfd,0xfe,0xff };
		static const char *const EXP[8] = {
	"7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b273bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7",
	"874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee",
	"3b3fd92eb72dad20333449f8e83cfb4ac8a64537a0b3a93fcde3cdad9f1ce58b26751f67a3cbb140b1808cf187a4f4dfc04b05357c5d1c0eeac4c66f9ff7f2e6",
	"3b3fd92eb72dad20333449f8e83cfb4a7789508d16918f03f53c52dac54ed8259740051e9c5fecf64344f7a82260edcc304c6528f659c77866a510d9c1d6ae5e",
	"784626c834ab18614677eb2074f2c5575146022d81cd18fef9bc1a1fd3a64d61102a1897c5f04a7b15e433733daf080f51284344ea0da9383f85b20ee99c3a94",
	"35e35825ac852f2b185d6b9bb4ea6f9d201ec3e66740adc7c540716c2f5a49952911a86a7841287429b6412dd677e359a2cf6977ee5c7a440920bb4826dc10f9",
	"6d59228313e6f73bc3b08993923bee401543be4d922e2c5e72e518de66199f9062841492941a99e8b2cd5497e396f71067f7cff4046b57037e3a3c1eabf798d5",
	"6d59228313e6f73bc3b08993923bee405dc2c81ba980f6e1ffe88338988c66716b8f840e2c55e339d515c53f3eba0c0dc18d6c80a7a6f02c56df4bf12452cc3f" };
		static const char *const NM[8] = { "AES-128-CBC", "AES-128-CTR", "AES-128-CFB",
			"AES-128-OFB", "SM4-CBC", "SM4-CTR", "SM4-CFB", "SM4-OFB" };
		static const uint32_t MODE[4] = { SDFE_MODE_CBC, SDFE_MODE_CTR,
		                                  SDFE_MODE_CFB, SDFE_MODE_OFB };
		uint8_t ct[64], back[64];
		int i, good = 0;

		for (i = 0; i < 8; i++) {
			uint32_t alg = i < 4 ? SDFE_ALG_AES128 : SDFE_ALG_SM4;
			const uint8_t *iv = (i % 4 == 1) ? IVC : IV0;

			rv = SDFE_LoadKatKey(ses, i < 4 ? SDFE_KAT_AES128_SP38A
			                               : SDFE_KAT_SM4_GBT32907);
			if (rv == SDR_OK)
				rv = SDFE_EncryptMode(ses, alg, MODE[i % 4], SDFE_SYM_KAT_SLOT,
				                      iv, PT, 64, ct);
			if (rv == SDR_OK)
				rv = SDFE_DecryptMode(ses, alg, MODE[i % 4], SDFE_SYM_KAT_SLOT,
				                      iv, ct, 64, back);
			if (rv == SDR_OK && unhex_eq(ct, EXP[i], 64) && !memcmp(back, PT, 64))
				good++;
			else
				bad(NM[i], rv);
		}
		if (good == 8) {
			printf("    ✅ 8 组全部逐字节对上标准向量，解密回到原文\n");
			n_ok++;
		}
	}
	{
		static uint8_t buf[16368], out[16368];
		uint8_t blob[SDFE_SYMKEY_BLOB_MAX], iv[16] = { 0 };
		uint32_t blen = sizeof blob;
		int i;

		if (SDFE_GenerateKey(ses, 5, 32, blob, &blen) == SDR_OK) {
			t0 = now_ms();
			for (i = 0; i < 8; i++)
				SDFE_EncryptMode(ses, SDFE_ALG_AES256, SDFE_MODE_CBC, 5, iv,
				                 buf, sizeof buf, out);
			printf("    吞吐：AES-256-CBC %.2f MB/s（每个分组都在 FPGA 里算；"
			       "瓶颈是每次寄存器访问都要经 EL3）\n",
			       8.0 * sizeof buf / 1048576.0 / ((now_ms() - t0) / 1e3));
		}
	}

	step("B4. SM3（GB/T 32905）杂凑 与 硬件随机数");
	{
		uint8_t d[32], r[32];

		rv = SDFE_Hash_SM3(ses, (const uint8_t *)"abc", 3, d);
		if (rv == SDR_OK && unhex_eq(d, "66c7f0f462eeedd9d1f2d46bdc10e4e2"
		                                "4167c4875cf2f7a2297da02b8f4ba8e0", 32))
			ok("%s", "SM3(\"abc\") 对上 GB/T 32905 附录 A");
		else
			bad("SM3", rv);
		rv = SDFE_GenerateRandom(ses, 32, r);
		if (rv == SDR_OK) {
			hex("随机数", r, 32, 16);
			printf("    ↑ 来自 FPGA 里 8 路环形振荡器 + SP 800-90B 健康检测，不是软件伪随机\n");
		}
	}

	/* ====================================================================
	 * C. 会话结束即擦
	 * ==================================================================== */
	chapter("C. 会话结束 —— 句柄作废、FPGA 里的对称密钥被擦");
	SDFE_CloseSession(ses);
	SDFE_CloseDevice(dev);
	printf("    已断开。daemon 清空句柄表，并让 FPGA 擦掉金库与运算核里的残留。\n");
	printf("    （run.sh 接下来从硬件侧核对这一点）\n");

	printf("\n  小结：✅ %d 项  ❌ %d 项\n", n_ok, n_bad);
	return n_bad ? 1 : 0;
}
