/* append-only 审计日志（哈希链，）
 *
 * 文件格式、截断检测的原理、以及"纯哈希链防不住谁"的完整分析都写在
 * pqchsm/audit.h 里，这里只讲实现上的取舍。
 */
#include "pqchsm/audit.h"

#include "pqchsm/kdf.h"
#include "pqchsm/util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const uint8_t AUDIT_MAGIC[8] = { 'P', 'Q', 'C', 'H', 'S', 'M', 'A', 'L' };

#define AUDIT_VERSION  1u
#define AUDIT_HDR_LEN  64u                                  /* 文件头，就地更新 */
#define AUDIT_WIRE_LEN 64u                                  /* 一条记录的序列化字段 */
#define AUDIT_REC_LEN  (AUDIT_WIRE_LEN + AUDIT_HASH_LEN)    /* 96：wire ‖ H_i */

/* 文件头内的偏移 */
#define HDR_OFF_VERSION 8u
#define HDR_OFF_COUNT   16u
#define HDR_OFF_HEAD    24u

/* wire 内的偏移 */
#define W_OFF_SEQ    0u
#define W_OFF_TS     8u
#define W_OFF_OP     16u
#define W_OFF_ROLE   20u
#define W_OFF_SLOT   24u
#define W_OFF_RESULT 28u
#define W_OFF_DETAIL 32u

/* 创世串：21 字节 ASCII，不含结尾的 NUL（写死在这里，改它等于换一条链） */
static const char AUDIT_GENESIS_STR[] = "pqc-hsm/audit/genesis";

struct audit_log {
	int      fd;
	uint64_t count;
	uint8_t  head[AUDIT_HASH_LEN];
};

/* ---- 显式小端编解码 ------------------------------------------------------ */
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

/* ---- 定位读写（短读短写都当失败：长度全是已知常量，不该出现半截）-------- */
static int io_pwrite(int fd, const void *buf, size_t n, off_t off)
{
	const uint8_t *p = (const uint8_t *)buf;
	size_t done = 0;
	while (done < n) {
		ssize_t w = pwrite(fd, p + done, n - done, off + (off_t)done);
		if (w <= 0) {
			return -1;
		}
		done += (size_t)w;
	}
	return 0;
}

static int io_pread(int fd, void *buf, size_t n, off_t off)
{
	uint8_t *p = (uint8_t *)buf;
	size_t done = 0;
	while (done < n) {
		ssize_t r = pread(fd, p + done, n - done, off + (off_t)done);
		if (r <= 0) {
			return -1;
		}
		done += (size_t)r;
	}
	return 0;
}

static int file_len(int fd, uint64_t *out)
{
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		return -1;
	}
	*out = (uint64_t)st.st_size;
	return 0;
}

/* 与 keystore.c 同一个套路：rename 之后把目录项也刷下去 */
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

/* ---- 链与记录 ------------------------------------------------------------ */
static int genesis_hash(uint8_t out[AUDIT_HASH_LEN])
{
	return pqc_sha3_256((const uint8_t *)AUDIT_GENESIS_STR,
	                    sizeof(AUDIT_GENESIS_STR) - 1, out);
}

/* H_next = SHA3-256(prev ‖ wire) */
static int chain_step(const uint8_t prev[AUDIT_HASH_LEN], const uint8_t *wire,
                      uint8_t out[AUDIT_HASH_LEN])
{
	uint8_t buf[AUDIT_HASH_LEN + AUDIT_WIRE_LEN];
	memcpy(buf, prev, AUDIT_HASH_LEN);
	memcpy(buf + AUDIT_HASH_LEN, wire, AUDIT_WIRE_LEN);
	int rc = pqc_sha3_256(buf, sizeof(buf), out);
	/* buf 里按 红线不该有密钥材料，但 detail 是外部传进来的字符串，
	 * 用后即清是零成本的好习惯。 */
	pqc_secure_zero(buf, sizeof(buf));
	return rc;
}

static void hdr_encode(uint8_t hdr[AUDIT_HDR_LEN], uint64_t count,
                       const uint8_t head[AUDIT_HASH_LEN])
{
	memset(hdr, 0, AUDIT_HDR_LEN);
	memcpy(hdr, AUDIT_MAGIC, sizeof(AUDIT_MAGIC));
	put_u32(hdr + HDR_OFF_VERSION, AUDIT_VERSION);
	put_u64(hdr + HDR_OFF_COUNT, count);
	memcpy(hdr + HDR_OFF_HEAD, head, AUDIT_HASH_LEN);
}

