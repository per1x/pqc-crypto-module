#include "pqchsm/anchor.h"

#include "pqchsm/util.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const uint8_t AN_MAGIC[8] = { 'P', 'Q', 'C', 'H', 'S', 'M', 'A', 'N' };

/* 被签名的定长前缀布局 */
#define OFF_MAGIC   0u
#define OFF_VERSION 8u
#define OFF_ALG     12u
#define OFF_COUNT   16u
#define OFF_HEAD    24u
#define OFF_TS      56u
/* = HSM_ANCHOR_SIGNED_LEN (64) */

static void put_u32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++) {
		p[i] = (uint8_t)(v >> (8 * i));
	}
}

static void put_u64(uint8_t *p, uint64_t v)
{
	for (int i = 0; i < 8; i++) {
		p[i] = (uint8_t)(v >> (8 * i));
	}
}

static uint32_t get_u32(const uint8_t *p)
{
	uint32_t v = 0;
	for (int i = 0; i < 4; i++) {
		v |= (uint32_t)p[i] << (8 * i);
	}
	return v;
}

static uint64_t get_u64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; i++) {
		v |= (uint64_t)p[i] << (8 * i);
	}
	return v;
}

static uint8_t *slurp(const char *path, size_t *out_n)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	long sz = ftell(f);
	rewind(f);
	if (sz <= 0) {
		fclose(f);
		return NULL;
	}
	uint8_t *b = malloc((size_t)sz);
	if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
		free(b);
		b = NULL;
	}
	fclose(f);
	if (b) {
		*out_n = (size_t)sz;
	}
	return b;
}

/* 解析锚点文件。成功返回 0，并回填各段指针（指向 buf 内部，不拷贝）。 */
static int parse_anchor(const uint8_t *buf, size_t n, uint32_t *alg, uint64_t *count,
                        const uint8_t **head, uint64_t *ts,
                        const uint8_t **pk, size_t *pk_len,
                        const uint8_t **sig, size_t *sig_len)
{
	if (n < HSM_ANCHOR_SIGNED_LEN + 8) {
		return -1;
	}
	if (memcmp(buf, AN_MAGIC, 8) != 0) {
		return -1;
	}
	if (get_u32(buf + OFF_VERSION) != HSM_ANCHOR_VERSION) {
		return -1;
	}
	*alg   = get_u32(buf + OFF_ALG);
	*count = get_u64(buf + OFF_COUNT);
	*head  = buf + OFF_HEAD;
	*ts    = get_u64(buf + OFF_TS);

	size_t off = HSM_ANCHOR_SIGNED_LEN;
	size_t plen = get_u32(buf + off);
	off += 4;
	if (plen > n - off) {
		return -1;
	}
	*pk = buf + off;
	*pk_len = plen;
	off += plen;
	if (n - off < 4) {
		return -1;
	}
	size_t slen = get_u32(buf + off);
	off += 4;
	if (slen != n - off) {
		return -1;      /* 尾部必须刚好是签名，不许有多余数据 */
	}
	*sig = buf + off;
	*sig_len = slen;
	return 0;
}

hsm_status_t hsm_audit_anchor_create(audit_log_t *log, const char *anchor_path,
                                     hsm_token_t *tok, hsm_session_t sess,
                                     hsm_handle_t identity_key, uint64_t timestamp)
{
	if (!log || !anchor_path || !tok) {
		return HSM_ERR_BAD_ARG;
	}
	uint8_t head[AUDIT_HASH_LEN];
	if (audit_head(log, head) != 0) {
		return HSM_ERR_CRYPTO;
	}
	uint64_t count = audit_count(log);

	/* 身份钥的算法参数集从元数据取 —— 锚点里要记下来，校验方才知道用哪个算法验签 */
	slot_meta_t meta;
	hsm_slot_id_t slot = (hsm_slot_id_t)((identity_key & 0xffffffffu) - 1);
	hsm_status_t st = hsm_slot_get_meta(tok, slot, &meta);
	if (st != HSM_OK) {
		return st;
	}
	const pqc_alg_info_t *info = pqc_alg_info(meta.alg);
	if (!info || info->kind != PQC_KIND_SIG) {
		return HSM_ERR_BAD_ARG;
	}

	uint8_t signed_pfx[HSM_ANCHOR_SIGNED_LEN];
	memset(signed_pfx, 0, sizeof(signed_pfx));
	memcpy(signed_pfx + OFF_MAGIC, AN_MAGIC, 8);
	put_u32(signed_pfx + OFF_VERSION, HSM_ANCHOR_VERSION);
	put_u32(signed_pfx + OFF_ALG, (uint32_t)meta.alg);
	put_u64(signed_pfx + OFF_COUNT, count);
	memcpy(signed_pfx + OFF_HEAD, head, AUDIT_HASH_LEN);
	put_u64(signed_pfx + OFF_TS, timestamp);

	uint8_t *pk = malloc(info->pk_len);
	uint8_t *sig = malloc(info->sig_len);
	char *tmp_path = NULL;
	int fd = -1;
	size_t pk_len = 0, sig_len = 0;
	st = HSM_ERR_NOMEM;
	if (!pk || !sig) {
		goto out;
	}
	st = hsm_object_public_key(tok, sess, identity_key, pk, info->pk_len, &pk_len);
	if (st != HSM_OK) {
		goto out;
	}
	/* 签的是定长前缀本身，不含公钥与签名 —— 避免"签名覆盖自己"的循环 */
	st = hsm_object_sign(tok, sess, identity_key, signed_pfx, sizeof(signed_pfx),
	                     NULL, 0, sig, info->sig_len, &sig_len);
	if (st != HSM_OK) {
		goto out;
	}

	{
		size_t plen = strlen(anchor_path);
		tmp_path = malloc(plen + 8);
		if (!tmp_path) {
			st = HSM_ERR_NOMEM;
			goto out;
		}
		snprintf(tmp_path, plen + 8, "%s.tmp", anchor_path);
		fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (fd < 0) {
			st = HSM_ERR_BAD_ARG;
			goto out;
		}
		uint8_t lenbuf[4];
		st = HSM_ERR_CRYPTO;
		if (write(fd, signed_pfx, sizeof(signed_pfx)) != (ssize_t)sizeof(signed_pfx)) {
			goto out;
		}
		put_u32(lenbuf, (uint32_t)pk_len);
		if (write(fd, lenbuf, 4) != 4 || write(fd, pk, pk_len) != (ssize_t)pk_len) {
			goto out;
		}
		put_u32(lenbuf, (uint32_t)sig_len);
		if (write(fd, lenbuf, 4) != 4 || write(fd, sig, sig_len) != (ssize_t)sig_len) {
			goto out;
		}
		if (fsync(fd) != 0) {
			goto out;
		}
		close(fd);
		fd = -1;
		if (rename(tmp_path, anchor_path) != 0) {
			goto out;
		}
		st = HSM_OK;
	}
out:
	if (fd >= 0) {
		close(fd);
	}
	if (st != HSM_OK && tmp_path) {
		(void)unlink(tmp_path);
	}
	free(tmp_path);
	free(pk);
	free(sig);
	return st;
}

