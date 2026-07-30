/* zeroize 的结构性验证
 *
 * 这个测试**直接窥视槽位的私有结构**（include slot_internal.h）：清零到底有
 * 没有落到内存上，从公共 API 是看不出来的。能写出这类断言，本身就是这一层
 * 选 C 而不是托管语言的理由 —— 托管语言里字节序列不可变、副本到处都是，
 * 根本无从断言"那块内存现在是什么"。
 *
 * 五组检查：
 *   1. 结构体覆盖   槽位清零后，除了少数非秘密字段，整个结构体逐字节为 0；
 *                   写成"挖掉已知非秘密字段，剩下的必须全 0"，这样将来新增
 *                   字段而忘了清零会在这里当场暴露，而不是逐点断言漏掉它
 *   2. 状态无关     从任意状态进入清零，结果都一样干净
 *   3. 落盘无明文   密钥库文件与备份文件里搜不到私钥 / PIN 密钥 / 验证值
 *   4. 不被优化掉   在 -O2 下确认 pqc_secure_zero 那条存储没有被死存储消除，
 *                   带反证：同一套探针必须能抓到"根本没清"的情形
 *   5. 释放后无残留 pqc_secure_free 归还的内存里搜不到原内容，同样带反证
 *
 * 界限（诚实说明）：本测试能证明的是进程地址空间内的残留。它**不能**证明
 * CPU 寄存器、已换出的页、swap 分区或掉电后的 DRAM 里没有残留 —— 那需要板子
 * 和物理手段，不属于软件阶段。
 */
#include "testlib.h"
#include "pqchsm/backup.h"
#include "pqchsm/keystore.h"
#include "pqchsm/slot.h"
#include "pqchsm/util.h"

#include "../../src/slot/slot_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#define SO_PIN   "so-secret-0001"
#define USER_PIN "user-pin-4242"

static int mem_contains(const void *hay, size_t hn, const void *needle, size_t nn)
{
	if (nn == 0 || hn < nn) {
		return 0;
	}
	const uint8_t *h = (const uint8_t *)hay;
	for (size_t i = 0; i + nn <= hn; i++) {
		if (memcmp(h + i, needle, nn) == 0) {
			return 1;
		}
	}
	return 0;
}

static int all_zero(const void *p, size_t n)
{
	const uint8_t *b = (const uint8_t *)p;
	for (size_t i = 0; i < n; i++) {
		if (b[i]) {
			return 0;
		}
	}
	return 1;
}

static uint8_t *slurp(const char *p, size_t *n)
{
	FILE *f = fopen(p, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	rewind(f);
	uint8_t *b = malloc((size_t)sz);
	if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
		free(b);
		b = NULL;
	}
	fclose(f);
	*n = (size_t)sz;
	return b;
}

/* 建一个装了密钥的槽位；把私钥前 48 字节抄一份出来供后续搜索 */
static hsm_token_t *fixture(uint8_t *sk_probe, size_t probe_len, uint32_t policy)
{
	hsm_token_t *tok = hsm_token_new(2);
	if (!tok) {
		return NULL;
	}
	hsm_session_t s;
	hsm_handle_t h;
	if (hsm_slot_init_token(tok, 0, "victim", SO_PIN) != HSM_OK ||
	    hsm_session_open(tok, 0, &s) != HSM_OK ||
	    hsm_session_login(tok, s, HSM_ROLE_SO, SO_PIN) != HSM_OK ||
	    hsm_slot_set_user_pin(tok, s, USER_PIN) != HSM_OK ||
	    hsm_session_logout(tok, s) != HSM_OK ||
	    hsm_session_login(tok, s, HSM_ROLE_USER, USER_PIN) != HSM_OK ||
	    hsm_slot_generate(tok, s, PQC_ALG_ML_DSA_65, KEY_USAGE_SIGN,
	                      policy | SLOT_POLICY_BACKUPABLE, &h) != HSM_OK) {
		hsm_token_free(tok);
		return NULL;
	}
	hsm_session_close(tok, s);

	slot_t *slot = &tok->slots[0];
	if (policy & SLOT_POLICY_SEED_STORAGE) {
		memcpy(sk_probe, slot->seed, probe_len);
	} else {
		memcpy(sk_probe, slot->sk, probe_len);
	}
	return tok;
}