static int hdr_ok(const uint8_t hdr[AUDIT_HDR_LEN])
{
	return memcmp(hdr, AUDIT_MAGIC, sizeof(AUDIT_MAGIC)) == 0 &&
	       get_u32(hdr + HDR_OFF_VERSION) == AUDIT_VERSION;
}

static void wire_encode(uint8_t *wire, uint64_t seq, uint64_t timestamp,
                        audit_op_t op, uint32_t role, uint32_t slot_id,
                        uint32_t result, const char *detail)
{
	memset(wire, 0, AUDIT_WIRE_LEN);
	put_u64(wire + W_OFF_SEQ, seq);
	put_u64(wire + W_OFF_TS, timestamp);
	put_u32(wire + W_OFF_OP, (uint32_t)op);
	put_u32(wire + W_OFF_ROLE, role);
	put_u32(wire + W_OFF_SLOT, slot_id);
	put_u32(wire + W_OFF_RESULT, result);
	if (detail) {
		/* 超长直接截断（不足补 0），非可打印字符换成 '.'：
		 * 审计日志要给人看，别让控制字符/ANSI 转义混进阅读器。 */
		for (size_t i = 0; i < AUDIT_DETAIL_LEN && detail[i] != '\0'; i++) {
			unsigned char c = (unsigned char)detail[i];
			wire[W_OFF_DETAIL + i] =
				(c >= 0x20 && c < 0x7f) ? (uint8_t)c : (uint8_t)'.';
		}
	}
}

/* ---- 打开 / 关闭 --------------------------------------------------------- */
/* 新建：tmp → fsync → rename → fsync(dir)，与 keystore.c 一致。
 * 审计日志本身是 append-only 的，只有"创世那一刻"用得上原子替换 —— 为的是
 * 外界永远看不到一个只写了一半的文件头。 */
static int create_genesis(const char *path)
{
	uint8_t hdr[AUDIT_HDR_LEN], head[AUDIT_HASH_LEN];
	char *tmp = NULL;
	int fd = -1;
	int rc = -1;
	size_t plen = strlen(path);

	if (genesis_hash(head) != 0) {
		goto out;
	}
	hdr_encode(hdr, 0, head);

	tmp = malloc(plen + 8);
	if (!tmp) {
		goto out;
	}
	snprintf(tmp, plen + 8, "%s.tmp", path);

	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) {
		goto out;
	}
	if (io_pwrite(fd, hdr, sizeof(hdr), 0) != 0) {
		goto out;
	}
	if (fsync(fd) != 0) {
		goto out;
	}
	close(fd);
	fd = -1;
	if (rename(tmp, path) != 0) {
		goto out;
	}
	fsync_dir(path);
	rc = 0;
out:
	if (fd >= 0) {
		close(fd);
	}
	if (rc != 0 && tmp) {
		(void)unlink(tmp);
	}
	free(tmp);
	return rc;
}

audit_log_t *audit_open(const char *path)
{
	audit_log_t *log = NULL;
	uint8_t hdr[AUDIT_HDR_LEN];
	uint64_t len = 0, count = 0;
	int fd = -1;

	if (!path || path[0] == '\0') {
		goto out;
	}
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		if (errno != ENOENT) {
			goto out;
		}
		if (create_genesis(path) != 0) {
			goto out;
		}
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			goto out;
		}
	}
	if (io_pread(fd, hdr, sizeof(hdr), 0) != 0 || !hdr_ok(hdr)) {
		goto out;
	}
	count = get_u64(hdr + HDR_OFF_COUNT);
	if (file_len(fd, &len) != 0) {
		goto out;
	}
	/* 打开时的廉价自洽检查：大小必须正好装得下 count 条。
	 * 完整的逐条重算交给 audit_verify_file —— 那是 O(N)，不该压在每次打开上。 */
	if (count > (UINT64_MAX - AUDIT_HDR_LEN) / AUDIT_REC_LEN) {
		goto out;
	}
	if (len != AUDIT_HDR_LEN + AUDIT_REC_LEN * count) {
		goto out;
	}

	log = calloc(1, sizeof(*log));
	if (!log) {
		goto out;
	}
	log->fd = fd;
	log->count = count;
	memcpy(log->head, hdr + HDR_OFF_HEAD, AUDIT_HASH_LEN);
	fd = -1;
out:
	if (fd >= 0) {
		close(fd);
	}
	return log;
}

void audit_close(audit_log_t *log)
{
	if (!log) {
		return;
	}
	if (log->fd >= 0) {
		close(log->fd);
	}
	free(log);
}

