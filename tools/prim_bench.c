/* prim_bench.c —— 单次原语代价的实测（配合 tools/prim_count.py）
 *
 * 【它补的是哪一块】
 * doc/profiling_report.md 记过一次失败：本机符号级热点归因做不了
 * （liboqs 0.16 在 arm64 上是手写汇编，没有帧指针，采样穿不过去）。
 * 于是 amdahl.py 一直只能用路线图的文献占比。
 *
 * 绕过去的办法是把占比拆成两个能分别拿到的因子：
 *     占比 = 调用次数 × 单次代价 / 总耗时
 * 次数由 tools/prim_count.py 从 FIPS 203 参考实现里**精确数出来**；
 * 单次代价就是本文件量的。
 *
 * 【怎么量单次 Keccak 置换 —— 用差分，不是除法】
 * 直接 `time(SHAKE(1 块)) ` 里混着 EVP 的建栈/初始化/收尾开销，除出来的数
 * 偏大且不稳定。这里改成量两个长度（N 块与 2N 块）再相减：
 *     t_perm = (t(2N) - t(N)) / N
 * 固定开销在相减时抵掉，剩下的就是纯粹多做 N 次置换的代价。
 *
 * 【为什么用 OpenSSL 的 Keccak 而不是我们自己的】
 * 目标是估 liboqs 里那部分的占比，所以要一个**优化过的**软件 Keccak 作代表。
 * OpenSSL 的接近这个量级；accel_stub 里那份是给 RTL 对拍用的直白 C 版，
 * 实测慢约 50 倍 —— 拿它当代表会把占比算爆（NTT 那一栏就是这么爆的）。
 * 两个都打出来，是为了让这个不确定度可见，而不是藏起来。
 *
 * 【这个工具最重要的一句结论】
 * 在本机（Apple M 系列）上，我们的单蝶形 NTT 核 @100MHz 比 liboqs 的软件
 * NTT **慢**。这不是 bug，是对象不对：真正的比较基准是目标平台的
 * Cortex-A53，那上面没有这些 SIMD 汇编。表里如实打出这一点，
 * 不拿"硬件一定更快"糊弄过去。
 */
#include "pqchsm/accel.h"
#include "pqchsm/pqc.h"

#include <openssl/evp.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* 吸收 nblocks 个 SHAKE128 块（rate=168）并挤出 32 字节，重复 iters 次 */
static double time_openssl_shake(size_t nblocks, int iters)
{
	size_t len = nblocks * 168;
	uint8_t *msg = calloc(1, len);
	uint8_t out[32];
	const EVP_MD *md = EVP_shake128();
	double t0 = now_s();
	for (int i = 0; i < iters; i++) {
		EVP_MD_CTX *c = EVP_MD_CTX_new();
		EVP_DigestInit_ex(c, md, NULL);
		EVP_DigestUpdate(c, msg, len);
		EVP_DigestFinalXOF(c, out, sizeof(out));
		EVP_MD_CTX_free(c);
	}
	double t = now_s() - t0;
	free(msg);
	return t / iters;
}

/* 我们自己那份（accel 软件桩里的 C 实现），走同样的差分 */
static double time_our_perm(int iters)
{
	uint8_t st[200];
	memset(st, 0, sizeof(st));
	double t0 = now_s();
	for (int i = 0; i < iters; i++) {
		accel_keccak_f1600(st, st);
	}
	return (now_s() - t0) / iters;
}

static double time_our_ntt(int iters)
{
	int16_t in[256], out[256];
	for (int i = 0; i < 256; i++) {
		in[i] = (int16_t)((i * 37 + 11) % 3329 - 1664);
	}
	double t0 = now_s();
	for (int i = 0; i < iters; i++) {
		accel_ntt(in, out, 0);
	}
	return (now_s() - t0) / iters;
}

