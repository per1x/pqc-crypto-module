#include "pqchsm/proto.h"

#include "pqchsm/keystore.h"
#include "pqchsm/util.h"

#include <stdlib.h>
#include <string.h>

#define MAGIC0 'P'
#define MAGIC1 'Q'
#define RESP_MARK 0xFF

static void put_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put_u32le(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++) {
		p[i] = (uint8_t)(v >> (8 * i));
	}
}

static uint16_t get_u16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32le(const uint8_t *p)
{
	uint32_t v = 0;
	for (int i = 0; i < 4; i++) {
		v |= (uint32_t)p[i] << (8 * i);
	}
	return v;
}

/* ---- TLV ---------------------------------------------------------------- */

void tlv_init(tlv_writer_t *w, uint8_t *buf, size_t cap)
{
	w->buf = buf;
	w->cap = cap;
	w->len = 0;
	w->err = 0;
}

void tlv_put(tlv_writer_t *w, uint16_t tag, const void *val, size_t len)
{
	if (w->err) {
		return;
	}
	if (len > 0xffffffffu || w->len + 6 + len > w->cap) {
		w->err = 1;
		return;
	}
	put_u16(w->buf + w->len, tag);
	put_u32le(w->buf + w->len + 2, (uint32_t)len);
	if (len) {
		memcpy(w->buf + w->len + 6, val, len);
	}
	w->len += 6 + len;
}

void tlv_put_u32(tlv_writer_t *w, uint16_t tag, uint32_t v)
{
	uint8_t b[4];
	put_u32le(b, v);
	tlv_put(w, tag, b, 4);
}

void tlv_put_u64(tlv_writer_t *w, uint16_t tag, uint64_t v)
{
	uint8_t b[8];
	for (int i = 0; i < 8; i++) {
		b[i] = (uint8_t)(v >> (8 * i));
	}
	tlv_put(w, tag, b, 8);
}

const uint8_t *tlv_find(const uint8_t *p, size_t n, uint16_t tag, size_t *out_len)
{
	size_t off = 0;
	while (off + 6 <= n) {
		uint16_t t = get_u16(p + off);
		uint32_t l = get_u32le(p + off + 2);
		if (l > n - off - 6) {
			return NULL;          /* 长度越界 = 畸形帧 */
		}
		if (t == tag) {
			if (out_len) {
				*out_len = l;
			}
			return p + off + 6;
		}
		off += 6 + l;
	}
	return NULL;
}

int tlv_get_u32(const uint8_t *p, size_t n, uint16_t tag, uint32_t *out)
{
	size_t l = 0;
	const uint8_t *v = tlv_find(p, n, tag, &l);
	if (!v || l != 4 || !out) {
		return -1;
	}
	*out = get_u32le(v);
	return 0;
}

int tlv_get_u64(const uint8_t *p, size_t n, uint16_t tag, uint64_t *out)
{
	size_t l = 0;
	const uint8_t *v = tlv_find(p, n, tag, &l);
	if (!v || l != 8 || !out) {
		return -1;
	}
	uint64_t x = 0;
	for (int i = 0; i < 8; i++) {
		x |= (uint64_t)v[i] << (8 * i);
	}
	*out = x;
	return 0;
}

/* ---- 分帧 --------------------------------------------------------------- */

long pqc_proto_frame_len(const uint8_t *buf, size_t n)
{
	if (n < PQC_PROTO_HDR_LEN) {
		return PQC_PROTO_HDR_LEN;
	}
	if (buf[0] != MAGIC0 || buf[1] != MAGIC1 || buf[2] != PQC_PROTO_VERSION) {
		return -1;
	}
	uint32_t plen = get_u32le(buf + 8);
	if (plen > PQC_PROTO_MAX_PAYLOAD) {
		return -1;
	}
	return (long)(PQC_PROTO_HDR_LEN + plen);
}

int pqc_proto_build_req(uint8_t cmd, uint32_t seq,
                        const uint8_t *payload, size_t payload_len,
                        uint8_t *out, size_t cap, size_t *out_len)
{
	if (!out || !out_len || payload_len > PQC_PROTO_MAX_PAYLOAD) {
		return -1;
	}
	if (cap < PQC_PROTO_HDR_LEN + payload_len) {
		return -1;
	}
	out[0] = MAGIC0;
	out[1] = MAGIC1;
	out[2] = PQC_PROTO_VERSION;
	out[3] = cmd;
	put_u32le(out + 4, seq);
	put_u32le(out + 8, (uint32_t)payload_len);
	if (payload_len) {
		memcpy(out + PQC_PROTO_HDR_LEN, payload, payload_len);
	}
	*out_len = PQC_PROTO_HDR_LEN + payload_len;
	return 0;
}