/* ---- 追加 ---------------------------------------------------------------- */
int audit_append(audit_log_t *log, uint64_t timestamp, audit_op_t op,
                 uint32_t role, uint32_t slot_id, uint32_t result,
                 const char *detail)
{
	uint8_t rec[AUDIT_REC_LEN], hdr[AUDIT_HDR_LEN], next[AUDIT_HASH_LEN];
	int rc = -1;

	if (!log || log->fd < 0) {
		goto out;
	}
	if (op < AUDIT_OP_INIT_TOKEN || op > AUDIT_OP_KEK_ROTATE) {
		goto out;
	}
	/* seq 是链的一部分，回绕会让两条记录同号 —— 宁可拒写 */
	if (log->count >= (UINT64_MAX - AUDIT_HDR_LEN) / AUDIT_REC_LEN) {
		goto out;
	}

	wire_encode(rec, log->count, timestamp, op, role, slot_id, result, detail);
	if (chain_step(log->head, rec, next) != 0) {
		goto out;
	}
	memcpy(rec + AUDIT_WIRE_LEN, next, AUDIT_HASH_LEN);

	/* 先记录、后更头：已经答应记下来的事优先留在盘上。
	 * 两次 fsync 之间掉电会留下一条文件头不认的记录，verify 会 fail-closed，
	 * 这是刻意的（audit.h 里有完整说明）。 */
	if (io_pwrite(log->fd, rec, sizeof(rec),
	              (off_t)(AUDIT_HDR_LEN + AUDIT_REC_LEN * log->count)) != 0) {
		goto out;
	}
	if (fsync(log->fd) != 0) {
		goto out;
	}
	hdr_encode(hdr, log->count + 1, next);
	if (io_pwrite(log->fd, hdr, sizeof(hdr), 0) != 0) {
		goto out;
	}
	if (fsync(log->fd) != 0) {
		goto out;
	}

	log->count++;
	memcpy(log->head, next, AUDIT_HASH_LEN);
	rc = 0;
out:
	pqc_secure_zero(rec, sizeof(rec));
	return rc;
}

int audit_head(const audit_log_t *log, uint8_t out[AUDIT_HASH_LEN])
{
	if (!log || !out) {
		return -1;
	}
	memcpy(out, log->head, AUDIT_HASH_LEN);
	return 0;
}

uint64_t audit_count(const audit_log_t *log)
{
	return log ? log->count : 0;
}

/* ---- 验证 ---------------------------------------------------------------- */
int audit_verify_file(const char *path, uint64_t *bad_seq)
{
	uint8_t hdr[AUDIT_HDR_LEN], rec[AUDIT_REC_LEN];
	/* 无需清零：审计链的哈希覆盖的是非敏感记录，且随文件与锚点一起对外发布 */
	uint8_t cur[AUDIT_HASH_LEN], want[AUDIT_HASH_LEN];
	uint64_t count = 0, len = 0, have = 0, bad = 0, seq = 0;
	int fd = -1;
	int rc = -1;

	if (!path) {
		goto out;
	}
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		goto out;
	}
	/* 文件头读不全或不认识 ⇒ 连锚点都没了，bad_seq 保持 0 */
	if (io_pread(fd, hdr, sizeof(hdr), 0) != 0 || !hdr_ok(hdr)) {
		goto out;
	}
	count = get_u64(hdr + HDR_OFF_COUNT);
	if (file_len(fd, &len) != 0 || len < AUDIT_HDR_LEN) {
		goto out;
	}

	/* 截断 / 尾部伪造追加 / 半条残记录，全在这一步落网 */
	have = (len - AUDIT_HDR_LEN) / AUDIT_REC_LEN;
	if (count > (UINT64_MAX - AUDIT_HDR_LEN) / AUDIT_REC_LEN ||
	    len != AUDIT_HDR_LEN + AUDIT_REC_LEN * count) {
		bad = (have < count) ? have : count;
		goto out;
	}

	if (genesis_hash(cur) != 0) {
		goto out;
	}
	for (seq = 0; seq < count; seq++) {
		bad = seq;
		if (io_pread(fd, rec, sizeof(rec),
		             (off_t)(AUDIT_HDR_LEN + AUDIT_REC_LEN * seq)) != 0) {
			goto out;
		}
		/* 序号必须与位置一致 —— 调换两条记录首先在这里露馅 */
		if (get_u64(rec + W_OFF_SEQ) != seq) {
			goto out;
		}
		if (chain_step(cur, rec, want) != 0) {
			goto out;
		}
		if (!pqc_ct_equal(want, rec + AUDIT_WIRE_LEN, AUDIT_HASH_LEN)) {
			goto out;
		}
		memcpy(cur, want, AUDIT_HASH_LEN);
	}
	/* 每条都自洽了，还要跟文件头里的锚点对上 */
	bad = count;
	if (!pqc_ct_equal(cur, hdr + HDR_OFF_HEAD, AUDIT_HASH_LEN)) {
		goto out;
	}
	rc = 0;
