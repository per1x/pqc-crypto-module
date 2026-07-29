/* 命令协议测试（Phase 5 第 3 项）
 *
 * 全部在进程内直接调 pqc_proto_dispatch —— 不碰 socket。
 * 端到端的 TCP 流程由 tools/cli_smoke.sh 覆盖。
 *
 * 重点是**畸形输入**：这一层是进程外接口，收到的字节全都不可信。
 */
#include "testlib.h"
#include "pqchsm/proto.h"
#include "pqchsm/slot.h"

#include <stdlib.h>

#define SO_PIN   "so-secret-0001"
#define USER_PIN "user-pin-4242"
#define CAP (1u << 20)

static uint8_t *g_req, *g_resp;
static pqc_proto_ctx_t g_ctx;

/* 发一条命令，返回 status；payload 回填 */
static int call(uint8_t cmd, const tlv_writer_t *w, const uint8_t **out, size_t *out_len)
{
	size_t rlen = 0;
	if (pqc_proto_build_req(cmd, 7, w ? w->buf : NULL, w ? w->len : 0,
	                        g_req, CAP, &rlen) != 0) {
		return -1;
	}
	size_t resp_len = 0;
	if (pqc_proto_dispatch(&g_ctx, g_req, rlen, g_resp, CAP, &resp_len) != 0) {
		return -1;
	}
	uint16_t st = 0;
	if (pqc_proto_resp_status(g_resp, resp_len, &st, out, out_len) != 0) {
		return -1;
	}
	/* seq 必须原样回带 */
	CHECK_EQ_INT(g_resp[4], 7);
	return (int)st;
}

static void test_tlv(void)
{
	TCASE("TLV 读写");
	uint8_t buf[256];
	tlv_writer_t w;
	tlv_init(&w, buf, sizeof(buf));
	tlv_put_u32(&w, TAG_SLOT, 3);
	tlv_put_u64(&w, TAG_SESSION, 0x1122334455667788ULL);
	tlv_put(&w, TAG_LABEL, "hi", 2);
	CHECK_EQ_INT(w.err, 0);
	CHECK_EQ_INT(w.len, (6 + 4) + (6 + 8) + (6 + 2));

	uint32_t u32 = 0;
	uint64_t u64 = 0;
	CHECK_EQ_INT(tlv_get_u32(buf, w.len, TAG_SLOT, &u32), 0);
	CHECK_EQ_INT(u32, 3);
	CHECK_EQ_INT(tlv_get_u64(buf, w.len, TAG_SESSION, &u64), 0);
	CHECK(u64 == 0x1122334455667788ULL);
	size_t l = 0;
	const uint8_t *v = tlv_find(buf, w.len, TAG_LABEL, &l);
	CHECK(v != NULL);
	CHECK_EQ_INT(l, 2);
	CHECK(memcmp(v, "hi", 2) == 0);
	/* 不存在的 tag */
	CHECK(tlv_find(buf, w.len, TAG_SIG, &l) == NULL);
	CHECK_EQ_INT(tlv_get_u32(buf, w.len, TAG_SESSION, &u32), -1);   /* 长度不符 */

	TCASE("TLV 写溢出必须置 err 而不是越界");
	tlv_init(&w, buf, 8);
	tlv_put(&w, TAG_DATA, "0123456789", 10);
	CHECK_EQ_INT(w.err, 1);
	CHECK_EQ_INT(w.len, 0);

	TCASE("TLV 长度字段越界必须被拒（畸形输入）");
	{
		uint8_t bad[16];
		memset(bad, 0, sizeof(bad));
		bad[0] = 0x12;                     /* tag = TAG_DATA */
		bad[2] = 0xff; bad[3] = 0xff;      /* len 远超实际 */
		CHECK(tlv_find(bad, sizeof(bad), TAG_DATA, &l) == NULL);
	}
}

