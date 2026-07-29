/* bench —— 算法级性能基线与热点采样靶子（路线图 §5.2）
 *
 * 两个用途：
 *   1. 直接跑：给出每个算法每个操作的延迟与吞吐，作为硬件加速的**对比基线**；
 *   2. 用 --loop 跑：进入一个只做单一操作的长循环，供 macOS 的 sample(1)
 *      做统计式采样，得到符号级热点归因（tools/profile.sh 就是干这个的）。
 *
 * 为什么要有基线：§5.2 的目的不是"知道 SHAKE 慢"，而是拿到**自己平台上的
 * 定量数字**，据此用 Amdahl 定律算出各种硬件切分方案的加速比上界。
 * 没有基线，后面上了板子也说不出"快了多少"。
 */
#include "pqchsm/pqc.h"

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

typedef struct {
	pqc_alg_t alg;
	const char *op;
} target_t;

/* 跑一个操作 n 次，返回总耗时 */
static double run_op(pqc_alg_t alg, const char *op, int n)
{
	const pqc_alg_info_t *i = pqc_alg_info(alg);
	uint8_t *pk = malloc(i->pk_len), *sk = malloc(i->sk_len);
	uint8_t *ct = malloc(i->ct_len ? i->ct_len : 1);
	uint8_t *sig = malloc(i->sig_len ? i->sig_len : 1);
	uint8_t ss[64], seed[64];
	const uint8_t msg[] = "benchmark message for pqc-hsm";
	double t0, t1;

	if (!pk || !sk || !ct || !sig) {
		exit(1);
	}
	memset(seed, 0x42, sizeof(seed));
	/* 先准备好前置状态，不计入计时 */
	if (i->kind == PQC_KIND_KEM) {
		pqc_keypair(alg, pk, sk);
		pqc_encaps(alg, pk, ct, ss);
	} else {
		size_t sl = i->sig_len;
		pqc_keypair(alg, pk, sk);
		pqc_sign(alg, sk, msg, sizeof(msg), NULL, 0, NULL, sig, &sl);
	}

	t0 = now_s();
	for (int k = 0; k < n; k++) {
		size_t sl = i->sig_len;
		if (!strcmp(op, "keygen")) {
			pqc_keypair(alg, pk, sk);
		} else if (!strcmp(op, "keygen_seed")) {
			pqc_keypair_from_seed(alg, seed, i->seed_len, pk, sk);
		} else if (!strcmp(op, "encaps")) {
			pqc_encaps(alg, pk, ct, ss);
		} else if (!strcmp(op, "decaps")) {
			pqc_decaps(alg, sk, ct, ss);
		} else if (!strcmp(op, "sign")) {
			pqc_sign(alg, sk, msg, sizeof(msg), NULL, 0, NULL, sig, &sl);
		} else if (!strcmp(op, "verify")) {
			pqc_verify(alg, pk, msg, sizeof(msg), NULL, 0, sig, i->sig_len);
		}
	}
	t1 = now_s();

	free(pk);
	free(sk);
	free(ct);
	free(sig);
	return t1 - t0;
}

static void bench_all(void)
{
	static const target_t T[] = {
		{ PQC_ALG_ML_KEM_512,  "keygen" }, { PQC_ALG_ML_KEM_512,  "encaps" },
		{ PQC_ALG_ML_KEM_512,  "decaps" },
		{ PQC_ALG_ML_KEM_768,  "keygen" }, { PQC_ALG_ML_KEM_768,  "keygen_seed" },
		{ PQC_ALG_ML_KEM_768,  "encaps" }, { PQC_ALG_ML_KEM_768,  "decaps" },
		{ PQC_ALG_ML_KEM_1024, "keygen" }, { PQC_ALG_ML_KEM_1024, "encaps" },
		{ PQC_ALG_ML_KEM_1024, "decaps" },
		{ PQC_ALG_ML_DSA_44,   "keygen" }, { PQC_ALG_ML_DSA_44,   "sign" },
		{ PQC_ALG_ML_DSA_44,   "verify" },
		{ PQC_ALG_ML_DSA_65,   "keygen" }, { PQC_ALG_ML_DSA_65,   "keygen_seed" },
		{ PQC_ALG_ML_DSA_65,   "sign" },   { PQC_ALG_ML_DSA_65,   "verify" },
		{ PQC_ALG_ML_DSA_87,   "keygen" }, { PQC_ALG_ML_DSA_87,   "sign" },
		{ PQC_ALG_ML_DSA_87,   "verify" },
	};
	printf("%-14s %-12s %10s %12s\n", "算法", "操作", "µs/次", "次/秒");
	printf("%-14s %-12s %10s %12s\n", "----", "----", "-----", "-----");
	for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
		const pqc_alg_info_t *info = pqc_alg_info(T[i].alg);
		/* 先粗测一次决定迭代次数，避免快操作测不准、慢操作跑太久 */
		double probe = run_op(T[i].alg, T[i].op, 20);
		int n = (int)(0.35 / (probe / 20.0));
		if (n < 50) {
			n = 50;
		}
		if (n > 20000) {
			n = 20000;
		}
		double t = run_op(T[i].alg, T[i].op, n);
		printf("%-14s %-12s %10.1f %12.0f\n", info->name, T[i].op,
		       t / n * 1e6, n / t);
	}
}

int main(int argc, char **argv)
{
	if (argc >= 3 && !strcmp(argv[1], "--loop")) {
		/* 采样靶子：持续做同一个操作，直到被杀。
		 * 用法：bench --loop <alg> <op> */
		pqc_alg_t a = pqc_alg_by_name(argv[2]);
		const char *op = argc >= 4 ? argv[3] : "keygen";
		if (a == PQC_ALG_NONE) {
			fprintf(stderr, "未知算法 %s\n", argv[2]);
			return 2;
		}
		for (;;) {
			run_op(a, op, 200);
		}
	}
	printf("pqc-hsm 算法基线（后端 %s）\n\n", pqc_get_backend()->name);
	bench_all();
	printf("\n注：这是**纯软件**基线。硬件加速后的对比要用同一张表。\n");
	return 0;
}