static double time_kem(pqc_alg_t alg, int op, int iters)
{
	const pqc_alg_info_t *in = pqc_alg_info(alg);
	uint8_t *pk = malloc(in->pk_len), *sk = malloc(in->sk_len);
	uint8_t *ct = malloc(in->ct_len), *ss = malloc(in->ss_len);
	pqc_keypair(alg, pk, sk);
	if (op == 1) {
		pqc_encaps(alg, pk, ct, ss);
	}
	double t0 = now_s();
	for (int i = 0; i < iters; i++) {
		if (op == 0) {
			pqc_keypair(alg, pk, sk);
		} else if (op == 1) {
			pqc_encaps(alg, pk, ct, ss);
		} else {
			pqc_decaps(alg, sk, ct, ss);
		}
	}
	double t = (now_s() - t0) / iters;
	free(pk);
	free(sk);
	free(ct);
	free(ss);
	return t;
}

int main(void)
{
	const size_t N = 2000;
	const int IT = 200;

	/* 差分法：两个长度相减，抵掉 EVP 的固定开销 */
	double t1 = time_openssl_shake(N, IT);
	double t2 = time_openssl_shake(2 * N, IT);
	double perm_ossl = (t2 - t1) / (double)N;

	double perm_ours = time_our_perm(200000);
	double ntt_ours = time_our_ntt(200000);

	double kg = time_kem(PQC_ALG_ML_KEM_768, 0, 20000);
	double en = time_kem(PQC_ALG_ML_KEM_768, 1, 20000);
	double de = time_kem(PQC_ALG_ML_KEM_768, 2, 20000);

	printf("单次原语代价（本机实测）\n\n");

	printf("  Keccak-f[1600] 一次置换（OpenSSL，差分法）  %8.1f ns\n", perm_ossl * 1e9);
	printf("  经 accel 寄存器接口跑一次置换（软件桩）     %8.1f ns\n", perm_ours * 1e9);
	printf("  经 accel 寄存器接口跑一次 256 点 NTT        %8.1f ns\n", ntt_ours * 1e9);
	printf("      ↑ 后两个是**算法 + 整条 HAL 路径**（写 MODE/IN_LEN、搬缓冲、轮询\n");
	printf("        STATUS），而且以算法为主：那两份 C 是给 RTL 对拍用的直白实现，\n");
	printf("        没做任何优化。第二行比第一行慢 %.0f 倍就是这么来的 ——\n",
	       perm_ours / perm_ossl);
	printf("        所以下面估占比只能用 OpenSSL 那一行。\n");
	printf("\n  ML-KEM-768 keygen / encaps / decaps        %8.2f / %.2f / %.2f us\n",
	       kg * 1e6, en * 1e6, de * 1e6);

	/* 次数来自 tools/prim_count.py 的精确计数（FIPS 203 决定的常量，
	 * 那边还与手推的解析式独立对上了：1(G)+9(H)+6(PRF)+27(SampleNTT)=43）。 */
	const double N_PERM_KG = 43, N_PERM_EN = 44;
	const double N_NTT_KG = 6, N_NTT_EN = 3;

	printf("\nKeccak 在 ML-KEM-768 里的软件占比（次数 x 单次代价 / 总耗时）\n\n");
	printf("  %-10s%12s%14s%16s\n", "操作", "置换次数", "Keccak 耗时", "占总耗时");
	struct { const char *n; double t, np; } R[] = {
		{ "KeyGen", kg, N_PERM_KG },
		{ "Encaps", en, N_PERM_EN },
	};
	for (size_t i = 0; i < sizeof(R) / sizeof(R[0]); i++) {
		double tk = R[i].np * perm_ossl;
		printf("  %-10s%12.0f%11.2f us%15.1f%%\n",
		       R[i].n, R[i].np, tk * 1e6, tk / R[i].t * 100);
	}
	printf("\n  这是个**下界**：拿 OpenSSL 的 Keccak 当代表，若 liboqs 自己那份更慢，\n");
	printf("  真实占比只会更高。路线图文献值是 ~55%%，量级上是一致的\n");
	printf("  （文献针对的是没有 SIMD 汇编的嵌入式核，本机 arm64 上占比自然更低）。\n");

	printf("\nNTT 的软件占比：**本机量不出可信值，如实记为不可用**\n\n");
	printf("  手上没有优化过的独立 NTT 实现可做代表 —— 只有本项目的直白 C 版，\n");
	printf("  它经 HAL 一次要 %.1f us，代进去算 KeyGen 的 NTT 占比会超过 100%%。\n",
	       ntt_ours * 1e6);
	printf("  这个荒谬结果本身就是结论：**这条路对 NTT 不成立**，不硬凑一个数。\n");
	printf("  能确定的只有次数：KeyGen 6 次 NTT + 9 次 basemul，Encaps 3 + 9。\n");

	printf("\n硬件侧：一次操作要多少 cycle（RTL 仿真实测，精确值）\n\n");
	{
		const accel_transport_t *v = accel_transport_verilator();
		if (!v) {
			printf("  Verilator 后端没编进来 —— 跳过（装 verilator 后重跑）。\n");
		} else {
			accel_set_transport(v);
			uint8_t st[200], so[200];
			memset(st, 0, sizeof(st));
			int16_t ni[256], no[256];
			for (int i = 0; i < 256; i++) {
				ni[i] = (int16_t)((i * 37 + 11) % 3329 - 1664);
			}
			if (accel_keccak_f1600(st, so) != 0 || accel_ntt(ni, no, 0) != 0) {
				printf("  RTL 跑失败。\n");
			} else {
				uint64_t kc = accel_verilator_keccak_cycles();
				uint64_t nc = accel_verilator_last_cycles();
				printf("  keccak_f1600 一次置换   %4llu cycle\n",
				       (unsigned long long)kc);
				printf("  ntt_core     一次变换   %4llu cycle\n",
				       (unsigned long long)nc);
				double tot_kg = N_PERM_KG * (double)kc + N_NTT_KG * (double)nc;
				double tot_en = N_PERM_EN * (double)kc + N_NTT_EN * (double)nc;
				printf("\n  代进上面的次数（**只算这两个核的部分**）：\n");
				printf("    KeyGen  %.0f x %llu + %.0f x %llu = %8.0f cycle"
				       "  = %6.2f us @100MHz\n",
				       N_PERM_KG, (unsigned long long)kc, N_NTT_KG,
				       (unsigned long long)nc, tot_kg, tot_kg / 100.0);
				printf("    Encaps  %.0f x %llu + %.0f x %llu = %8.0f cycle"
				       "  = %6.2f us @100MHz\n",
				       N_PERM_EN, (unsigned long long)kc, N_NTT_EN,
				       (unsigned long long)nc, tot_en, tot_en / 100.0);
				printf("\n  ★ 直说：%.1f us 比 liboqs 整个 KeyGen 的 %.2f us"
				       " 还慢。\n", tot_kg / 100.0, kg * 1e6);
				printf("     不回避这一点。原因是比较对象不对 —— 本机是 Apple M 系列，\n");
				printf("     liboqs 在它上面跑手写 NEON 汇编；而这两个核的设计目标是\n");
				printf("     Cortex-A53 @ ~650MHz、无 SIMD、liboqs 走通用 C 路径。\n");
				printf("     结论不是\"硬件没用\"，而是\"本机不是能下结论的平台\"：\n");
				printf("     真正的加速比要等板子上跑同一套 tools/bench 才算数。\n");
				printf("\n  能从这张表确定的事有两件：\n");
				printf("   · Keccak 核 24 cycle/置换，KeyGen 里 43 次一共才"
				       " %.0f cycle，\n", N_PERM_KG * (double)kc);
				printf("     相比 NTT 的 %.0f cycle 微不足道 ——"
				       " **24 轮展开毫无必要**，\n", N_NTT_KG * (double)nc);
				printf("     当初选单轮迭代是对的。\n");
				printf("   · 瓶颈在 NTT 核：单蝶形 %llu cycle/次。要提速就是加蝶形\n",
				       (unsigned long long)nc);
				printf("     单元与 BRAM bank —— tools/cycle_budget.py 正是算这个的。\n");
			}
			accel_set_transport(accel_transport_stub());
		}
	}

	printf("\n结论怎么用：Keccak 的占比有下界了，NTT 的没有。\n");
	printf("tools/amdahl.py --keccak <上表实测> 可覆盖文献值重算切分决策；\n");
	printf("NTT 那一项在拿到目标平台（Cortex-A53）之前继续用文献值。\n");
	return 0;
}