static void test_framing(void)
{
	TCASE("分帧");
	uint8_t req[64];
	size_t n = 0;
	CHECK_EQ_INT(pqc_proto_build_req(CMD_PING, 1, NULL, 0, req, sizeof(req), &n), 0);
	CHECK_EQ_INT(n, PQC_PROTO_HDR_LEN);
	CHECK_EQ_INT(pqc_proto_frame_len(req, PQC_PROTO_HDR_LEN), PQC_PROTO_HDR_LEN);
	/* 头没读齐时先要求读满头 */
	CHECK_EQ_INT(pqc_proto_frame_len(req, 3), PQC_PROTO_HDR_LEN);

	TCASE("畸形帧头必须被拒");
	uint8_t bad[PQC_PROTO_HDR_LEN];
	memcpy(bad, req, sizeof(bad));
	bad[0] = 'X';
	CHECK_EQ_INT(pqc_proto_frame_len(bad, sizeof(bad)), -1);
	memcpy(bad, req, sizeof(bad));
	bad[2] = 99;                     /* 版本不对 */
	CHECK_EQ_INT(pqc_proto_frame_len(bad, sizeof(bad)), -1);
	memcpy(bad, req, sizeof(bad));
	bad[8] = 0xff; bad[9] = 0xff; bad[10] = 0xff; bad[11] = 0xff;   /* 超大 payload */
	CHECK_EQ_INT(pqc_proto_frame_len(bad, sizeof(bad)), -1);

	TCASE("dispatch 对畸形请求返回 -1（调用方应断开连接）");
	size_t rl = 0;
	CHECK_EQ_INT(pqc_proto_dispatch(&g_ctx, bad, sizeof(bad), g_resp, CAP, &rl), -1);
	CHECK_EQ_INT(pqc_proto_dispatch(&g_ctx, req, 3, g_resp, CAP, &rl), -1);
	/* 声明长度与实际长度不符 */
	memcpy(bad, req, sizeof(bad));
	bad[8] = 5;
	CHECK_EQ_INT(pqc_proto_dispatch(&g_ctx, bad, sizeof(bad), g_resp, CAP, &rl), -1);
	/* 响应缓冲太小 */
	CHECK_EQ_INT(pqc_proto_dispatch(&g_ctx, req, PQC_PROTO_HDR_LEN, g_resp, 4, &rl), -1);
}

