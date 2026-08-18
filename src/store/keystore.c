#include "pqchsm/keystore.h"

#include "pqchsm/kdf.h"
#include "pqchsm/kdr.h"
#include "pqchsm/rbanchor.h"
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
/* 头部：magic(8) | version(4) | n_slots(4) | salt(16) | epoch(8) */
#define KS_HDR_LEN  (8 + 4 + 4 + KS_SALT_LEN + 8)
#define KS_TAG_LEN  32
#define KS_OFF_EPOCH (8 + 4 + 4 + KS_SALT_LEN)

/* ============================================================================
 * 【防回滚 epoch —— 它挡得住什么、挡不住什么，取决于锚点放在哪】
 * ============================================================================
 * 独立评审 H3：keystore 没有任何单调量，于是**把整份文件换成一份旧快照**
 * （回放）不会被发现 —— PIN 锁定计数、已吊销的密钥、槽位状态全都跟着回到
 * 从前，而全文件 MAC 对旧快照**照样是对的**（那份 MAC 当初就是自己算的）。
 *
 * 这里每次成功写盘就把锚点推一格，把推完的值写进文件头、进 MAC 覆盖范围。
 * 装载时 epoch 比锚点小就**拒绝装载**（fail-closed，不是只报个警告）。
 *
 * **锚点放在哪儿由 pqchsm/rbanchor.h 的 provider 决定**，强度差一个量级：
 *
 *   file（默认）—— 旁边的另一个普通文件。⚠️ **这不是真正的防回滚**：
 *                   能写 SD 卡的攻击者两个文件一起换回去就绕过了。
 *                   真实收益只有"必须一致地换两个文件"和"事后查得出来"。
 *
 *   rpmb        —— eMMC 的 RPMB 写计数器。硬件维护、只增不减，**谁都没办法
 *                   让它变小**，包括拿到 RPMB 认证密钥的人。这一条成立时
 *                   回放在物理上不成立，而不只是"很难"。
 *                   这块板实测有 4 MB RPMB 且认证密钥未烧写（board/src/rpmb_probe.c），
 *                   所以这条路是通的；结论与取舍见 docs/SECURITY.md。
 *
 * pqc_rbanchor_is_hardware_monotonic() 会如实告诉上层现在是哪一种 ——
 * 别把 file 说成"有防回滚"。
 */

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

static void put_u64(uint8_t *p, uint64_t v)
{
	for (int i = 0; i < 8; i++) {
		p[i] = (uint8_t)(v >> (8 * i));
	}
}