int pqc_proto_resp_status(const uint8_t *resp, size_t n, uint16_t *status,
                          const uint8_t **payload, size_t *payload_len)
{
	if (!resp || n < PQC_PROTO_HDR_LEN + 2) {
		return -1;
	}
	if (resp[0] != MAGIC0 || resp[1] != MAGIC1 || resp[2] != PQC_PROTO_VERSION ||
	    resp[3] != RESP_MARK) {
		return -1;
	}
	uint32_t plen = get_u32le(resp + 8);
	if (plen < 2 || plen > n - PQC_PROTO_HDR_LEN) {
		return -1;
	}
	if (status) {
		*status = get_u16(resp + PQC_PROTO_HDR_LEN);
	}
	if (payload) {
		*payload = resp + PQC_PROTO_HDR_LEN + 2;
	}
	if (payload_len) {
		*payload_len = plen - 2;
	}
	return 0;
}

/* ---- 分派 --------------------------------------------------------------- */

/* 组装响应：头 + status + TLV */
static int emit(uint8_t *resp, size_t cap, size_t *resp_len, uint32_t seq,
                hsm_status_t st, const tlv_writer_t *w)
{
	size_t plen = 2 + (w ? w->len : 0);
	if (cap < PQC_PROTO_HDR_LEN + plen) {
		return -1;
	}
	resp[0] = MAGIC0;
	resp[1] = MAGIC1;
	resp[2] = PQC_PROTO_VERSION;
	resp[3] = RESP_MARK;
	put_u32le(resp + 4, seq);
	put_u32le(resp + 8, (uint32_t)plen);
	put_u16(resp + PQC_PROTO_HDR_LEN, (uint16_t)st);
	/* TLV 已经由调用方写在 resp+HDR+2 上（见下面的用法） */
	*resp_len = PQC_PROTO_HDR_LEN + plen;
	return 0;
}

/* 由对象句柄反查算法元数据。句柄低 32 位是 slot_id + 1。 */
static const pqc_alg_info_t *info_of_handle(hsm_token_t *tok, uint64_t handle)
{
	slot_meta_t m;
	hsm_slot_id_t slot = (hsm_slot_id_t)((handle & 0xffffffffu) - 1);
	if (hsm_slot_get_meta(tok, slot, &m) != HSM_OK) {
		return NULL;
	}
	return pqc_alg_info(m.alg);
}

/* 从 payload 里取一个字符串型 TLV 到 NUL 结尾缓冲。失败返回 -1。 */
static int get_str(const uint8_t *p, size_t n, uint16_t tag, char *out, size_t cap)
{
	size_t l = 0;
	const uint8_t *v = tlv_find(p, n, tag, &l);
	if (!v || l >= cap) {
		return -1;
	}
	memcpy(out, v, l);
	out[l] = '\0';
	return 0;
}

