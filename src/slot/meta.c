#include "meta.h"

#include "pqchsm/kdf.h"
#include "pqchsm/kdr.h"
#include "pqchsm/util.h"

#include <string.h>

/* 元数据的序列化长度：固定，方便栈上缓冲 */
#define META_WIRE_LEN SLOT_META_WIRE_LEN
_Static_assert(SLOT_META_WIRE_LEN == 4 + 4 + SLOT_LABEL_MAX + 4 + 4 + 4 + 4 + 8 + 4 + 4 + 8 + 8 + 4,
               "SLOT_META_WIRE_LEN 与字段之和不符");

static void put_u32(uint8_t **p, uint32_t v)
{
	(*p)[0] = (uint8_t)(v);
	(*p)[1] = (uint8_t)(v >> 8);
	(*p)[2] = (uint8_t)(v >> 16);
	(*p)[3] = (uint8_t)(v >> 24);
	*p += 4;
}

static void put_u64(uint8_t **p, uint64_t v)
{
	for (int i = 0; i < 8; i++) {
		(*p)[i] = (uint8_t)(v >> (8 * i));
	}
	*p += 8;
}

long slot_meta_serialize(const slot_meta_t *m, uint8_t *out, size_t cap)
{
	if (!m || !out || cap < META_WIRE_LEN) {
		return -1;
	}
	uint8_t *p = out;
	put_u32(&p, m->version);
	put_u32(&p, m->slot_id);
	memcpy(p, m->label, SLOT_LABEL_MAX);
	p += SLOT_LABEL_MAX;
	put_u32(&p, (uint32_t)m->alg);
	put_u32(&p, m->usage);
	put_u32(&p, m->policy);
	put_u32(&p, (uint32_t)m->state);
	put_u64(&p, m->use_count);
	put_u32(&p, m->so_pin_fails);
	put_u32(&p, m->user_pin_fails);
	put_u64(&p, m->created_at);
	put_u64(&p, m->last_used_at);
	put_u32(&p, m->generation);
	return (long)(p - out);
}

/* 每个槽位一把独立的元数据密钥，salt = slot_id（小端 4 字节） */
static int derive_meta_key(uint32_t slot_id, uint8_t key[32])
{
	uint8_t salt[4];
	uint8_t *p = salt;
	put_u32(&p, slot_id);
	return pqc_kdr_derive("pqc-hsm/slot-meta-key", salt, sizeof(salt), key, 32);
}

int slot_meta_seal(const slot_meta_t *m, uint8_t tag[SLOT_META_TAG_LEN])
{
	if (!m || !tag) {
		return -1;
	}
	uint8_t wire[META_WIRE_LEN];
	long n = slot_meta_serialize(m, wire, sizeof(wire));
	if (n < 0) {
		return -1;
	}
	uint8_t key[32];
	int rc = -1;
	if (derive_meta_key(m->slot_id, key) != 0) {
		goto out;
	}
	rc = pqc_kmac256(key, sizeof(key), wire, (size_t)n,
	                 "pqc-hsm/slot-meta", tag, SLOT_META_TAG_LEN);
out:
	pqc_secure_zero(key, sizeof(key));
	pqc_secure_zero(wire, sizeof(wire));
	return rc;
}

int slot_meta_verify(const slot_meta_t *m, const uint8_t tag[SLOT_META_TAG_LEN])
{
	uint8_t want[SLOT_META_TAG_LEN];
	if (!m || !tag || slot_meta_seal(m, want) != 0) {
		return -1;
	}
	int ok = pqc_ct_equal(want, tag, SLOT_META_TAG_LEN);
	pqc_secure_zero(want, sizeof(want));
	return ok ? 0 : -1;
}

static uint32_t get_u32(const uint8_t **p)
{
	uint32_t v = 0;
	for (int i = 0; i < 4; i++) {
		v |= (uint32_t)(*p)[i] << (8 * i);
	}
	*p += 4;
	return v;
}

static uint64_t get_u64(const uint8_t **p)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; i++) {
		v |= (uint64_t)(*p)[i] << (8 * i);
	}
	*p += 8;
	return v;
}

long slot_meta_deserialize(slot_meta_t *m, const uint8_t *in, size_t len)
{
	if (!m || !in || len < META_WIRE_LEN) {
		return -1;
	}
	const uint8_t *p = in;
	memset(m, 0, sizeof(*m));
	m->version = get_u32(&p);
	m->slot_id = get_u32(&p);
	memcpy(m->label, p, SLOT_LABEL_MAX);
	m->label[SLOT_LABEL_MAX - 1] = '\0';
	p += SLOT_LABEL_MAX;
	m->alg    = (pqc_alg_t)get_u32(&p);
	m->usage  = get_u32(&p);
	m->policy = get_u32(&p);
	m->state  = (slot_state_t)get_u32(&p);
	m->use_count      = get_u64(&p);
	m->so_pin_fails   = get_u32(&p);
	m->user_pin_fails = get_u32(&p);
	m->created_at     = get_u64(&p);
	m->last_used_at   = get_u64(&p);
	m->generation     = get_u32(&p);
	/* 合法性：状态与算法必须在枚举范围内，否则就是被改过的文件 */
	if (m->state < SLOT_ST_UNINIT || m->state >= SLOT_ST__COUNT) {
		return -1;
	}
	if (m->alg != PQC_ALG_NONE && !pqc_alg_info(m->alg)) {
		return -1;
	}
	return (long)(p - in);
}
