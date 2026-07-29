#include "testlib.h"
#include "pqchsm/kat.h"

#include <stdio.h>
#include <unistd.h>

static const char *SAMPLE =
	"# 注释行\n"
	"# SKIPPED: 某类组\n"
	"\n"
	"alg = ML-KEM-768\n"
	"op = keygen\n"
	"tcid = 1\n"
	"d = 00112233\n"
	"\n"
	"\n"                        /* 多余空行不应产生空记录 */
	"alg = ML-DSA-65\n"
	"op = siggen\n"
	"context =\n"               /* 空值字段 */
	"sig = aabb\n"
	"\n"
	"alg = ML-KEM-512\n"        /* 文件末尾无空行 */
	"op = decaps\n";

static char *write_temp(void)
{
	static char path[] = "/tmp/pqchsm_kat_XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0) {
		return NULL;
	}
	FILE *f = fdopen(fd, "w");
	fputs(SAMPLE, f);
	fclose(f);
	return path;
}

int main(void)
{
	char *path = write_temp();
	if (!path) {
		fprintf(stderr, "无法创建临时文件\n");
		return 1;
	}

	kat_reader_t *r = kat_open(path);
	CHECK(r != NULL);
	if (!r) {
		return 1;
	}

	kat_record_t rec;
	uint8_t buf[8];

	TCASE("第 1 条记录");
	CHECK_EQ_INT(kat_next(r, &rec), 1);
	CHECK(kat_str(&rec, "alg") && strcmp(kat_str(&rec, "alg"), "ML-KEM-768") == 0);
	CHECK(kat_str(&rec, "op") && strcmp(kat_str(&rec, "op"), "keygen") == 0);
	CHECK_EQ_INT(kat_len(&rec, "d"), 4);            /* 8 个 hex 字符 = 4 字节 */
	CHECK_EQ_INT(kat_bytes(&rec, "d", buf, sizeof(buf)), 4);
	CHECK_EQ_INT(buf[0], 0x00);
	CHECK_EQ_INT(buf[1], 0x11);
	CHECK_EQ_INT(buf[3], 0x33);
	/* 缓冲不足必须失败而不是截断 */
	CHECK_EQ_INT(kat_bytes(&rec, "d", buf, 2), -1);
	CHECK(kat_str(&rec, "nonexistent") == NULL);
	CHECK_EQ_INT(kat_len(&rec, "nonexistent"), -1);

	TCASE("第 2 条记录：空值字段与跳过多余空行");
	CHECK_EQ_INT(kat_next(r, &rec), 1);
	CHECK(kat_str(&rec, "alg") && strcmp(kat_str(&rec, "alg"), "ML-DSA-65") == 0);
	CHECK(kat_str(&rec, "context") != NULL);            /* 存在 */
	CHECK_EQ_INT(kat_len(&rec, "context"), 0);          /* 长度 0 */
	CHECK_EQ_INT(kat_bytes(&rec, "context", buf, sizeof(buf)), 0);
	CHECK_EQ_INT(kat_len(&rec, "sig"), 2);

	TCASE("第 3 条记录：文件末尾无空行也要收下");
	CHECK_EQ_INT(kat_next(r, &rec), 1);
	CHECK(kat_str(&rec, "alg") && strcmp(kat_str(&rec, "alg"), "ML-KEM-512") == 0);

	TCASE("文件结束");
	CHECK_EQ_INT(kat_next(r, &rec), 0);

	kat_close(r);
	kat_close(NULL);            /* 不应崩溃 */
	CHECK(kat_open("/nonexistent/path.kat") == NULL);
	unlink(path);

	return test_report("test_kat_parse");
}
