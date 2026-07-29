#include "pqchsm/keystore.h"

#include "pqchsm/kdf.h"
#include "pqchsm/kdr.h"
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

static const uint8_t KS_MAGIC[8] = { 'P', 'Q', 'C', 'H', 'S', 'M', 'K', 'S' };

#define KS_SALT_LEN 16
#define KS_HDR_LEN  (8 + 4 + 4 + KS_SALT_LEN)
#define KS_TAG_LEN  32

/* ---- 掉电模拟钩子（仅测试用）------------------------------------------- */
static int g_crash_point = -1;
static int g_write_count;

void hsm_keystore_set_crash_point(int n)
{
	g_crash_point = n;
}

int hsm_keystore_last_write_count(void)
{
	return g_write_count;
}

/* 所有落盘写都经这里，便于在任意一次写之后模拟断电 */
static int ks_write(int fd, const void *buf, size_t n)
{
	const uint8_t *p = (const uint8_t *)buf;
	size_t done = 0;
	while (done < n) {
		ssize_t w = write(fd, p + done, n - done);
		if (w <= 0) {
			return -1;
		}
		done += (size_t)w;
	}
	g_write_count++;
	if (g_crash_point >= 0 && g_write_count >= g_crash_point) {
		/* 模拟掉电：不 fsync、不 rename、不清理，直接消失 */
		_exit(9);
	}
	return 0;
}

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

/* 全文件 MAC 的密钥：由 KDR 按 kek_salt 派生，与 KEK 分开（不同域分隔串） */
static int derive_file_key(const uint8_t *salt, uint8_t key[32])
{
	return pqc_kdr_derive("pqc-hsm/keystore-filemac", salt, KS_SALT_LEN, key, 32);
}

static void fsync_dir(const char *path)
{
	char *copy = strdup(path);
	if (!copy) {
		return;
	}
	char *slash = strrchr(copy, '/');
	const char *dir = ".";
	if (slash) {
		*slash = '\0';
		dir = copy[0] ? copy : "/";
	}
	int dfd = open(dir, O_RDONLY);
	if (dfd >= 0) {
		(void)fsync(dfd);
		close(dfd);
	}
	free(copy);
}

hsm_status_t hsm_keystore_save(hsm_token_t *tok, const char *path)
{
	if (!tok || !path) {
		return HSM_ERR_BAD_ARG;
	}
	size_t n_slots = hsm_token_slot_count(tok);
	size_t blob_cap = hsm_slot_blob_max();

	uint8_t salt[KS_SALT_LEN], kek[PQC_KEK_LEN], fkey[32];
	if (RAND_bytes(salt, sizeof(salt)) != 1) {
		return HSM_ERR_CRYPTO;
	}
	/* 每次落盘换一把新 KEK ⇒ 顺带完成 §8.1 的 KEK 轮换 */
	if (pqc_kek_derive(salt, sizeof(salt), kek) != 0 || derive_file_key(salt, fkey) != 0) {
		return HSM_ERR_CRYPTO;
	}

	/* 先在内存里拼出整个文件（除 tag），再一次性落盘 —— 这样全文件 MAC 好算，
	 * 而且写入期间不需要回头 seek。 */
	size_t body_cap = KS_HDR_LEN + n_slots * (4 + blob_cap);
	uint8_t *body = malloc(body_cap);
	uint8_t *blob = pqc_secure_alloc(blob_cap);
	hsm_status_t st = HSM_ERR_NOMEM;
	char *tmp_path = NULL;
	int fd = -1;
	if (!body || !blob) {
		goto out;
	}

	memcpy(body, KS_MAGIC, 8);
	put_u32(body + 8, HSM_KEYSTORE_VERSION);
	put_u32(body + 12, (uint32_t)n_slots);
	memcpy(body + 16, salt, KS_SALT_LEN);
	size_t body_len = KS_HDR_LEN;

	for (size_t i = 0; i < n_slots; i++) {
		size_t blob_len = 0;
		st = hsm_slot_serialize(tok, (hsm_slot_id_t)i, kek, sizeof(kek),
		                        blob, blob_cap, &blob_len);
		if (st != HSM_OK) {
			goto out;
		}
		put_u32(body + body_len, (uint32_t)blob_len);
		body_len += 4;
		memcpy(body + body_len, blob, blob_len);
		body_len += blob_len;
	}

	uint8_t tag[KS_TAG_LEN];
	if (pqc_kmac256(fkey, sizeof(fkey), body, body_len, "pqc-hsm/keystore",
	                tag, sizeof(tag)) != 0) {
		st = HSM_ERR_CRYPTO;
		goto out;
	}

	/* 原子写：tmp → fsync → rename → fsync(dir) */
	size_t plen = strlen(path);
	tmp_path = malloc(plen + 8);
	if (!tmp_path) {
		st = HSM_ERR_NOMEM;
		goto out;
	}
	snprintf(tmp_path, plen + 8, "%s.tmp", path);

	g_write_count = 0;
	fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		st = HSM_ERR_BAD_ARG;
		goto out;
	}
	/* 按"头部 / 每条记录 / 尾部 tag"分次写：既贴近真实写入模式，
	 * 也让掉电模拟能落在文件中间的多个位置（§5.7.3）。 */
	st = HSM_ERR_CRYPTO;
	if (ks_write(fd, body, KS_HDR_LEN) != 0) {
		goto out;
	}
	{
		size_t off = KS_HDR_LEN;
		for (size_t i = 0; i < n_slots; i++) {
			size_t rlen = 4 + get_u32(body + off);
			if (ks_write(fd, body + off, rlen) != 0) {
				goto out;
			}
			off += rlen;
		}
	}
	if (ks_write(fd, tag, sizeof(tag)) != 0) {
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
	fsync_dir(path);
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
	pqc_secure_zero(kek, sizeof(kek));
	pqc_secure_zero(fkey, sizeof(fkey));
	pqc_secure_zero(salt, sizeof(salt));
	/* 每次 save 都换新 KEK，等价于一次 KEK 轮换（§8.1） */
	slot_audit(tok, AUDIT_OP_KEK_ROTATE, HSM_ROLE_PUBLIC, 0, st, "keystore-save");
	return st;
}

