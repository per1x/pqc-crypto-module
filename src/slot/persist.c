/* persist.c —— 单个槽位的完整状态 ↔ 密文 blob
 *
 * 密钥库（src/store/keystore.c）与备份恢复（src/backup/）都用这一对函数，
 * 保证"存到 Flash"和"导出成备份"用的是同一套格式与同一套完整性保证。
 *
 * blob = meta_wire(92 B, 明文) ‖ wrap(KEK, AAD = meta_wire, 秘密载荷)
 *
 * 秘密载荷（明文布局，只在 KEK 包裹内部存在）：
 *   pin_flags u8 | pin_key(32) | so_salt(16) | so_verifier(32)
 *                | user_salt(16) | user_verifier(32)
 *   pre_lock u32 | key_kind u8 | pk_len u32 | pk | key_len u32 | key
 *   key_kind: 0 = 无密钥, 1 = 完整私钥, 2 = 种子（§7.6）
 *
 * 元数据放在包裹外面是有意的：密钥库需要能在不解密的情况下列出槽位状态，
 * 而它进了 AAD，改一个字节就会让解包失败。
 */
#include "persist.h"

#include "slot_internal.h"
#include "pqchsm/util.h"
#include "pqchsm/wrap.h"

#include <string.h>

#define PAYLOAD_FIXED (1 + 32 + PIN_SALT_LEN + VERIFIER_LEN + PIN_SALT_LEN + VERIFIER_LEN + 4 + 1 + 4 + 4)
#define PAYLOAD_MAX   (PAYLOAD_FIXED + 2592 + 4896)   /* 最大公钥 + 最大私钥 */

size_t hsm_slot_blob_max(void)
{
	return SLOT_META_WIRE_LEN + pqc_wrap_blob_len(PAYLOAD_MAX);
}

