/* 槽位元数据的 KMAC 完整性标签
 *
 * 要防的具体攻击：攻击者离线改密钥库文件，把"不可导出"改成"可导出"、
 * 把用途从 decap 改成 sign、或把 slot 3 的记录整条搬到 slot 5。
 * 这些都必须在装载时被检出。
 */
#include "testlib.h"
#include "pqchsm/slot.h"
#include "pqchsm/kdr.h"
#include "pqchsm/util.h"

#include "../../src/slot/meta.h"

static slot_meta_t base_meta(void)
{
	slot_meta_t m;
	memset(&m, 0, sizeof(m));
	m.version    = SLOT_META_VERSION;
	m.slot_id    = 3;
	m.alg        = PQC_ALG_ML_DSA_65;
	m.usage      = KEY_USAGE_SIGN;
	m.policy     = SLOT_POLICY_BACKUPABLE;      /* 注意：不含 EXTRACTABLE */
	m.state      = SLOT_ST_LOADED;
	m.use_count  = 7;
	/* 非零起点，否则下面"把失败计数改成 0"的篡改测试等于什么都没改 */
	m.user_pin_fails = 2;
	m.created_at = 1700000000;
	m.generation = 2;
	strncpy(m.label, "signing-key", SLOT_LABEL_MAX - 1);
	return m;
}

int main(void)
{
	uint8_t tag[SLOT_META_TAG_LEN], tag2[SLOT_META_TAG_LEN];

	TCASE("序列化是确定性的、定长的");
	slot_meta_t m = base_meta();
	uint8_t w1[256], w2[256];
	long n1 = slot_meta_serialize(&m, w1, sizeof(w1));
	long n2 = slot_meta_serialize(&m, w2, sizeof(w2));
	CHECK(n1 > 0);
	CHECK_EQ_INT(n1, n2);
	CHECK_EQ_MEM(w1, w2, (size_t)n1);
	CHECK_EQ_INT(slot_meta_serialize(&m, w1, 4), -1);       /* 缓冲不足 */
	CHECK_EQ_INT(slot_meta_serialize(NULL, w1, sizeof(w1)), -1);

	TCASE("盖标签与校验");
	CHECK_EQ_INT(slot_meta_seal(&m, tag), 0);
	CHECK_EQ_INT(slot_meta_verify(&m, tag), 0);

	/* ---- 逐字段篡改，每一项都必须被检出 ---- */
	TCASE("篡改策略位：不可导出 → 可导出");
	slot_meta_t t = m;
	t.policy |= SLOT_POLICY_EXTRACTABLE;
	CHECK_EQ_INT(slot_meta_verify(&t, tag), -1);

	TCASE("篡改用途位：decap → sign");
	t = m;
	t.usage = KEY_USAGE_VERIFY;
	CHECK_EQ_INT(slot_meta_verify(&t, tag), -1);

	TCASE("篡改状态：锁定 → 已装载");
	t = m;
	t.state = SLOT_ST_EMPTY;
	CHECK_EQ_INT(slot_meta_verify(&t, tag), -1);

	TCASE("篡改 PIN 失败计数（绕过锁定的经典手法）");
	t = m;
	t.user_pin_fails = 0;
	CHECK_EQ_INT(slot_meta_verify(&t, tag), -1);
	t = m;
	t.so_pin_fails = 99;
	CHECK_EQ_INT(slot_meta_verify(&t, tag), -1);

	TCASE("篡改算法、标签、计数器、时间戳、代数");
	t = m; t.alg = PQC_ALG_ML_DSA_44;   CHECK_EQ_INT(slot_meta_verify(&t, tag), -1);
	t = m; t.label[0] = 'X';            CHECK_EQ_INT(slot_meta_verify(&t, tag), -1);
	t = m; t.use_count = 8;             CHECK_EQ_INT(slot_meta_verify(&t, tag), -1);
	t = m; t.created_at += 1;           CHECK_EQ_INT(slot_meta_verify(&t, tag), -1);
	t = m; t.last_used_at += 1;         CHECK_EQ_INT(slot_meta_verify(&t, tag), -1);
	t = m; t.generation = 3;            CHECK_EQ_INT(slot_meta_verify(&t, tag), -1);
	t = m; t.version = 2;               CHECK_EQ_INT(slot_meta_verify(&t, tag), -1);

	TCASE("整条记录搬到别的槽位也必须被检出（标签绑定 slot_id）");
	t = m;
	t.slot_id = 5;
	CHECK_EQ_INT(slot_meta_verify(&t, tag), -1);
	/* 而且换了槽位号后重新盖的标签与原标签不同 */
	CHECK_EQ_INT(slot_meta_seal(&t, tag2), 0);
	CHECK(memcmp(tag, tag2, SLOT_META_TAG_LEN) != 0);

	TCASE("篡改标签本身");
	memcpy(tag2, tag, SLOT_META_TAG_LEN);
	tag2[0] ^= 0x01;
	CHECK_EQ_INT(slot_meta_verify(&m, tag2), -1);
	tag2[0] ^= 0x01;
	tag2[SLOT_META_TAG_LEN - 1] ^= 0x80;
	CHECK_EQ_INT(slot_meta_verify(&m, tag2), -1);

	TCASE("设备绑定：换一台设备的 KDR，旧标签必须失效");
	CHECK_EQ_INT(slot_meta_verify(&m, tag), 0);
	pqc_kdr_set_test_root((const uint8_t *)"another device", 14);
	CHECK_EQ_INT(slot_meta_verify(&m, tag), -1);
	/* 换回来就又能过 —— 说明失效来自 KDR 而不是别的副作用 */
	pqc_kdr_set_test_root(NULL, 0);
	CHECK_EQ_INT(slot_meta_verify(&m, tag), 0);

	TCASE("非法参数");
	CHECK_EQ_INT(slot_meta_seal(NULL, tag), -1);
	CHECK_EQ_INT(slot_meta_seal(&m, NULL), -1);
	CHECK_EQ_INT(slot_meta_verify(NULL, tag), -1);
	CHECK_EQ_INT(slot_meta_verify(&m, NULL), -1);

	return test_report("test_slot_meta");
}
