#include "pqchsm/kat.h"
#include "pqchsm/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct kat_reader {
	FILE  *fp;
	char  *line;      /* 动态增长的行缓冲：ML-DSA-87 的 sk 有 ~9.8K hex 字符 */
	size_t line_cap;
	char  *pool;      /* 当前记录的字符串池 */
	size_t pool_cap;
	size_t pool_used;
};

kat_reader_t *kat_open(const char *path)
{
	kat_reader_t *r = calloc(1, sizeof(*r));
	if (!r) {
		return NULL;
	}
	r->fp = fopen(path, "r");
	if (!r->fp) {
		free(r);
		return NULL;
	}
	r->line_cap = 4096;
	r->line = malloc(r->line_cap);
	r->pool_cap = 65536;
	r->pool = malloc(r->pool_cap);
	if (!r->line || !r->pool) {
		kat_close(r);
		return NULL;
	}
	return r;
}

void kat_close(kat_reader_t *r)
{
	if (!r) {
		return;
	}
	if (r->fp) {
		fclose(r->fp);
	}
	free(r->line);
	free(r->pool);
	free(r);
}

/* 读一整行（去掉换行符）。返回长度，EOF 且无内容返回 -1。 */
static long read_line(kat_reader_t *r)
{
	size_t len = 0;
	for (;;) {
		if (len + 2 > r->line_cap) {
			size_t nc = r->line_cap * 2;
			char *nb = realloc(r->line, nc);
			if (!nb) {
				return -1;
			}
			r->line = nb;
			r->line_cap = nc;
		}
		int c = fgetc(r->fp);
		if (c == EOF) {
			if (len == 0) {
				return -1;
			}
			break;
		}
		if (c == '\n') {
			break;
		}
		if (c != '\r') {
			r->line[len++] = (char)c;
		}
	}
	r->line[len] = '\0';
	return (long)len;
}

static char *pool_dup(kat_reader_t *r, const char *s, size_t n)
{
	while (r->pool_used + n + 1 > r->pool_cap) {
		size_t nc = r->pool_cap * 2;
		char *nb = realloc(r->pool, nc);
		if (!nb) {
			return NULL;
		}
		r->pool = nb;
		r->pool_cap = nc;
	}
	char *dst = r->pool + r->pool_used;
	memcpy(dst, s, n);
	dst[n] = '\0';
	r->pool_used += n + 1;
	return dst;
}

int kat_next(kat_reader_t *r, kat_record_t *rec)
{
	if (!r || !rec) {
		return -1;
	}
	memset(rec, 0, sizeof(*rec));
	r->pool_used = 0;

	for (;;) {
		long len = read_line(r);
		if (len < 0) {
			/* EOF：若已积累字段则算最后一条记录 */
			return rec->n_fields > 0 ? 1 : 0;
		}
		char *s = r->line;
		while (*s == ' ' || *s == '\t') {
			s++;
		}
		if (*s == '#') {
			continue;
		}
		if (*s == '\0') {
			if (rec->n_fields > 0) {
				return 1;   /* 记录结束 */
			}
			continue;       /* 记录间的多余空行 */
		}

		char *eq = strchr(s, '=');
		if (!eq) {
			return -1;
		}
		/* key */
		char *ke = eq;
		while (ke > s && (ke[-1] == ' ' || ke[-1] == '\t')) {
			ke--;
		}
		size_t klen = (size_t)(ke - s);
		if (klen == 0 || klen >= sizeof(rec->fields[0].name)) {
			return -1;
		}
		if (rec->n_fields >= KAT_MAX_FIELDS) {
			return -1;
		}
		/* value（允许为空，如 context = ） */
		char *v = eq + 1;
		while (*v == ' ' || *v == '\t') {
			v++;
		}
		size_t vlen = strlen(v);
		while (vlen && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t')) {
			vlen--;
		}

		kat_field_t *f = &rec->fields[rec->n_fields];
		memcpy(f->name, s, klen);
		f->name[klen] = '\0';
		f->value = pool_dup(r, v, vlen);
		if (!f->value) {
			return -1;
		}
		f->value_len = vlen;
		rec->n_fields++;
	}
}

const char *kat_str(const kat_record_t *rec, const char *name)
{
	if (!rec || !name) {
		return NULL;
	}
	for (size_t i = 0; i < rec->n_fields; i++) {
		if (strcmp(rec->fields[i].name, name) == 0) {
			return rec->fields[i].value;
		}
	}
	return NULL;
}

long kat_len(const kat_record_t *rec, const char *name)
{
	if (!rec || !name) {
		return -1;
	}
	for (size_t i = 0; i < rec->n_fields; i++) {
		if (strcmp(rec->fields[i].name, name) == 0) {
			return (long)(rec->fields[i].value_len / 2u);
		}
	}
	return -1;
}

long kat_bytes(const kat_record_t *rec, const char *name, uint8_t *out, size_t out_cap)
{
	if (!rec || !name) {
		return -1;
	}
	for (size_t i = 0; i < rec->n_fields; i++) {
		if (strcmp(rec->fields[i].name, name) == 0) {
			const kat_field_t *f = &rec->fields[i];
			if (f->value_len == 0) {
				return 0;
			}
			return pqc_hex_decode(f->value, f->value_len, out, out_cap);
		}
	}
	return -1;
}