int pqc_proto_dispatch(pqc_proto_ctx_t *ctx,
                       const uint8_t *req, size_t req_len,
                       uint8_t *resp, size_t resp_cap, size_t *resp_len)
{
	if (!ctx || !ctx->tok || !req || !resp || !resp_len) {
		return -1;
	}
	if (req_len < PQC_PROTO_HDR_LEN) {
		return -1;
	}
	if (req[0] != MAGIC0 || req[1] != MAGIC1 || req[2] != PQC_PROTO_VERSION) {
		return -1;
	}
	uint8_t  cmd = req[3];
	uint32_t seq = get_u32le(req + 4);
	uint32_t plen = get_u32le(req + 8);
	if (plen != req_len - PQC_PROTO_HDR_LEN) {
		return -1;
	}
	const uint8_t *p = req + PQC_PROTO_HDR_LEN;
	size_t n = plen;

	if (resp_cap < PQC_PROTO_HDR_LEN + 2) {
		return -1;
	}
	/* TLV 直接写进响应缓冲的 payload 区，避免一次多余拷贝 */
	tlv_writer_t w;
	tlv_init(&w, resp + PQC_PROTO_HDR_LEN + 2, resp_cap - PQC_PROTO_HDR_LEN - 2);

	hsm_status_t st = HSM_ERR_BAD_ARG;
	uint32_t u32a = 0, u32b = 0, u32c = 0;
	uint64_t u64a = 0, u64b = 0;
	char pin[HSM_PIN_MAX_LEN + 1], label[SLOT_LABEL_MAX];

	switch (cmd) {
	case CMD_PING:
		st = HSM_OK;
		break;

	case CMD_SLOT_LIST:
		tlv_put_u64(&w, TAG_COUNT, (uint64_t)hsm_token_slot_count(ctx->tok));
		st = HSM_OK;
		break;

	case CMD_SLOT_INFO: {
		if (tlv_get_u32(p, n, TAG_SLOT, &u32a) != 0) {
			break;
		}
		slot_meta_t m;
		st = hsm_slot_get_meta(ctx->tok, (hsm_slot_id_t)u32a, &m);
		if (st == HSM_OK) {
			tlv_put(&w, TAG_LABEL, m.label, strlen(m.label));
			tlv_put_u32(&w, TAG_ALG, (uint32_t)m.alg);
			tlv_put_u32(&w, TAG_STATE, (uint32_t)m.state);
			tlv_put_u32(&w, TAG_USAGE, m.usage);
			tlv_put_u32(&w, TAG_POLICY, m.policy);
			tlv_put_u64(&w, TAG_COUNT, m.use_count);
		}
		break;
	}

	case CMD_INIT_TOKEN:
		if (tlv_get_u32(p, n, TAG_SLOT, &u32a) != 0 ||
		    get_str(p, n, TAG_LABEL, label, sizeof(label)) != 0 ||
		    get_str(p, n, TAG_PIN, pin, sizeof(pin)) != 0) {
			break;
		}
		st = hsm_slot_init_token(ctx->tok, (hsm_slot_id_t)u32a, label, pin);
		pqc_secure_zero(pin, sizeof(pin));
		break;

	case CMD_SET_USER_PIN:
		if (tlv_get_u64(p, n, TAG_SESSION, &u64a) != 0 ||
		    get_str(p, n, TAG_PIN, pin, sizeof(pin)) != 0) {
			break;
		}
		st = hsm_slot_set_user_pin(ctx->tok, (hsm_session_t)u64a, pin);
		pqc_secure_zero(pin, sizeof(pin));
		break;

	case CMD_SESSION_OPEN: {
		if (tlv_get_u32(p, n, TAG_SLOT, &u32a) != 0) {
			break;
		}
		hsm_session_t s = 0;
		st = hsm_session_open(ctx->tok, (hsm_slot_id_t)u32a, &s);
		if (st == HSM_OK) {
			tlv_put_u64(&w, TAG_SESSION, (uint64_t)s);
		}
		break;
	}

	case CMD_SESSION_CLOSE:
		if (tlv_get_u64(p, n, TAG_SESSION, &u64a) != 0) {
			break;
		}
		st = hsm_session_close(ctx->tok, (hsm_session_t)u64a);
		break;

	case CMD_LOGIN:
		if (tlv_get_u64(p, n, TAG_SESSION, &u64a) != 0 ||
		    tlv_get_u32(p, n, TAG_ROLE, &u32a) != 0 ||
		    get_str(p, n, TAG_PIN, pin, sizeof(pin)) != 0) {
			break;
		}
		st = hsm_session_login(ctx->tok, (hsm_session_t)u64a, (hsm_role_t)u32a, pin);
		pqc_secure_zero(pin, sizeof(pin));
		break;

	case CMD_LOGOUT:
		if (tlv_get_u64(p, n, TAG_SESSION, &u64a) != 0) {
			break;
		}
		st = hsm_session_logout(ctx->tok, (hsm_session_t)u64a);
		break;

	case CMD_GENERATE: {
		if (tlv_get_u64(p, n, TAG_SESSION, &u64a) != 0 ||
		    tlv_get_u32(p, n, TAG_ALG, &u32a) != 0 ||
		    tlv_get_u32(p, n, TAG_USAGE, &u32b) != 0) {
			break;
		}
		if (tlv_get_u32(p, n, TAG_POLICY, &u32c) != 0) {
			u32c = 0;
		}
		hsm_handle_t h = HSM_INVALID_HANDLE;
		st = hsm_slot_generate(ctx->tok, (hsm_session_t)u64a, (pqc_alg_t)u32a,
		                       u32b, u32c, &h);
		if (st == HSM_OK) {
			tlv_put_u64(&w, TAG_HANDLE, (uint64_t)h);
		}
		break;
	}

	case CMD_PUBKEY: {
		if (tlv_get_u64(p, n, TAG_SESSION, &u64a) != 0 ||
		    tlv_get_u64(p, n, TAG_HANDLE, &u64b) != 0) {
			break;
		}
		/* 直接写进响应缓冲的 TLV 值区，省一次大拷贝 */
		uint8_t *dst = w.buf + w.len + 6;
		size_t room = (w.cap > w.len + 6) ? w.cap - w.len - 6 : 0;
		size_t got = 0;
		/* 先按算法元数据预检查空间：否则"响应装不下"会被底层报成
		 * BAD_ARG，客户端分不清是自己参数错还是缓冲不够。 */
		{
			const pqc_alg_info_t *ai = info_of_handle(ctx->tok, u64b);
			if (ai && room < ai->pk_len) {
				st = HSM_ERR_NOMEM;
				break;
			}
		}
		st = hsm_object_public_key(ctx->tok, (hsm_session_t)u64a, (hsm_handle_t)u64b,
		                           dst, room, &got);
		if (st == HSM_OK) {
			put_u16(w.buf + w.len, TAG_DATA);
			put_u32le(w.buf + w.len + 2, (uint32_t)got);
			w.len += 6 + got;
		}
		break;
	}

	case CMD_SIGN: {
		size_t dlen = 0;
		const uint8_t *data = tlv_find(p, n, TAG_DATA, &dlen);
		if (tlv_get_u64(p, n, TAG_SESSION, &u64a) != 0 ||
		    tlv_get_u64(p, n, TAG_HANDLE, &u64b) != 0 || !data) {
			break;
		}
		uint8_t *dst = w.buf + w.len + 6;
		size_t room = (w.cap > w.len + 6) ? w.cap - w.len - 6 : 0;
		size_t got = 0;
		{
			const pqc_alg_info_t *ai = info_of_handle(ctx->tok, u64b);
			if (ai && room < ai->sig_len) {
				st = HSM_ERR_NOMEM;
				break;
			}
		}
		st = hsm_object_sign(ctx->tok, (hsm_session_t)u64a, (hsm_handle_t)u64b,
		                     data, dlen, NULL, 0, dst, room, &got);
		if (st == HSM_OK) {
			put_u16(w.buf + w.len, TAG_SIG);
			put_u32le(w.buf + w.len + 2, (uint32_t)got);
			w.len += 6 + got;
		}
		break;
	}

	case CMD_DECAPS: {
		size_t dlen = 0;
		const uint8_t *ct = tlv_find(p, n, TAG_DATA, &dlen);
		if (tlv_get_u64(p, n, TAG_SESSION, &u64a) != 0 ||
		    tlv_get_u64(p, n, TAG_HANDLE, &u64b) != 0 || !ct) {
			break;
		}
		uint8_t ss[64];
		size_t got = 0;
		st = hsm_object_decaps(ctx->tok, (hsm_session_t)u64a, (hsm_handle_t)u64b,
		                       ct, dlen, ss, sizeof(ss), &got);
		if (st == HSM_OK) {
			tlv_put(&w, TAG_DATA, ss, got);
		}
		pqc_secure_zero(ss, sizeof(ss));
		break;
	}

	case CMD_DESTROY:
		if (tlv_get_u64(p, n, TAG_SESSION, &u64a) != 0 ||
		    tlv_get_u64(p, n, TAG_HANDLE, &u64b) != 0) {
			break;
		}
		st = hsm_object_destroy(ctx->tok, (hsm_session_t)u64a, (hsm_handle_t)u64b);
		break;

	case CMD_ZEROIZE:
		if (tlv_get_u64(p, n, TAG_SESSION, &u64a) != 0 ||
		    tlv_get_u32(p, n, TAG_SLOT, &u32a) != 0) {
			break;
		}
		st = hsm_slot_zeroize(ctx->tok, (hsm_session_t)u64a, (hsm_slot_id_t)u32a);
		break;

	case CMD_UNLOCK:
		if (tlv_get_u64(p, n, TAG_SESSION, &u64a) != 0 ||
		    tlv_get_u32(p, n, TAG_SLOT, &u32a) != 0) {
			break;
		}
		st = hsm_slot_unlock(ctx->tok, (hsm_session_t)u64a, (hsm_slot_id_t)u32a);
		break;

	case CMD_SAVE:
		st = ctx->keystore_path ? hsm_keystore_save(ctx->tok, ctx->keystore_path)
		                        : HSM_ERR_BAD_ARG;
		break;

	case CMD_ROTATE_KEK:
		st = ctx->keystore_path ? hsm_keystore_rotate_kek(ctx->tok, ctx->keystore_path)
		                        : HSM_ERR_BAD_ARG;
		break;

	default:
		st = HSM_ERR_BAD_ARG;
		tlv_put(&w, TAG_ERRMSG, "unknown command", 15);
		break;
	}

	if (w.err) {
		/* TLV 写溢出：退回一个只带状态码的响应，不要交出半截数据 */
		tlv_init(&w, resp + PQC_PROTO_HDR_LEN + 2, resp_cap - PQC_PROTO_HDR_LEN - 2);
		st = HSM_ERR_NOMEM;
	}
	return emit(resp, resp_cap, resp_len, seq, st, &w);
}
