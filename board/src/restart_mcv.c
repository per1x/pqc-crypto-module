// restart_mcv —— 在板上直接算重启矩阵的行/列最小熵（MCV，SP 800-90B §6.3.1）
//
//   restart_mcv <矩阵文件> [H_原始]
//
// ============================================================================
// 【为什么要有一个板上版本】
// ============================================================================
// 分析本来在 Mac 上用 tools/restart_test.py 做。但这块板的网络在传 125 KB 的
// 矩阵时反复截断（32768 / 65536 这种整齐的边界，试了四五次都没传全），
// 而板上没有 python。与其继续和传输搏斗，不如把尺子搬到数据那一侧。
//
// ============================================================================
// 【它必须和 Python 那版算出同一个数 —— 否则两把尺子都不可信】
// ============================================================================
// 用法上先拿**已经有 Python 结果**的那个矩阵复算一遍（restart.bin：
// 行最小 0.728758、列最小 0.000000）。对上了，这版才有资格去算新数据。
// 两个独立实现在同一份数据上一致，比任何一个单独说自己对都强。
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZC 2.5758293035489008   /* 与 tools/sp800_90b.py 的 99% 单边一致 */

static double mcv_of(const unsigned char *bits, long n)
{
	long ones = 0, i;
	double ph, pu;

	for (i = 0; i < n; i++)
		ones += bits[i];
	ph = (double)(ones > n - ones ? ones : n - ones) / (double)n;
	pu = ph + ZC * sqrt(ph * (1.0 - ph) / (double)(n - 1));
	if (pu > 1.0)
		pu = 1.0;
	return -log2(pu);
}

int main(int argc, char **argv)
{
	const char *path = (argc > 1) ? argv[1] : "restart.bin";
	double h_orig = (argc > 2) ? atof(argv[2]) : -1.0;
	FILE *f = fopen(path, "rb");
	long rows = 0, cols = 0, stride, r, c;
	char line[256];
	unsigned char *m, *tmp;
	double hmin_r = 9, hmin_c = 9, hmed_r, hmed_c, h;
	double *hr, *hc;
	long ones_total = 0;

	if (!f) { perror(path); return 1; }

	/* 文件头：以 '#' 开头的若干 ASCII 行，原样打印（口径写在里面） */
	printf("=== 采集端写进文件头的口径 ===\n");
	for (;;) {
		long pos = ftell(f);
		if (!fgets(line, sizeof line, f)) break;
		if (line[0] != '#') { fseek(f, pos, SEEK_SET); break; }
		printf("  %s", line);
		if (!rows) sscanf(line, "#SP800-90B-restart-matrix rows=%ld cols=%ld",
				  &rows, &cols);
	}
	if (rows <= 0 || cols <= 0) { fprintf(stderr, "文件头没有 rows/cols\n"); return 2; }
	stride = (cols + 7) / 8;

	m = malloc((size_t)rows * stride);
	if (!m) return 1;
	if (fread(m, 1, (size_t)rows * stride, f) != (size_t)rows * stride) {
		fprintf(stderr, "数据不完整：要 %ld 字节\n", rows * stride);
		return 3;
	}
	fclose(f);

	tmp = malloc((size_t)(rows > cols ? rows : cols));
	hr = malloc(sizeof(double) * rows);
	hc = malloc(sizeof(double) * cols);
	if (!tmp || !hr || !hc) return 1;

	for (r = 0; r < rows; r++) {
		for (c = 0; c < cols; c++)
			tmp[c] = (m[r * stride + (c >> 3)] >> (7 - (c & 7))) & 1;
		for (c = 0; c < cols; c++) ones_total += tmp[c];
		hr[r] = mcv_of(tmp, cols);
		if (hr[r] < hmin_r) hmin_r = hr[r];
	}
	for (c = 0; c < cols; c++) {
		for (r = 0; r < rows; r++)
			tmp[r] = (m[r * stride + (c >> 3)] >> (7 - (c & 7))) & 1;
		hc[c] = mcv_of(tmp, rows);
		if (hc[c] < hmin_c) hmin_c = hc[c];
	}

	/* 中位数：简单选择即可，rows/cols 都只有一千 */
	{
		double *s = malloc(sizeof(double) * (rows > cols ? rows : cols));
		long i, j;
		memcpy(s, hr, sizeof(double) * rows);
		for (i = 0; i < rows; i++) for (j = i + 1; j < rows; j++)
			if (s[j] < s[i]) { double t = s[i]; s[i] = s[j]; s[j] = t; }
		hmed_r = s[rows / 2];
		memcpy(s, hc, sizeof(double) * cols);
		for (i = 0; i < cols; i++) for (j = i + 1; j < cols; j++)
			if (s[j] < s[i]) { double t = s[i]; s[i] = s[j]; s[j] = t; }
		hmed_c = s[cols / 2];
		free(s);
	}

	printf("\n=== %ld x %ld ===\n", rows, cols);
	printf("按行（一次重启之内）  最小 %.6f  中位 %.6f\n", hmin_r, hmed_r);
	printf("按列（跨重启同一位置）最小 %.6f  中位 %.6f\n", hmin_c, hmed_c);
	h = hmin_r < hmin_c ? hmin_r : hmin_c;
	printf("H_restart = min(行, 列) = %.6f 比特/样本\n", h);

	/* 列熵偏低的前几个：暂态若存在，形状会在这里显出来 */
	printf("\n列熵低于 0.5 的列（前 20 个）：");
	{
		int n = 0;
		for (c = 0; c < cols && n < 20; c++)
			if (hc[c] < 0.5) { printf(" %ld(%.3f)", c, hc[c]); n++; }
		if (!n) printf(" 无");
		printf("\n");
	}

	printf("整体 1 占比 %.6f\n", (double)ones_total / ((double)rows * cols));

	if (h_orig > 0) {
		printf("\nH_原始（顺序采集） = %.6f\n", h_orig);
		printf("判据：H_restart >= H_原始/2 = %.6f  →  %s\n",
		       h_orig / 2, (h >= h_orig / 2) ? "**通过**" : "**不通过**");
		return (h >= h_orig / 2) ? 0 : 1;
	}
	return 0;
}