static void test_struct_wiped(void)
{
	TCASE("zeroize 后槽位结构体里的秘密字段全为 0");
	uint8_t probe[48];
	hsm_token_t *tok = fixture(probe, sizeof(probe), 0);
	CHECK(tok != NULL);
	slot_t *s = &tok->slots[0];

	/* 清零前：确实有东西 */
	CHECK(s->sk != NULL);
	CHECK(!all_zero(s->pin_key, sizeof(s->pin_key)));
	CHECK(!all_zero(s->so_verifier, sizeof(s->so_verifier)));
	CHECK(!all_zero(probe, sizeof(probe)));

	CHECK_EQ_INT(hsm_slot_zeroize_forced(tok, 0), HSM_OK);

	/* 清零后：指针归零、内联秘密字段全 0 */
	CHECK(s->sk == NULL);
	CHECK(s->pk == NULL);
	CHECK_EQ_INT(s->sk_len, 0);
	CHECK_EQ_INT(s->pk_len, 0);
	CHECK_EQ_INT(s->has_seed, 0);
	CHECK_EQ_INT(s->seed_len, 0);
	CHECK(all_zero(s->seed, sizeof(s->seed)));
	CHECK(all_zero(s->pin_key, sizeof(s->pin_key)));
	CHECK(all_zero(s->so_salt, sizeof(s->so_salt)));
	CHECK(all_zero(s->so_verifier, sizeof(s->so_verifier)));
	CHECK(all_zero(s->user_salt, sizeof(s->user_salt)));
	CHECK(all_zero(s->user_verifier, sizeof(s->user_verifier)));
	CHECK_EQ_INT(s->has_so_pin, 0);
	CHECK_EQ_INT(s->has_user_pin, 0);
	/* 整个结构体里搜不到原私钥的任何片段 */
	CHECK_EQ_INT(mem_contains(s, sizeof(*s), probe, sizeof(probe)), 0);
	/* 元数据也清了（除了 slot_id / generation / version 这些非秘密字段） */
	CHECK_EQ_INT(s->meta.alg, PQC_ALG_NONE);
	CHECK_EQ_INT(s->meta.usage, 0);
	CHECK_EQ_INT(s->meta.use_count, 0);
	CHECK(all_zero(s->meta.label, SLOT_LABEL_MAX));

	hsm_token_free(tok);

	TCASE("种子存储槽位：zeroize 后种子也搜不到");
	uint8_t seed_probe[48];
	hsm_token_t *t2 = fixture(seed_probe, sizeof(seed_probe), SLOT_POLICY_SEED_STORAGE);
	CHECK(t2 != NULL);
	slot_t *s2 = &t2->slots[0];
	CHECK_EQ_INT(s2->has_seed, 1);
	CHECK_EQ_INT(hsm_slot_zeroize_forced(t2, 0), HSM_OK);
	CHECK(all_zero(s2->seed, sizeof(s2->seed)));
	CHECK_EQ_INT(mem_contains(s2, sizeof(*s2), seed_probe, sizeof(seed_probe)), 0);
	hsm_token_free(t2);
}

static void test_zeroize_from_every_state(void)
{
	TCASE("zeroize 从任意状态可达，且之后结构体都是干净的");
	for (int st = SLOT_ST_UNINIT; st < SLOT_ST__COUNT; st++) {
		uint8_t probe[48];
		hsm_token_t *tok = fixture(probe, sizeof(probe), 0);
		CHECK(tok != NULL);
		CHECK_EQ_INT(hsm_slot_force_state(tok, 0, (slot_state_t)st), HSM_OK);
		CHECK_EQ_INT(hsm_slot_zeroize_forced(tok, 0), HSM_OK);
		slot_t *s = &tok->slots[0];
		CHECK(s->sk == NULL);
		CHECK(all_zero(s->pin_key, sizeof(s->pin_key)));
		CHECK_EQ_INT(mem_contains(s, sizeof(*s), probe, sizeof(probe)), 0);
		slot_state_t got;
		CHECK_EQ_INT(hsm_slot_get_state(tok, 0, &got), HSM_OK);
		CHECK_EQ_INT(got, SLOT_ST_UNINIT);
		hsm_token_free(tok);
	}
}