hsm_status_t hsm_keystore_load(hsm_token_t *tok, const char *path)
{
	if (!tok || !path) {
		return HSM_ERR_BAD_ARG;
	}
	FILE *f = fopen(path, "rb");
	if (!f) {
		return HSM_ERR_BAD_ARG;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return HSM_ERR_BAD_ARG;
	}
	long fsz = ftell(f);
	rewind(f);
	if (fsz < (long)(KS_HDR_LEN + KS_TAG_LEN)) {
		fclose(f);
		return HSM_ERR_INTEGRITY;
	}
	uint8_t *buf = malloc((size_t)fsz);
	if (!buf) {
		fclose(f);
		return HSM_ERR_NOMEM;
	}
	size_t got = fread(buf, 1, (size_t)fsz, f);
	fclose(f);
	hsm_status_t st = HSM_ERR_INTEGRITY;
	uint8_t kek[PQC_KEK_LEN], fkey[32];
	memset(kek, 0, sizeof(kek));
	memset(fkey, 0, sizeof(fkey));
	if (got != (size_t)fsz) {
		goto out;
	}
	if (memcmp(buf, KS_MAGIC, 8) != 0 || get_u32(buf + 8) != HSM_KEYSTORE_VERSION) {
		goto out;
	}
	{
		size_t n_slots = get_u32(buf + 12);
		if (n_slots != hsm_token_slot_count(tok)) {
			goto out;
		}
		const uint8_t *salt = buf + 16;
		if (derive_file_key(salt, fkey) != 0 || pqc_kek_derive(salt, KS_SALT_LEN, kek) != 0) {
			st = HSM_ERR_CRYPTO;
			goto out;
		}
		/* 先验全文件 MAC：记录删除/调换/截断都在这一步被拦住 */
		size_t body_len = (size_t)fsz - KS_TAG_LEN;
		uint8_t want[KS_TAG_LEN];
		if (pqc_kmac256(fkey, sizeof(fkey), buf, body_len, "pqc-hsm/keystore",
		                want, sizeof(want)) != 0) {
			st = HSM_ERR_CRYPTO;
			goto out;
		}
		if (!pqc_ct_equal(want, buf + body_len, KS_TAG_LEN)) {
			goto out;
		}

		/* MAC 过了才逐槽位装载。到这一步失败只可能是文件内部自相矛盾。 */
		size_t off = KS_HDR_LEN;
		for (size_t i = 0; i < n_slots; i++) {
			if (off + 4 > body_len) {
				goto out;
			}
			size_t blen = get_u32(buf + off);
			off += 4;
			if (blen > body_len - off) {
				goto out;
			}
			st = hsm_slot_deserialize(tok, (hsm_slot_id_t)i, kek, sizeof(kek),
			                          buf + off, blen);
			if (st != HSM_OK) {
				goto out;
			}
			off += blen;
		}
		if (off != body_len) {
			st = HSM_ERR_INTEGRITY;   /* 尾部有多余数据 */
			goto out;
		}
		st = HSM_OK;
	}
out:
	pqc_secure_zero(buf, (size_t)fsz);
	free(buf);
	pqc_secure_zero(kek, sizeof(kek));
	pqc_secure_zero(fkey, sizeof(fkey));
	return st;
}