static void test_flow(void)
{
	uint8_t plbuf[4096];
	tlv_writer_t w;
	const uint8_t *out = NULL;
	size_t out_len = 0;

	TCASE("ping / slots");
	CHECK_EQ_INT(call(CMD_PING, NULL, &out, &out_len), HSM_OK);
	CHECK_EQ_INT(call(CMD_SLOT_LIST, NULL, &out, &out_len), HSM_OK);
	{
		uint64_t c = 0;
		CHECK_EQ_INT(tlv_get_u64(out, out_len, TAG_COUNT, &c), 0);
		CHECK_EQ_INT(c, 2);
	}

	TCASE("未知命令码");
	CHECK_EQ_INT(call(0xEE, NULL, &out, &out_len), HSM_ERR_BAD_ARG);
	{
		size_t l = 0;
		CHECK(tlv_find(out, out_len, TAG_ERRMSG, &l) != NULL);
	}

	TCASE("缺必需字段一律 BAD_ARG，不能崩");
	tlv_init(&w, plbuf, sizeof(plbuf));
	CHECK_EQ_INT(call(CMD_SLOT_INFO, &w, &out, &out_len), HSM_ERR_BAD_ARG);
	CHECK_EQ_INT(call(CMD_INIT_TOKEN, &w, &out, &out_len), HSM_ERR_BAD_ARG);
	CHECK_EQ_INT(call(CMD_LOGIN, &w, &out, &out_len), HSM_ERR_BAD_ARG);
	CHECK_EQ_INT(call(CMD_SIGN, &w, &out, &out_len), HSM_ERR_BAD_ARG);
	CHECK_EQ_INT(call(CMD_GENERATE, &w, &out, &out_len), HSM_ERR_BAD_ARG);

	TCASE("init-token → 开会话 → SO 登录 → 设 User PIN");
	tlv_init(&w, plbuf, sizeof(plbuf));
	tlv_put_u32(&w, TAG_SLOT, 0);
	tlv_put(&w, TAG_LABEL, "proto-slot", 10);
	tlv_put(&w, TAG_PIN, SO_PIN, strlen(SO_PIN));
	CHECK_EQ_INT(call(CMD_INIT_TOKEN, &w, &out, &out_len), HSM_OK);

	uint64_t sess = 0;
	tlv_init(&w, plbuf, sizeof(plbuf));
	tlv_put_u32(&w, TAG_SLOT, 0);
	CHECK_EQ_INT(call(CMD_SESSION_OPEN, &w, &out, &out_len), HSM_OK);
	CHECK_EQ_INT(tlv_get_u64(out, out_len, TAG_SESSION, &sess), 0);
	CHECK(sess != 0);

	/* 错误 PIN */
	tlv_init(&w, plbuf, sizeof(plbuf));
	tlv_put_u64(&w, TAG_SESSION, sess);
	tlv_put_u32(&w, TAG_ROLE, HSM_ROLE_SO);
	tlv_put(&w, TAG_PIN, "wrong-pin", 9);
	CHECK_EQ_INT(call(CMD_LOGIN, &w, &out, &out_len), HSM_ERR_PIN_INCORRECT);

	tlv_init(&w, plbuf, sizeof(plbuf));
	tlv_put_u64(&w, TAG_SESSION, sess);
	tlv_put_u32(&w, TAG_ROLE, HSM_ROLE_SO);
	tlv_put(&w, TAG_PIN, SO_PIN, strlen(SO_PIN));
	CHECK_EQ_INT(call(CMD_LOGIN, &w, &out, &out_len), HSM_OK);

	tlv_init(&w, plbuf, sizeof(plbuf));
	tlv_put_u64(&w, TAG_SESSION, sess);
	tlv_put(&w, TAG_PIN, USER_PIN, strlen(USER_PIN));
	CHECK_EQ_INT(call(CMD_SET_USER_PIN, &w, &out, &out_len), HSM_OK);

	tlv_init(&w, plbuf, sizeof(plbuf));
	tlv_put_u64(&w, TAG_SESSION, sess);
	CHECK_EQ_INT(call(CMD_LOGOUT, &w, &out, &out_len), HSM_OK);

	/* 未登录不许生成 */
	tlv_init(&w, plbuf, sizeof(plbuf));
	tlv_put_u64(&w, TAG_SESSION, sess);
	tlv_put_u32(&w, TAG_ALG, PQC_ALG_ML_DSA_65);
	tlv_put_u32(&w, TAG_USAGE, KEY_USAGE_SIGN);
	CHECK_EQ_INT(call(CMD_GENERATE, &w, &out, &out_len), HSM_ERR_NOT_AUTHORIZED);

	tlv_init(&w, plbuf, sizeof(plbuf));
	tlv_put_u64(&w, TAG_SESSION, sess);
	tlv_put_u32(&w, TAG_ROLE, HSM_ROLE_USER);
	tlv_put(&w, TAG_PIN, USER_PIN, strlen(USER_PIN));
	CHECK_EQ_INT(call(CMD_LOGIN, &w, &out, &out_len), HSM_OK);

	TCASE("生成 → 取公钥 → 签名 → 用公钥独立验证");
	uint64_t handle = 0;
	tlv_init(&w, plbuf, sizeof(plbuf));
	tlv_put_u64(&w, TAG_SESSION, sess);
	tlv_put_u32(&w, TAG_ALG, PQC_ALG_ML_DSA_65);
	tlv_put_u32(&w, TAG_USAGE, KEY_USAGE_SIGN);
	CHECK_EQ_INT(call(CMD_GENERATE, &w, &out, &out_len), HSM_OK);
	CHECK_EQ_INT(tlv_get_u64(out, out_len, TAG_HANDLE, &handle), 0);

	static uint8_t pk[4096];
	size_t pk_len = 0;
	tlv_init(&w, plbuf, sizeof(plbuf));
	tlv_put_u64(&w, TAG_SESSION, sess);
	tlv_put_u64(&w, TAG_HANDLE, handle);
	CHECK_EQ_INT(call(CMD_PUBKEY, &w, &out, &out_len), HSM_OK);
	{
		size_t l = 0;
		const uint8_t *v = tlv_find(out, out_len, TAG_DATA, &l);
		CHECK(v != NULL);
		CHECK_EQ_INT(l, 1952);
		memcpy(pk, v, l);
		pk_len = l;
	}

	const uint8_t msg[] = "over the wire";
	static uint8_t sig[8192];
	size_t sig_len = 0;
	tlv_init(&w, plbuf, sizeof(plbuf));
	tlv_put_u64(&w, TAG_SESSION, sess);
	tlv_put_u64(&w, TAG_HANDLE, handle);
	tlv_put(&w, TAG_DATA, msg, sizeof(msg));
	CHECK_EQ_INT(call(CMD_SIGN, &w, &out, &out_len), HSM_OK);
	{
		size_t l = 0;
		const uint8_t *v = tlv_find(out, out_len, TAG_SIG, &l);
		CHECK(v != NULL);
		memcpy(sig, v, l);
		sig_len = l;
	}
	CHECK_EQ_INT(pqc_verify(PQC_ALG_ML_DSA_65, pk, msg, sizeof(msg), NULL, 0, sig, sig_len),
	             PQC_OK);
	CHECK_EQ_INT(pk_len, 1952);

	TCASE("协议里没有导出私钥的路径：全部命令码扫一遍，响应里不含私钥");
	{
		/* 用一个已知的探针：先取公钥，再确认任何命令的响应里都不出现
		 * 与私钥等长的可疑大块。这里做一个更直接的检查 ——
		 * 遍历所有命令码，断言没有任何一个返回 >4096 字节的 TLV（ML-DSA-65 私钥 4032）。 */
		int leaked = 0;
		for (uint8_t c = 1; c <= 0x12; c++) {
			tlv_init(&w, plbuf, sizeof(plbuf));
			tlv_put_u64(&w, TAG_SESSION, sess);
			tlv_put_u64(&w, TAG_HANDLE, handle);
			tlv_put_u32(&w, TAG_SLOT, 0);
			tlv_put(&w, TAG_DATA, msg, sizeof(msg));
			int rc = call(c, &w, &out, &out_len);
			if (rc < 0) {
				continue;
			}
			/* 响应里如果出现了私钥，一定是 4032 字节那一块 */
			size_t off = 0;
			while (off + 6 <= out_len) {
				uint32_t l = (uint32_t)out[off + 2] | ((uint32_t)out[off + 3] << 8) |
				             ((uint32_t)out[off + 4] << 16) | ((uint32_t)out[off + 5] << 24);
				if (l == 4032) {
					leaked = 1;
				}
				if (l > out_len - off - 6) {
					break;
				}
				off += 6 + l;
			}
		}
		CHECK_EQ_INT(leaked, 0);
	}

	TCASE("响应缓冲不足时不得交出半截数据");
	{
		tlv_init(&w, plbuf, sizeof(plbuf));
		tlv_put_u64(&w, TAG_SESSION, sess);
		tlv_put_u64(&w, TAG_HANDLE, handle);
		size_t rlen = 0, resp_len = 0;
		CHECK_EQ_INT(pqc_proto_build_req(CMD_PUBKEY, 1, w.buf, w.len, g_req, CAP, &rlen), 0);
		/* 给一个刚好放得下头+status 但放不下公钥的缓冲 */
		uint8_t small[PQC_PROTO_HDR_LEN + 2 + 32];
		CHECK_EQ_INT(pqc_proto_dispatch(&g_ctx, g_req, rlen, small, sizeof(small), &resp_len),
		             0);
		uint16_t st = 0;
		const uint8_t *o = NULL;
		size_t ol = 0;
		CHECK_EQ_INT(pqc_proto_resp_status(small, resp_len, &st, &o, &ol), 0);
		CHECK_EQ_INT(st, HSM_ERR_NOMEM);
		CHECK_EQ_INT(ol, 0);
	}

	TCASE("info 反映真实状态");
	tlv_init(&w, plbuf, sizeof(plbuf));
	tlv_put_u32(&w, TAG_SLOT, 0);
	CHECK_EQ_INT(call(CMD_SLOT_INFO, &w, &out, &out_len), HSM_OK);
	{
		uint32_t state = 0, alg = 0;
		uint64_t uc = 0;
		tlv_get_u32(out, out_len, TAG_STATE, &state);
		tlv_get_u32(out, out_len, TAG_ALG, &alg);
		tlv_get_u64(out, out_len, TAG_COUNT, &uc);
		CHECK_EQ_INT(state, SLOT_ST_LOADED);
		CHECK_EQ_INT(alg, PQC_ALG_ML_DSA_65);
		CHECK_EQ_INT(uc, 1);              /* 上面签了一次 */
	}

	TCASE("越界槽位");
	tlv_init(&w, plbuf, sizeof(plbuf));
	tlv_put_u32(&w, TAG_SLOT, 999);
	CHECK_EQ_INT(call(CMD_SLOT_INFO, &w, &out, &out_len), HSM_ERR_BAD_ARG);
}

int main(void)
{
	g_req = malloc(CAP);
	g_resp = malloc(CAP);
	if (!g_req || !g_resp) {
		return 1;
	}
	g_ctx.tok = hsm_token_new(2);
	g_ctx.keystore_path = NULL;
	CHECK(g_ctx.tok != NULL);

	test_tlv();
	test_framing();
	test_flow();

	hsm_token_free(g_ctx.tok);
	free(g_req);
	free(g_resp);
	return test_report("test_proto");
}