static void test_no_plaintext_on_disk(void)
{
	char ks[128], bk[128];
	snprintf(ks, sizeof(ks), "/tmp/pqchsm_z_ks_%d.bin", (int)getpid());
	snprintf(bk, sizeof(bk), "/tmp/pqchsm_z_bk_%d.bin", (int)getpid());

	TCASE("红线：密钥库文件里搜不到明文私钥 / PIN 密钥 / 验证值");
	uint8_t probe[48];
	hsm_token_t *tok = fixture(probe, sizeof(probe), 0);
	CHECK(tok != NULL);
	slot_t *s = &tok->slots[0];
	uint8_t pin_key_copy[32], so_ver_copy[32];
	memcpy(pin_key_copy, s->pin_key, sizeof(pin_key_copy));
	memcpy(so_ver_copy, s->so_verifier, sizeof(so_ver_copy));

	CHECK_EQ_INT(hsm_keystore_save(tok, ks), HSM_OK);
	{
		size_t n = 0;
		uint8_t *f = slurp(ks, &n);
		CHECK(f != NULL);
		CHECK(n > 0);
		CHECK_EQ_INT(mem_contains(f, n, probe, sizeof(probe)), 0);
		CHECK_EQ_INT(mem_contains(f, n, pin_key_copy, sizeof(pin_key_copy)), 0);
		CHECK_EQ_INT(mem_contains(f, n, so_ver_copy, sizeof(so_ver_copy)), 0);
		/* 公钥也在包裹内，同样搜不到 */
		CHECK_EQ_INT(mem_contains(f, n, s->pk, 48), 0);
		free(f);
	}

	TCASE("红线：备份文件里同样搜不到");
	{
		hsm_session_t so;
		CHECK_EQ_INT(hsm_session_open(tok, 0, &so), HSM_OK);
		CHECK_EQ_INT(hsm_session_login(tok, so, HSM_ROLE_SO, SO_PIN), HSM_OK);
		uint8_t shares[5 * HSM_SHARE_CAP];
		size_t lens[5];
		CHECK_EQ_INT(hsm_backup_export(tok, so, bk, 3, 5, shares, HSM_SHARE_CAP, lens, NULL),
		             HSM_OK);
		size_t n = 0;
		uint8_t *f = slurp(bk, &n);
		CHECK(f != NULL);
		CHECK_EQ_INT(mem_contains(f, n, probe, sizeof(probe)), 0);
		CHECK_EQ_INT(mem_contains(f, n, pin_key_copy, sizeof(pin_key_copy)), 0);
		free(f);
		/* 分片里也不能直接出现 RMK 派生物之外的东西 —— 至少不能有私钥 */
		CHECK_EQ_INT(mem_contains(shares, sizeof(shares), probe, sizeof(probe)), 0);
		hsm_session_close(tok, so);
	}

	TCASE("清零后重新落盘：文件里彻底没有旧密钥");
	CHECK_EQ_INT(hsm_slot_zeroize_forced(tok, 0), HSM_OK);
	CHECK_EQ_INT(hsm_keystore_save(tok, ks), HSM_OK);
	{
		size_t n = 0;
		uint8_t *f = slurp(ks, &n);
		CHECK(f != NULL);
		CHECK_EQ_INT(mem_contains(f, n, probe, sizeof(probe)), 0);
		free(f);
		/* 重新装载得到的是一个 UNINIT 槽位，不是旧密钥 */
		hsm_token_t *t2 = hsm_token_new(2);
		CHECK_EQ_INT(hsm_keystore_load(t2, ks), HSM_OK);
		slot_meta_t m;
		CHECK_EQ_INT(hsm_slot_get_meta(t2, 0, &m), HSM_OK);
		CHECK_EQ_INT(m.state, SLOT_ST_UNINIT);
		CHECK_EQ_INT(m.alg, PQC_ALG_NONE);
		CHECK(all_zero(t2->slots[0].pin_key, 32));
		CHECK_EQ_INT(mem_contains(&t2->slots[0], sizeof(slot_t), probe, sizeof(probe)), 0);
		hsm_token_free(t2);
	}

	hsm_token_free(tok);
	unlink(ks);
	unlink(bk);
}

