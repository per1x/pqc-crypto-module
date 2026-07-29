/* pqchsm/kat.h —— 扁平黄金向量解析器（vectors 目录下的 .kat 文件）
 *
 * 格式见 tools/acvp_to_kat.py：`key = hexvalue` 行，空行分隔记录，# 为注释。
 * 刻意做得极简，以便将来 cocotb testbench 复用同一批向量文件。
 */
#ifndef PQCHSM_KAT_H
#define PQCHSM_KAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KAT_MAX_FIELDS 16

typedef struct {
	char   name[24];
	char  *value;    /* 原始字符串（hex 或十进制/关键字），归 kat_record 所有 */
	size_t value_len;
} kat_field_t;

typedef struct {
	kat_field_t fields[KAT_MAX_FIELDS];
	size_t      n_fields;
} kat_record_t;

typedef struct kat_reader kat_reader_t;

kat_reader_t *kat_open(const char *path);
void          kat_close(kat_reader_t *r);

/* 读下一条记录。返回 1 = 有记录，0 = 文件结束，-1 = 解析错误。 */
int kat_next(kat_reader_t *r, kat_record_t *rec);

/* 取字段原始字符串；不存在返回 NULL。 */
const char *kat_str(const kat_record_t *rec, const char *name);

/* 取字段并 hex 解码到 out。返回字节数；字段不存在或解码失败返回 -1。
 * 空值（如 context = ）返回 0。 */
long kat_bytes(const kat_record_t *rec, const char *name, uint8_t *out, size_t out_cap);

/* 字段的解码后字节长度（不写出）；不存在返回 -1。 */
long kat_len(const kat_record_t *rec, const char *name);

#ifdef __cplusplus
}
#endif
#endif /* PQCHSM_KAT_H */
