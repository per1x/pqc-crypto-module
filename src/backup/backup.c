#include "pqchsm/backup.h"

#include "pqchsm/kdf.h"
#include "pqchsm/shamir.h"
#include "pqchsm/util.h"
#include "pqchsm/wrap.h"
#include "../slot/persist.h"
#include "../slot/slot_internal.h"
#include "pqchsm/audit.h"

#include <fcntl.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const uint8_t BK_MAGIC[8] = { 'P', 'Q', 'C', 'H', 'S', 'M', 'B', 'K' };

#define BK_SALT_LEN 16
#define BK_HDR_LEN  (8 + 4 + 4 + BK_SALT_LEN)
#define BK_TAG_LEN  32
/* 每条记录：slot_id u32 | blob_len u32 | blob */
#define BK_REC_HDR  8

static void put_u32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++) {
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

/* BEK / 文件 MAC 密钥都从 RMK 派生，**不经过 KDR** —— 这正是备份能跨设备
 * 恢复而密钥库不能的原因（§8.3）。两者用不同域分隔串，互不可替换。 */
static int derive_from_rmk(const uint8_t rmk[HSM_RMK_LEN], const uint8_t *salt,
                           const char *label, uint8_t *out, size_t out_len)
{
	return pqc_kdf(rmk, HSM_RMK_LEN, salt, BK_SALT_LEN, label, out, out_len);
}

hsm_status_t hsm_backup_export(hsm_token_t *tok, hsm_session_t sess, const char *path,
                               uint8_t m, uint8_t n,
                               uint8_t *shares, size_t share_cap, size_t *share_lens,
                               size_t *n_exported)
{
	if (!tok || !path || !shares || !share_lens || share_cap < HSM_SHARE_LEN) {
		return HSM_ERR_BAD_ARG;
	}
	if (m < 2 || m > n || n > 16) {
		return HSM_ERR_BAD_ARG;
	}
	/* §7.3：导出备份是敏感操作，要求 SO */
	hsm_role_t role;
	hsm_status_t st = hsm_session_role(tok, sess, &role);
	if (st != HSM_OK) {
		return st;
	}
	if (role != HSM_ROLE_SO) {
		return HSM_ERR_NOT_AUTHORIZED;
	}

	size_t n_slots = hsm_token_slot_count(tok);
	size_t blob_cap = hsm_slot_blob_max();

	uint8_t rmk[HSM_RMK_LEN], salt[BK_SALT_LEN], bek[PQC_KEK_LEN], fkey[32];
	uint8_t *body = malloc(BK_HDR_LEN + n_slots * (BK_REC_HDR + blob_cap));
	uint8_t *blob = pqc_secure_alloc(blob_cap);
	char *tmp_path = NULL;
	int fd = -1;
	size_t exported = 0;

	st = HSM_ERR_NOMEM;
	if (!body || !blob) {
		goto out;
	}
	st = HSM_ERR_CRYPTO;
	if (RAND_bytes(rmk, sizeof(rmk)) != 1 || RAND_bytes(salt, sizeof(salt)) != 1) {
		goto out;
	}
	if (derive_from_rmk(rmk, salt, "pqc-hsm/backup-bek", bek, sizeof(bek)) != 0 ||
	    derive_from_rmk(rmk, salt, "pqc-hsm/backup-filemac", fkey, sizeof(fkey)) != 0) {
		goto out;
	}

	memcpy(body, BK_MAGIC, 8);
	put_u32(body + 8, HSM_BACKUP_VERSION);
	memcpy(body + 16, salt, BK_SALT_LEN);
	size_t body_len = BK_HDR_LEN;

	for (size_t i = 0; i < n_slots; i++) {
		slot_meta_t meta;
		if (hsm_slot_get_meta(tok, (hsm_slot_id_t)i, &meta) != HSM_OK) {
			continue;
		}
		/* §8.3：只有"可恢复槽位"进备份；纯 sealed 槽位跟着设备一起消失 */
		if (!(meta.policy & SLOT_POLICY_BACKUPABLE)) {
			continue;
		}
		size_t blob_len = 0;
		st = hsm_slot_serialize(tok, (hsm_slot_id_t)i, bek, sizeof(bek),
		                        blob, blob_cap, &blob_len);
		if (st != HSM_OK) {
			goto out;
		}
		put_u32(body + body_len, (uint32_t)i);
		put_u32(body + body_len + 4, (uint32_t)blob_len);
		body_len += BK_REC_HDR;
		memcpy(body + body_len, blob, blob_len);
		body_len += blob_len;
		exported++;
	}
	put_u32(body + 12, (uint32_t)exported);

	{
		uint8_t tag[BK_TAG_LEN];
		st = HSM_ERR_CRYPTO;
		if (pqc_kmac256(fkey, sizeof(fkey), body, body_len, "pqc-hsm/backup",
		                tag, sizeof(tag)) != 0) {
			goto out;
		}
		/* 原子写，与密钥库同样的 tmp → fsync → rename */
		size_t plen = strlen(path);
		tmp_path = malloc(plen + 8);
		if (!tmp_path) {
			st = HSM_ERR_NOMEM;
			goto out;
		}
		snprintf(tmp_path, plen + 8, "%s.tmp", path);
		fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (fd < 0) {
			st = HSM_ERR_BAD_ARG;
			goto out;
		}
		st = HSM_ERR_CRYPTO;
		if (write(fd, body, body_len) != (ssize_t)body_len) {
			goto out;
		}
		if (write(fd, tag, sizeof(tag)) != (ssize_t)sizeof(tag)) {
			goto out;
		}
		if (fsync(fd) != 0) {
			goto out;
		}
		close(fd);
		fd = -1;
		if (rename(tmp_path, path) != 0) {
			goto out;
		}
		pqc_secure_zero(tag, sizeof(tag));
	}

	/* 最后一步才切分 RMK：前面任何失败都不会留下有效分片 */
	if (shamir_split(rmk, HSM_RMK_LEN, m, n, shares, share_cap, share_lens) != 0) {
		st = HSM_ERR_CRYPTO;
		(void)unlink(path);
		goto out;
	}
	if (n_exported) {
		*n_exported = exported;
	}
	st = HSM_OK;
out:
	if (fd >= 0) {
		close(fd);
	}
	if (st != HSM_OK && tmp_path) {
		(void)unlink(tmp_path);
	}
	free(tmp_path);
	free(body);
	pqc_secure_free(blob, blob_cap);
	/* RMK / BEK 用后即清（§8.7）：此后世上只剩分片 */
	pqc_secure_zero(rmk, sizeof(rmk));
	pqc_secure_zero(bek, sizeof(bek));
	pqc_secure_zero(fkey, sizeof(fkey));
	pqc_secure_zero(salt, sizeof(salt));
	/* §8.6：备份导出是最敏感的操作之一，成功失败都要留痕 */
	slot_audit(tok, AUDIT_OP_BACKUP_EXPORT, HSM_ROLE_SO, 0, st, path);
	return st;
}

hsm_status_t hsm_backup_restore(hsm_token_t *tok, const char *path,
                                const uint8_t *shares, size_t share_cap,
                                const size_t *share_lens, uint8_t k,
                                size_t *n_restored)
{
	if (!tok || !path || !shares || !share_lens || k == 0) {
		return HSM_ERR_BAD_ARG;
	}
	uint8_t rmk[HSM_RMK_LEN], bek[PQC_KEK_LEN], fkey[32];
	memset(rmk, 0, sizeof(rmk));
	memset(bek, 0, sizeof(bek));
	memset(fkey, 0, sizeof(fkey));

	uint8_t *buf = NULL;
	size_t fsz = 0;
	hsm_status_t st = HSM_ERR_INTEGRITY;
	size_t restored = 0;

	{
		size_t rmk_len = 0;
		/* 分片不足 m 份时这里会"成功"地算出一个错误的 RMK ——
		 * 由后面的 GCM tag 校验兜底（见 backup.h 的说明）。 */
		if (shamir_combine(shares, share_cap, share_lens, k,
		                   rmk, sizeof(rmk), &rmk_len) != 0 || rmk_len != HSM_RMK_LEN) {
			st = HSM_ERR_INTEGRITY;
			goto out;
		}
	}

	{
		FILE *f = fopen(path, "rb");
		if (!f) {
			st = HSM_ERR_BAD_ARG;
			goto out;
		}
		fseek(f, 0, SEEK_END);
		long sz = ftell(f);
		rewind(f);
		if (sz < (long)(BK_HDR_LEN + BK_TAG_LEN)) {
			fclose(f);
			goto out;
		}
		buf = malloc((size_t)sz);
		if (!buf) {
			fclose(f);
			st = HSM_ERR_NOMEM;
			goto out;
		}
		size_t got = fread(buf, 1, (size_t)sz, f);
		fclose(f);
		if (got != (size_t)sz) {
			goto out;
		}
		fsz = (size_t)sz;
	}

	if (memcmp(buf, BK_MAGIC, 8) != 0 || get_u32(buf + 8) != HSM_BACKUP_VERSION) {
		goto out;
	}
	{
		size_t n_recs = get_u32(buf + 12);
		const uint8_t *salt = buf + 16;
		if (derive_from_rmk(rmk, salt, "pqc-hsm/backup-bek", bek, sizeof(bek)) != 0 ||
		    derive_from_rmk(rmk, salt, "pqc-hsm/backup-filemac", fkey, sizeof(fkey)) != 0) {
			st = HSM_ERR_CRYPTO;
			goto out;
		}
		size_t body_len = fsz - BK_TAG_LEN;
		uint8_t want[BK_TAG_LEN];
		if (pqc_kmac256(fkey, sizeof(fkey), buf, body_len, "pqc-hsm/backup",
		                want, sizeof(want)) != 0) {
			st = HSM_ERR_CRYPTO;
			goto out;
		}
		/* 分片凑不齐 / 分片错 / 文件被改 —— 都在这一步现形 */
		int mac_ok = pqc_ct_equal(want, buf + body_len, BK_TAG_LEN);
		pqc_secure_zero(want, sizeof(want));
		if (!mac_ok) {
			goto out;
		}

		size_t off = BK_HDR_LEN;
		for (size_t i = 0; i < n_recs; i++) {
			if (off + BK_REC_HDR > body_len) {
				goto out;
			}
			uint32_t slot_id = get_u32(buf + off);
			size_t blen = get_u32(buf + off + 4);
			off += BK_REC_HDR;
			if (blen > body_len - off) {
				goto out;
			}
			if (slot_id >= hsm_token_slot_count(tok)) {
				goto out;
			}
			/* slot_deserialize 内部还会再校验记录里的 slot_id 与目标槽位一致，
			 * 所以把 slot 3 的记录改到 slot 5 会被拦住 */
			st = hsm_slot_deserialize(tok, (hsm_slot_id_t)slot_id, bek, sizeof(bek),
			                          buf + off, blen);
			if (st != HSM_OK) {
				goto out;
			}
			off += blen;
			restored++;
		}
		if (off != body_len) {
			st = HSM_ERR_INTEGRITY;
			goto out;
		}
		if (n_restored) {
			*n_restored = restored;
		}
		st = HSM_OK;
	}
out:
	if (buf) {
		pqc_secure_zero(buf, fsz);
		free(buf);
	}
	/* 恢复过程的中间值用后即清（§8.7 红线） */
	pqc_secure_zero(rmk, sizeof(rmk));
	pqc_secure_zero(bek, sizeof(bek));
	pqc_secure_zero(fkey, sizeof(fkey));
	/* 恢复仪式：谁、何时、恢复了哪个库、成功与否（§8.4 全程审计） */
	slot_audit(tok, AUDIT_OP_RESTORE, HSM_ROLE_SO, 0, st, path);
	return st;
}