static void test_secure_alloc(void)
{
	TCASE("pqc_secure_alloc 返回清零内存；secure_zero 真的写下去了");
	uint8_t *p = pqc_secure_alloc(4096);
	CHECK(p != NULL);
	CHECK(all_zero(p, 4096));
	memset(p, 0xA5, 4096);
	CHECK(!all_zero(p, 4096));
	pqc_secure_zero(p, 4096);
	CHECK(all_zero(p, 4096));
	pqc_secure_free(p, 4096);
	CHECK(pqc_secure_alloc(0) == NULL);
	pqc_secure_free(NULL, 0);   /* 不应崩溃 */
}

/* ---- 结构性覆盖：清零后整个结构体只剩非秘密字段 ------------------------- */

/* 清零之后仍然允许非 0 的字段。除此之外的每一个字节都必须是 0 ——
 * 包括将来新增的字段，这正是这条检查与逐点断言的区别。 */
static void punch_public_fields(uint8_t *shadow, const slot_t *s)
{
	const uint8_t *base = (const uint8_t *)s;
	struct { const void *at; size_t n; } keep[] = {
		{ &s->lock,     sizeof(s->lock) },      /* 互斥量：运行期状态 */
		{ &s->meta,     sizeof(s->meta) },      /* 元数据：清零后复位并重新盖章 */
		{ &s->meta_tag, sizeof(s->meta_tag) },  /* 元数据标签：随之重算 */
		{ &s->pre_lock, sizeof(s->pre_lock) },  /* 解锁目标状态：非秘密 */
	};
	for (size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
		size_t off = (size_t)((const uint8_t *)keep[i].at - base);
		memset(shadow + off, 0, keep[i].n);
	}
}

static void test_struct_fully_wiped(void)
{
	TCASE("结构性：清零后除已知非秘密字段外，slot_t 逐字节为 0");
	uint8_t probe[48];
	hsm_token_t *tok = fixture(probe, sizeof(probe), 0);
	CHECK(tok != NULL);
	slot_t *s = &tok->slots[0];

	uint8_t *shadow = malloc(sizeof(slot_t));
	CHECK(shadow != NULL);

	/* 先确认这条检查在"没清零"时确实会响 —— 否则清零后的全 0 说明不了任何事 */
	memcpy(shadow, s, sizeof(slot_t));
	punch_public_fields(shadow, s);
	CHECK(!all_zero(shadow, sizeof(slot_t)));

	CHECK_EQ_INT(hsm_slot_zeroize_forced(tok, 0), HSM_OK);

	memcpy(shadow, s, sizeof(slot_t));
	punch_public_fields(shadow, s);
	CHECK(all_zero(shadow, sizeof(slot_t)));

	free(shadow);
	hsm_token_free(tok);
}

/* ---- 不被优化掉：-O2 下的死存储消除探针 --------------------------------- */

/* 哨兵：16 字节，随机内容不太可能撞上 */
static const uint8_t SENTINEL[16] = {
	0x9e, 0x37, 0x79, 0xb9, 0x7f, 0x4a, 0x7c, 0x15,
	0xf3, 0x9c, 0xc0, 0x60, 0x5c, 0xed, 0xc8, 0x34,
};