static uint64_t get_u64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; i++) {
		v |= (uint64_t)p[i] << (8 * i);
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

static hsm_status_t keystore_save_impl(hsm_token_t *tok, const char *path,
                                       const char *audit_detail)
{
	if (!tok || !path) {
		return HSM_ERR_BAD_ARG;
	}
	size_t n_slots = hsm_token_slot_count(tok);
	size_t blob_cap = hsm_slot_blob_max();

	uint8_t salt[KS_SALT_LEN], kek[PQC_KEK_LEN], fkey[32];
	if (pqc_random_bytes(salt, sizeof(salt)) != 0) {
		return HSM_ERR_CRYPTO;
	}
	/* 每次落盘换一把新 KEK，等价于一次 KEK 轮换 */
	if (pqc_kek_derive(salt, sizeof(salt), kek) != 0 || derive_file_key(salt, fkey) != 0) {
		/* 派生可能在中途失败，缓冲里已经有半截密钥了 —— 不能就这么返回 */
		pqc_secure_zero(kek, sizeof(kek));
		pqc_secure_zero(fkey, sizeof(fkey));
		pqc_secure_zero(salt, sizeof(salt));
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
	/* epoch = 当前锚点 + 1。锚点在**文件落定之后**才真正推进（见下面 fsync_dir 之后）。
	 *
	 * ⚠️ 这个顺序不能反，而且我一度反过、被掉电用例当场抓住：
	 * 先推锚点再写文件的话，中间掉一次电就得到"锚点 N+1、文件还是 N"，
	 * 下一次装载被自己的防回滚判据拒掉 —— **一次掉电把好好的库变成打不开的库**。
	 * 现在这个顺序最坏是"文件 N+1、锚点还是 N"：装载照过（N+1 ≥ N），
	 * 下次写盘再把锚点推上去，什么都不丢。
	 *
	 * 这个顺序对 RPMB 锚点同样成立：读计数器拿到 C，文件里写 C+1，
	 * 然后那一次认证写把计数器推到 C+1。 */
	uint64_t epoch = 0;
	{
		uint64_t seen = 0;

		if (pqc_rbanchor_read(path, &seen) != 0) {
			st = HSM_ERR_CRYPTO;
			goto out;
		}
		epoch = seen + 1u;
	}
	put_u64(body + KS_OFF_EPOCH, epoch);
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

	/* 真正要抹掉的是算出这个标签的 fkey，在 out: 里做。
	 * 无需清零：tag 是随文件一起落盘的全文件 MAC，本身就是公开值 */
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
	 * 也让掉电模拟能落在文件中间的多个位置。 */
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
	/* 文件落定了才推锚点。推不动不算保存失败：keystore 本身已经在盘上、能用；
	 * 但要如实记一笔 —— 这段时间里防回滚保护是缺位的。 */
	{
		uint64_t got = 0;

		if (pqc_rbanchor_bump(path, &got) != 0 || got != epoch) {
			audit_detail = "keystore saved but rollback anchor did not advance";
		}
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
	pqc_secure_zero(kek, sizeof(kek));
	pqc_secure_zero(fkey, sizeof(fkey));
	pqc_secure_zero(salt, sizeof(salt));
	/* 每次 save 都换新 KEK，等价于一次 KEK 轮换 */
	slot_audit(tok, AUDIT_OP_KEK_ROTATE, HSM_ROLE_PUBLIC, 0, st, audit_detail);
	return st;
}

hsm_status_t hsm_keystore_save(hsm_token_t *tok, const char *path)
{
	return keystore_save_impl(tok, path, "keystore-save");
}

hsm_status_t hsm_keystore_rotate_kek(hsm_token_t *tok, const char *path)
{
	/* 轮换前先确认在内存里的 token 是自洽的：所有槽位元数据标签都验得过。
	 * 否则就是拿一个已经可疑的状态去覆盖盘上那份。 */
	for (size_t i = 0; i < hsm_token_slot_count(tok); i++) {
		slot_meta_t m;
		hsm_status_t st = hsm_slot_get_meta(tok, (hsm_slot_id_t)i, &m);
		if (st != HSM_OK) {
			return st;
		}
	}
	return keystore_save_impl(tok, path, "kek-rotate");
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
		/* 无需清零：want 是重算出来的全文件 MAC，与文件尾那份逐字节相同，是公开值 */
		uint8_t want[KS_TAG_LEN];
		if (pqc_kmac256(fkey, sizeof(fkey), buf, body_len, "pqc-hsm/keystore",
		                want, sizeof(want)) != 0) {
			st = HSM_ERR_CRYPTO;
			goto out;
		}
		if (!pqc_ct_equal(want, buf + body_len, KS_TAG_LEN)) {
			goto out;
		}

		/* ---- 防回滚：MAC 过了才谈 epoch ----
		 * 顺序要紧：epoch 在文件头里，而头是攻击者能改的；先验 MAC 才知道
		 * 这个数是我们自己写下的那个。反过来先信 epoch 等于信了未认证的数据。
		 *
		 * 判据是**严格小于就拒**（fail-closed）。相等是正常的：同一份文件
		 * 反复装载。大于也放行 —— 那说明锚点落后于文件（掉电窗口，或者把
		 * 同一份库拿到另一台机器上装），文件本身是新的，装载它并无风险。 */
		{
			uint64_t epoch = get_u64(buf + KS_OFF_EPOCH);
			uint64_t seen = 0;

			if (pqc_rbanchor_read(path, &seen) != 0) {
				/* 读不到锚点**不能**当成"锚点是 0"放行 —— 那等于把防回滚
				 * 的开关交到攻击者手上（把锚点弄坏即可）。 */
				st = HSM_ERR_INTEGRITY;
				goto out;
			}
			if (epoch < seen) {
				/* 这是一次**被检测到的回放**：文件比锚点旧。 */
				st = HSM_ERR_INTEGRITY;
				goto out;
			}
			/* ⚠️ epoch > seen 时**不把锚点往上推**。
			 * 老版本推了，理由是"免得下次又落后"。但那给了攻击者一条
			 * 抹证据的路：拿一份自己造的高 epoch 文件读一次，锚点就被顶上去，
			 * 真正的当前文件反而被判成回放。推进只由 save 做。 */
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