hsm_status_t hsm_audit_anchor_verify(const char *log_path, const char *anchor_path,
                                     const uint8_t *expect_pk, size_t expect_pk_len,
                                     uint64_t *anchored_count)
{
	if (!log_path || !anchor_path || !expect_pk || expect_pk_len == 0) {
		return HSM_ERR_BAD_ARG;
	}
	size_t n = 0;
	uint8_t *buf = slurp(anchor_path, &n);
	if (!buf) {
		return HSM_ERR_BAD_ARG;
	}
	hsm_status_t st = HSM_ERR_INTEGRITY;
	uint32_t alg_id = 0;
	uint64_t count = 0, ts = 0;
	const uint8_t *head = NULL, *pk = NULL, *sig = NULL;
	size_t pk_len = 0, sig_len = 0;

	if (parse_anchor(buf, n, &alg_id, &count, &head, &ts, &pk, &pk_len, &sig, &sig_len) != 0) {
		goto out;
	}
	{
		pqc_alg_t alg = (pqc_alg_t)alg_id;
		const pqc_alg_info_t *info = pqc_alg_info(alg);
		if (!info || info->kind != PQC_KIND_SIG || pk_len != info->pk_len) {
			goto out;
		}
		/* (2) 锚点里的公钥必须就是调用方带来的那把 —— 否则签名验了也没意义 */
		if (pk_len != expect_pk_len || !pqc_ct_equal(pk, expect_pk, pk_len)) {
			goto out;
		}
		/* (1) 签名在该公钥下有效 */
		if (pqc_verify(alg, expect_pk, buf, HSM_ANCHOR_SIGNED_LEN, NULL, 0,
		               sig, sig_len) != PQC_OK) {
			goto out;
		}
		/* (3) 日志自身的链完好 */
		if (audit_verify_file(log_path, NULL) != 0) {
			goto out;
		}
		/* (4) 日志前 count 条之后的链哈希，必须等于被签过的 head。
		 * 这一条才是真正堵洞的：攻击者可以把整条链重算得自洽（第 3 条照过），
		 * 但改不出一个能对上已签 head 的前缀。 */
		uint8_t got[AUDIT_HASH_LEN];
		if (audit_hash_at(log_path, count, got) != 0) {
			goto out;   /* 日志被截短到锚点之下 */
		}
		if (!pqc_ct_equal(got, head, AUDIT_HASH_LEN)) {
			goto out;
		}
		if (anchored_count) {
			*anchored_count = count;
		}
		st = HSM_OK;
	}
out:
	free(buf);
	return st;
}

hsm_status_t hsm_audit_anchor_peek_pk(const char *anchor_path, pqc_alg_t *alg,
                                      uint8_t *pk_out, size_t cap, size_t *pk_len_out)
{
	if (!anchor_path || !pk_out || !pk_len_out) {
		return HSM_ERR_BAD_ARG;
	}
	size_t n = 0;
	uint8_t *buf = slurp(anchor_path, &n);
	if (!buf) {
		return HSM_ERR_BAD_ARG;
	}
	hsm_status_t st = HSM_ERR_INTEGRITY;
	uint32_t alg_id = 0;
	uint64_t count = 0, ts = 0;
	const uint8_t *head = NULL, *pk = NULL, *sig = NULL;
	size_t pk_len = 0, sig_len = 0;
	if (parse_anchor(buf, n, &alg_id, &count, &head, &ts, &pk, &pk_len, &sig, &sig_len) != 0) {
		goto out;
	}
	if (pk_len > cap) {
		st = HSM_ERR_BAD_ARG;
		goto out;
	}
	memcpy(pk_out, pk, pk_len);
	*pk_len_out = pk_len;
	if (alg) {
		*alg = (pqc_alg_t)alg_id;
	}
	st = HSM_OK;
out:
	free(buf);
	return st;
}