#define FRAME_LEN 512

enum { WIPE_NONE, WIPE_MEMSET, WIPE_SECURE };

/* volatile：编译器不能假设这个指针之后没人读，也不能把它优化掉 */
static uint8_t *volatile g_frame;

/* 在自己的栈帧里放满哨兵，按 mode 决定怎么"清零"，然后把那段栈的地址留出来。
 * 返回后这个帧就死了 —— 里面还剩什么，正是这条检查要看的。 */
__attribute__((noinline))
static void leave_sentinel_on_stack(int mode)
{
	uint8_t buf[FRAME_LEN];
	for (size_t i = 0; i + sizeof(SENTINEL) <= FRAME_LEN; i += sizeof(SENTINEL)) {
		memcpy(buf + i, SENTINEL, sizeof(SENTINEL));
	}
	/* 把一个即将失效的栈地址留出来正是本检查的手段，不是笔误。
	 * GCC 12 起会就此告警，这里定点关掉。 */
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 12
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
#endif
	g_frame = buf;
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 12
#pragma GCC diagnostic pop
#endif
	if (mode == WIPE_SECURE) {
		pqc_secure_zero(buf, sizeof(buf));
	} else if (mode == WIPE_MEMSET) {
		memset(buf, 0, sizeof(buf));
	}
	/* buf 之后不再被读取。编译器有充分理由消除上面那条清零 ——
	 * pqc_secure_zero 不能被消除，普通 memset 可以。 */
}

/* 把刚死掉的那个帧原样抄一份。抄的过程必须在调用方自己的帧里内联完成：
 * 中途只要再调用一个函数，新帧就会盖在要检查的那段栈上。 */
#define SNAPSHOT_DEAD_FRAME(dst)                                            \
	do {                                                                \
		volatile uint8_t *_src = g_frame;                           \
		for (size_t _i = 0; _i < FRAME_LEN; _i++) {                 \
			(dst)[_i] = _src[_i];                               \
		}                                                           \
	} while (0)

static void test_secure_zero_not_elided(void)
{
	uint8_t after_none[FRAME_LEN], after_memset[FRAME_LEN], after_secure[FRAME_LEN];

	leave_sentinel_on_stack(WIPE_NONE);
	SNAPSHOT_DEAD_FRAME(after_none);
	leave_sentinel_on_stack(WIPE_MEMSET);
	SNAPSHOT_DEAD_FRAME(after_memset);
	leave_sentinel_on_stack(WIPE_SECURE);
	SNAPSHOT_DEAD_FRAME(after_secure);

	int hits_none = mem_contains(after_none, FRAME_LEN, SENTINEL, sizeof(SENTINEL));
	int hits_memset = mem_contains(after_memset, FRAME_LEN, SENTINEL, sizeof(SENTINEL));
	int hits_secure = mem_contains(after_secure, FRAME_LEN, SENTINEL, sizeof(SENTINEL));

	/* 反证：完全不清零时探针必须看得见哨兵。看不见就说明探针根本没读到那段栈，
	 * 后面"清零后看不见"这个结论也就一文不值。 */
	TCASE("反证：不清零时栈上残留必须被探针抓到");
	CHECK(hits_none == 1);

	TCASE("pqc_secure_zero 的清零不会被死存储消除（本 TU 编在 -O2）");
	CHECK(hits_secure == 0);

	/* 普通 memset 到一个即将失效的栈缓冲是一条死存储，编译器有权消除它。
	 * 消不消除取决于编译器与版本，所以这里只如实报告观察结果，不作为判据 ——
	 * 判据是上面那条反证。 */
	printf("    观察：普通 memset 那条清零%s（栈上%s哨兵）\n",
	       hits_memset ? "被编译器消除了" : "未被消除",
	       hits_memset ? "仍能找到" : "已找不到");
}

/* ---- 释放后无残留 -------------------------------------------------------- */
#define POOL_N   32
#define BLOCK_N  1024
/* 哨兵写在偏移 64 处：分配器会把空闲链表指针写进块首那几个字节 */
#define SENT_AT  64
#define SENT_LEN 16