static void put_u32(uint8_t **p, uint32_t v)
{
	for (int i = 0; i < 4; i++) {
		(*p)[i] = (uint8_t)(v >> (8 * i));
	}
	*p += 4;
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

/* 调用方须持槽位锁 */
hsm_status_t slot_serialize_locked(slot_t *s, const uint8_t *kek, size_t kek_len,
                                   uint8_t *blob, size_t cap, size_t *blob_len)
{
	if (!kek || kek_len != PQC_KEK_LEN || !blob || !blob_len) {
		return HSM_ERR_BAD_ARG;
	}
	uint8_t meta_wire[SLOT_META_WIRE_LEN];
	if (slot_meta_serialize(&s->meta, meta_wire, sizeof(meta_wire)) != SLOT_META_WIRE_LEN) {
		return HSM_ERR_CRYPTO;
	}

	uint8_t *payload = pqc_secure_alloc(PAYLOAD_MAX);
	if (!payload) {
		return HSM_ERR_NOMEM;
	}
	uint8_t *p = payload;
	*p++ = (uint8_t)((s->has_so_pin ? 1 : 0) | (s->has_user_pin ? 2 : 0));
	/* pin_key 必须跟着走：PIN 验证值由它派生，不带上就没法跨设备恢复登录态 */
	memcpy(p, s->pin_key, 32);                 p += 32;
	memcpy(p, s->so_salt, PIN_SALT_LEN);       p += PIN_SALT_LEN;
	memcpy(p, s->so_verifier, VERIFIER_LEN);   p += VERIFIER_LEN;
	memcpy(p, s->user_salt, PIN_SALT_LEN);     p += PIN_SALT_LEN;
	memcpy(p, s->user_verifier, VERIFIER_LEN); p += VERIFIER_LEN;
	put_u32(&p, (uint32_t)s->pre_lock);

	uint8_t kind = s->sk ? 1 : (s->has_seed ? 2 : 0);
	*p++ = kind;
	put_u32(&p, (uint32_t)s->pk_len);
	if (s->pk_len) {
		memcpy(p, s->pk, s->pk_len);
		p += s->pk_len;
	}
	size_t klen = (kind == 1) ? s->sk_len : (kind == 2 ? s->seed_len : 0);
	put_u32(&p, (uint32_t)klen);
	if (klen) {
		memcpy(p, kind == 1 ? s->sk : s->seed, klen);
		p += klen;
	}
	size_t payload_len = (size_t)(p - payload);

	hsm_status_t st = HSM_OK;
	if (cap < SLOT_META_WIRE_LEN + pqc_wrap_blob_len(payload_len)) {
		st = HSM_ERR_BAD_ARG;
		goto out;
	}
	memcpy(blob, meta_wire, SLOT_META_WIRE_LEN);
	{
		size_t wlen = 0;
		if (pqc_wrap(kek, kek_len, meta_wire, SLOT_META_WIRE_LEN, payload, payload_len,
		             blob + SLOT_META_WIRE_LEN, cap - SLOT_META_WIRE_LEN, &wlen) != 0) {
			st = HSM_ERR_CRYPTO;
			goto out;
		}
		*blob_len = SLOT_META_WIRE_LEN + wlen;
	}
out:
	pqc_secure_free(payload, PAYLOAD_MAX);
	return st;
}

/* 调用方须持槽位锁。失败时槽位内容保持不变。 */
hsm_status_t slot_deserialize_locked(slot_t *s, const uint8_t *kek, size_t kek_len,
                                     const uint8_t *blob, size_t blob_len)
{
	if (!kek || kek_len != PQC_KEK_LEN || !blob || blob_len < SLOT_META_WIRE_LEN) {
		return HSM_ERR_BAD_ARG;
	}
	slot_meta_t meta;
	if (slot_meta_deserialize(&meta, blob, SLOT_META_WIRE_LEN) != SLOT_META_WIRE_LEN) {
		return HSM_ERR_INTEGRITY;
	}
	/* 记录必须属于本槽位 —— 防止把 slot 3 的记录塞进 slot 5 */
	if (meta.slot_id != s->meta.slot_id || meta.version != SLOT_META_VERSION) {
		return HSM_ERR_INTEGRITY;
	}

	uint8_t *payload = pqc_secure_alloc(PAYLOAD_MAX);
	if (!payload) {
		return HSM_ERR_NOMEM;
	}
	hsm_status_t st = HSM_ERR_INTEGRITY;
	size_t payload_len = 0;
	if (pqc_unwrap(kek, kek_len, blob, SLOT_META_WIRE_LEN,
	               blob + SLOT_META_WIRE_LEN, blob_len - SLOT_META_WIRE_LEN,
	               payload, PAYLOAD_MAX, &payload_len) != 0) {
		goto out;   /* AAD 不符 = 元数据被改过，或 KEK 不对 = 换了设备 */
	}
	if (payload_len < PAYLOAD_FIXED) {
		goto out;
	}

	{
		const uint8_t *p = payload;
		const uint8_t *end = payload + payload_len;
		uint8_t pin_flags = *p++;
		const uint8_t *pin_key = p;        p += 32;
		const uint8_t *so_salt = p;        p += PIN_SALT_LEN;
		const uint8_t *so_ver  = p;        p += VERIFIER_LEN;
		const uint8_t *user_salt = p;      p += PIN_SALT_LEN;
		const uint8_t *user_ver  = p;      p += VERIFIER_LEN;
		uint32_t pre_lock = get_u32(&p);
		uint8_t kind = *p++;
		uint32_t pk_len = get_u32(&p);
		if (kind > 2 || pre_lock >= (uint32_t)SLOT_ST__COUNT ||
		    pk_len > (uint32_t)(end - p)) {
			goto out;
		}
		const uint8_t *pk = p;
		p += pk_len;
		if ((size_t)(end - p) < 4) {
			goto out;
		}
		uint32_t key_len = get_u32(&p);
		if (key_len > (uint32_t)(end - p)) {
			goto out;
		}
		const uint8_t *key = p;

		/* 与元数据交叉校验：长度必须与算法参数集相符，
		 * 否则就是一条被拼接过的记录 */
		if (meta.alg != PQC_ALG_NONE) {
			const pqc_alg_info_t *info = pqc_alg_info(meta.alg);
			if (!info || pk_len != info->pk_len) {
				goto out;
			}
			if (kind == 1 && key_len != info->sk_len) {
				goto out;
			}
			if (kind == 2 && key_len != info->seed_len) {
				goto out;
			}
		} else if (kind != 0 || pk_len != 0) {
			goto out;
		}

		/* 到这里才动槽位 —— 前面任何一步失败都不留痕迹 */
		uint8_t *new_pk = NULL, *new_sk = NULL;
		if (pk_len) {
			new_pk = pqc_secure_alloc(pk_len);
			if (!new_pk) {
				st = HSM_ERR_NOMEM;
				goto out;
			}
			memcpy(new_pk, pk, pk_len);
		}
		if (kind == 1 && key_len) {
			new_sk = pqc_secure_alloc(key_len);
			if (!new_sk) {
				pqc_secure_free(new_pk, pk_len);
				st = HSM_ERR_NOMEM;
				goto out;
			}
			memcpy(new_sk, key, key_len);
		}

		slot_wipe_key_material(s);
		slot_wipe_pins(s);
		s->pk = new_pk;
		s->pk_len = pk_len;
		s->sk = new_sk;
		s->sk_len = (kind == 1) ? key_len : 0;
		if (kind == 2) {
			memcpy(s->seed, key, key_len);
			s->seed_len = key_len;
			s->has_seed = 1;
		}
		memcpy(s->pin_key, pin_key, 32);
		s->has_so_pin   = (pin_flags & 1) ? 1 : 0;
		s->has_user_pin = (pin_flags & 2) ? 1 : 0;
		memcpy(s->so_salt, so_salt, PIN_SALT_LEN);
		memcpy(s->so_verifier, so_ver, VERIFIER_LEN);
		memcpy(s->user_salt, user_salt, PIN_SALT_LEN);
		memcpy(s->user_verifier, user_ver, VERIFIER_LEN);
		s->pre_lock = (slot_state_t)pre_lock;
		s->meta = meta;
		/* 元数据 KMAC 标签用本机 KDR 重新盖 —— 跨设备恢复后自动重新 sealing */
		st = slot_reseal(s);
	}
out:
	pqc_secure_free(payload, PAYLOAD_MAX);
	return st;
}

hsm_status_t hsm_slot_serialize(hsm_token_t *tok, hsm_slot_id_t id,
                                const uint8_t *kek, size_t kek_len,
                                uint8_t *blob, size_t cap, size_t *blob_len)
{
	slot_t *s = slot_at(tok, id);
	if (!s) {
		return HSM_ERR_BAD_ARG;
	}
	SLOCK(s);
	hsm_status_t st = slot_check_integrity(s);
	if (st == HSM_OK) {
		st = slot_serialize_locked(s, kek, kek_len, blob, cap, blob_len);
	}
	SUNLOCK(s);
	return st;
}

hsm_status_t hsm_slot_deserialize(hsm_token_t *tok, hsm_slot_id_t id,
                                  const uint8_t *kek, size_t kek_len,
                                  const uint8_t *blob, size_t blob_len)
{
	slot_t *s = slot_at(tok, id);
	if (!s) {
		return HSM_ERR_BAD_ARG;
	}
	SLOCK(s);
	hsm_status_t st = slot_deserialize_locked(s, kek, kek_len, blob, blob_len);
	SUNLOCK(s);
	return st;
}