out:
	if (fd >= 0) {
		close(fd);
	}
	if (bad_seq) {
		*bad_seq = (rc == 0) ? 0 : bad;
	}
	return rc;
}

/* ---- 单条读取 ------------------------------------------------------------ */
int audit_hash_at(const char *path, uint64_t count, uint8_t out[AUDIT_HASH_LEN])
{
	uint8_t hdr[AUDIT_HDR_LEN], rec[AUDIT_REC_LEN];
	uint64_t have = 0, len = 0;
	int fd = -1;
	int rc = -1;

	if (!path || !out) {
		goto out;
	}
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		goto out;
	}
	if (io_pread(fd, hdr, sizeof(hdr), 0) != 0 || !hdr_ok(hdr)) {
		goto out;
	}
	if (file_len(fd, &len) != 0 || len < AUDIT_HDR_LEN) {
		goto out;
	}
	have = (len - AUDIT_HDR_LEN) / AUDIT_REC_LEN;
	if (count > have) {
		goto out;      /* 记录数不够 —— 日志被截短到锚点之下 */
	}
	if (count == 0) {
		rc = genesis_hash(out);
		goto out;
	}
	/* 第 count-1 条记录自带的 H_i 就是 H_count。
	 * 这里只取值不校验链 —— 链的完整性由 audit_verify_file 负责，
	 * 锚点校验会把两者一起用（见 anchor.c）。 */
	if (io_pread(fd, rec, sizeof(rec), (off_t)(AUDIT_HDR_LEN + (count - 1) * AUDIT_REC_LEN)) != 0) {
		goto out;
	}
	memcpy(out, rec + AUDIT_WIRE_LEN, AUDIT_HASH_LEN);
	rc = 0;
out:
	if (fd >= 0) {
		close(fd);
	}
	return rc;
}

int audit_read(const char *path, uint64_t seq, uint64_t *timestamp, uint32_t *op,
               uint32_t *role, uint32_t *slot_id, uint32_t *result,
               char detail[AUDIT_DETAIL_LEN + 1])
{
	uint8_t hdr[AUDIT_HDR_LEN], rec[AUDIT_REC_LEN];
	/* 无需清零：审计链的哈希覆盖的是非敏感记录，且随文件与锚点一起对外发布 */
	uint8_t prev[AUDIT_HASH_LEN], want[AUDIT_HASH_LEN];
	uint64_t count = 0;
	int fd = -1;
	int rc = -1;

	if (!path) {
		goto out;
	}
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		goto out;
	}
	if (io_pread(fd, hdr, sizeof(hdr), 0) != 0 || !hdr_ok(hdr)) {
		goto out;
	}
	count = get_u64(hdr + HDR_OFF_COUNT);
	if (seq >= count) {
		goto out;
	}
	if (io_pread(fd, rec, sizeof(rec),
	             (off_t)(AUDIT_HDR_LEN + AUDIT_REC_LEN * seq)) != 0) {
		goto out;
	}
	if (get_u64(rec + W_OFF_SEQ) != seq) {
		goto out;
	}
	/* 就近校验：拿前一条的哈希（或创世哈希）重算本条。
	 * 只是廉价的局部检查，整体完好性以 audit_verify_file 为准。 */
	if (seq == 0) {
		if (genesis_hash(prev) != 0) {
			goto out;
		}
	} else if (io_pread(fd, prev, sizeof(prev),
	                    (off_t)(AUDIT_HDR_LEN + AUDIT_REC_LEN * (seq - 1) +
	                            AUDIT_WIRE_LEN)) != 0) {
		goto out;
	}
	if (chain_step(prev, rec, want) != 0) {
		goto out;
	}
	if (!pqc_ct_equal(want, rec + AUDIT_WIRE_LEN, AUDIT_HASH_LEN)) {
		goto out;
	}

	if (timestamp) {
		*timestamp = get_u64(rec + W_OFF_TS);
	}
	if (op) {
		*op = get_u32(rec + W_OFF_OP);
	}
	if (role) {
		*role = get_u32(rec + W_OFF_ROLE);
	}
	if (slot_id) {
		*slot_id = get_u32(rec + W_OFF_SLOT);
	}
	if (result) {
		*result = get_u32(rec + W_OFF_RESULT);
	}
	if (detail) {
		memcpy(detail, rec + W_OFF_DETAIL, AUDIT_DETAIL_LEN);
		detail[AUDIT_DETAIL_LEN] = '\0';
	}
	rc = 0;
out:
	if (fd >= 0) {
		close(fd);
	}
	pqc_secure_zero(rec, sizeof(rec));
	return rc;
}