/* 两次观察各用一个哨兵，理由见 residue_visible_after_free 的说明 */
static const uint8_t SENT_CONTROL[SENT_LEN] = {
	0xA5, 0x5A, 0xC3, 0x3C, 0x96, 0x69, 0x0F, 0xF0,
	0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
};
static const uint8_t SENT_SECURE[SENT_LEN] = {
	0x5A, 0xA5, 0x3C, 0xC3, 0x69, 0x96, 0xF0, 0x0F,
	0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00,
};

/* 申请一批块、写入哨兵、全部释放，再用**普通 malloc** 申请同样一批，
 * 看哨兵还在不在。两种分配器走的是同一套访问模式，唯一的差别就是释放路径，
 * 所以两次观察可比。
 *
 * 不按"释放前后地址是否相同"来判断：小块分配器会在释放时与相邻空闲块合并再
 * 重新切分，地址对不上是常态，那样判会把结论建在分配器实现细节上。
 *
 * 【每次观察用各自的哨兵】反证与被测两次调用共用一个堆。反证跑在前面，
 * 它留下的、含哨兵的空闲块可能正好被后一次的 malloc 拿到 —— 那样被测这一次
 * 会"看到哨兵"，而残留其实来自反证而不是 pqc_secure_free。哨兵取值不同就
 * 不存在这种混淆。 */
static int residue_visible_after_free(int secure, const uint8_t *sentinel)
{
	uint8_t *blocks[POOL_N];
	for (int i = 0; i < POOL_N; i++) {
		blocks[i] = secure ? pqc_secure_alloc(BLOCK_N) : malloc(BLOCK_N);
		if (!blocks[i]) {
			return -1;
		}
		memcpy(blocks[i] + SENT_AT, sentinel, SENT_LEN);
		if (!mem_contains(blocks[i], BLOCK_N, sentinel, SENT_LEN)) {
			return -1;      /* 哨兵没写进去，后面的观察无从谈起 */
		}
	}
	for (int i = 0; i < POOL_N; i++) {
		if (secure) {
			pqc_secure_free(blocks[i], BLOCK_N);
		} else {
			free(blocks[i]);
		}
	}

	int found = 0;
	uint8_t *again[POOL_N];
	for (int i = 0; i < POOL_N; i++) {
		again[i] = malloc(BLOCK_N);
		if (again[i] && mem_contains(again[i], BLOCK_N, sentinel, SENT_LEN)) {
			found = 1;
		}
	}
	for (int i = 0; i < POOL_N; i++) {
		free(again[i]);
	}
	return found;
}

static void test_no_residue_after_free(void)
{
	/* 反证：普通 malloc/free 之后，原内容必须还能在归还的内存里找到。
	 * 找不到就说明分配器在释放时自己就把块内容清了（macOS 的 libmalloc、
	 * ASan 的隔离区都是这样），这套观察手段在当前平台没有分辨力；
	 * 此时下面那条结论也无从建立，如实跳过而不是记成通过。 */
	int control = residue_visible_after_free(0, SENT_CONTROL);
	CHECK(control >= 0);
	if (control != 1) {
		printf("    分配器在释放时即清除块内容，观察手段无分辨力，"
		       "跳过释放后残留观察\n");
		return;
	}

	TCASE("反证：普通 malloc/free 之后原内容仍能在归还的内存里找到");
	CHECK_EQ_INT(control, 1);

	TCASE("pqc_secure_free 释放的内存里搜不到原内容");
	CHECK_EQ_INT(residue_visible_after_free(1, SENT_SECURE), 0);
}

int main(void)
{
	test_struct_wiped();
	test_struct_fully_wiped();
	test_zeroize_from_every_state();
	test_no_plaintext_on_disk();
	test_secure_alloc();
	test_secure_zero_not_elided();
	test_no_residue_after_free();
	return test_report("test_zeroize");
}
