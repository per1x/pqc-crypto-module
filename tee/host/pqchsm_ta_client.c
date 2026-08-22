#include "pqchsm_ta_client.h"

#include <stdlib.h>
#include <string.h>

#include "pqchsm_ta_proto.h"

int pqchsm_ta_open(pqchsm_ta *t)
{
	TEEC_Result     res;
	TEEC_UUID       uuid = TA_PQCHSM_UUID;
	TEEC_Operation  op;
	uint32_t        origin;

	if (!t)
		return -1;
	memset(t, 0, sizeof(*t));

	res = TEEC_InitializeContext(NULL, &t->ctx);
	if (res != TEEC_SUCCESS)
		return (int)res;
	memset(&op, 0, sizeof(op));
	/* ⚠️ 不再用 TEEC_LOGIN_PUBLIC（PS-22）：那等于"调用方是谁不记录也不检查"。
	 * USER 让 OP-TEE 把调用进程的 uid 放进客户端身份里，TA 侧据此拒绝匿名会话
	 * （见 pqchsm_ta.c 的 TA_OpenSessionEntryPoint）。 */
	res = TEEC_OpenSession(&t->ctx, &t->sess, &uuid,
	                       TEEC_LOGIN_USER, NULL, &op, &origin);
	if (res != TEEC_SUCCESS) {
		TEEC_FinalizeContext(&t->ctx);
		return (int)res;
	}
	t->open = 1;
	return 0;
}

void pqchsm_ta_close(pqchsm_ta *t)
{
	if (!t || !t->open)
		return;
	TEEC_CloseSession(&t->sess);
	TEEC_FinalizeContext(&t->ctx);
	t->open = 0;
}

static int invoke(pqchsm_ta *t, uint32_t cmd, TEEC_Operation *op,
                  uint32_t *origin)
{
	TEEC_Result res;

	res = TEEC_InvokeCommand(&t->sess, cmd, op, origin);
	return (int)res;
}

int pqchsm_ta_get_info(pqchsm_ta *t, uint32_t *version, uint32_t *features)
{
	TEEC_Operation op;
	uint32_t       origin;
	int            rc;

	memset(&op, 0, sizeof(op));
	op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_OUTPUT, TEEC_NONE,
	                                 TEEC_NONE, TEEC_NONE);
	rc = invoke(t, TA_PQCHSM_CMD_GET_INFO, &op, &origin);
	if (rc)
		return rc;
	if (version)
		*version = op.params[0].value.a;
	if (features)
		*features = op.params[0].value.b;
	return 0;
}

/* ============================================================================
 * 【已删除的三个 shim：kdf / wrap / unwrap（PS-22 / PS-24）】
 * ============================================================================
 * 对应的 TA 命令 1/3/4 已经删掉（理由见 pqchsm_ta_proto.h 的墓碑注释）。
 * 这一侧一并删干净，而不是留一个"调用必失败"的空壳 —— 留着的话，
 * 将来有人为了让它"能用"再把 TA 那边补回去，就等于把洞挖回来。
 * 全仓 grep 过：产品路径一处都没用过它们。
 */

int pqchsm_ta_kek_set(pqchsm_ta *t, const uint8_t *salt, size_t salt_len)
{
	TEEC_Operation op;
	uint32_t       origin;

	if (!salt || !salt_len)
		return -1;
	memset(&op, 0, sizeof(op));
	op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT, TEEC_NONE,
	                                 TEEC_NONE, TEEC_NONE);
	op.params[0].tmpref.buffer = (void *)salt;
	op.params[0].tmpref.size   = salt_len;
	return invoke(t, TA_PQCHSM_CMD_KEK_SET, &op, &origin);
}

int pqchsm_ta_keygen(pqchsm_ta *t, uint32_t alg,
                     uint8_t *pk,
                     uint8_t *blob, size_t cap, size_t *blob_len)
{
	TEEC_Operation op;
	uint32_t       origin;
	int            rc;

	if (!pk || !blob || !blob_len)
		return -1;
	memset(&op, 0, sizeof(op));
	op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT,
	                                 TEEC_MEMREF_TEMP_OUTPUT,
	                                 TEEC_MEMREF_TEMP_INOUT, TEEC_NONE);
	op.params[0].value.a = alg;
	op.params[1].tmpref.buffer = pk;
	op.params[1].tmpref.size   = TA_PQCHSM_MAX_PK;
	op.params[2].tmpref.buffer = blob;
	op.params[2].tmpref.size   = cap;
	rc = invoke(t, TA_PQCHSM_CMD_KEYGEN, &op, &origin);
	*blob_len = op.params[2].tmpref.size;
	return rc;
}

int pqchsm_ta_keygen_from_seed(pqchsm_ta *t, uint32_t alg,
                               const uint8_t *seed, size_t seed_len,
                               uint8_t *pk,
                               uint8_t *blob, size_t cap, size_t *blob_len)
{
	TEEC_Operation op;
	uint32_t       origin;
	int            rc;

	if (!seed || !pk || !blob || !blob_len)
		return -1;
	memset(&op, 0, sizeof(op));
	op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT,
	                                 TEEC_MEMREF_TEMP_INPUT,
	                                 TEEC_MEMREF_TEMP_OUTPUT,
	                                 TEEC_MEMREF_TEMP_INOUT);
	op.params[0].value.a = alg;
	op.params[1].tmpref.buffer = (void *)seed;
	op.params[1].tmpref.size   = seed_len;
	op.params[2].tmpref.buffer = pk;
	op.params[2].tmpref.size   = TA_PQCHSM_MAX_PK;
	op.params[3].tmpref.buffer = blob;
	op.params[3].tmpref.size   = cap;
	rc = invoke(t, TA_PQCHSM_CMD_KEYGEN_FROM_SEED, &op, &origin);
	*blob_len = op.params[3].tmpref.size;
	return rc;
}

int pqchsm_ta_decaps(pqchsm_ta *t, uint32_t alg,
                     const uint8_t *blob, size_t blob_len,
                     const uint8_t *ct, size_t ct_len,
                     uint8_t *ss, size_t ss_len)
{
	TEEC_Operation op;
	uint32_t       origin;

	if (!blob || !ct || !ss)
		return -1;
	memset(&op, 0, sizeof(op));
	op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT,
	                                 TEEC_MEMREF_TEMP_INPUT,
	                                 TEEC_MEMREF_TEMP_INPUT,
	                                 TEEC_MEMREF_TEMP_OUTPUT);
	op.params[0].value.a = alg;
	op.params[1].tmpref.buffer = (void *)blob;
	op.params[1].tmpref.size   = blob_len;
	op.params[2].tmpref.buffer = (void *)ct;
	op.params[2].tmpref.size   = ct_len;
	op.params[3].tmpref.buffer = ss;
	op.params[3].tmpref.size   = ss_len;
	return invoke(t, TA_PQCHSM_CMD_DECAPS, &op, &origin);
}

int pqchsm_ta_sign(pqchsm_ta *t, uint32_t alg,
                   const uint8_t *blob, size_t blob_len,
                   const uint8_t *ctx, size_t ctx_len,
                   const uint8_t *msg, size_t msg_len,
                   uint8_t *sig, size_t cap, size_t *sig_len)
{
	TEEC_Operation op;
	uint32_t       origin;
	uint8_t       *frame;
	size_t         frame_len;
	int            rc;

	if (!blob || !sig || !sig_len || ctx_len > 255)
		return -1;
	if ((!msg && msg_len) || (!ctx && ctx_len))
		return -1;

	/* 消息帧：ctx_len(1B) ‖ ctx ‖ msg */
	frame_len = 1 + ctx_len + msg_len;
	frame = malloc(frame_len);
	if (!frame)
		return -1;
	frame[0] = (uint8_t)ctx_len;
	if (ctx_len)
		memcpy(frame + 1, ctx, ctx_len);
	if (msg_len)
		memcpy(frame + 1 + ctx_len, msg, msg_len);

	memset(&op, 0, sizeof(op));
	op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT,
	                                 TEEC_MEMREF_TEMP_INPUT,
	                                 TEEC_MEMREF_TEMP_INPUT,
	                                 TEEC_MEMREF_TEMP_INOUT);
	op.params[0].value.a = alg;
	op.params[1].tmpref.buffer = (void *)blob;
	op.params[1].tmpref.size   = blob_len;
	op.params[2].tmpref.buffer = frame;
	op.params[2].tmpref.size   = frame_len;
	op.params[3].tmpref.buffer = sig;
	op.params[3].tmpref.size   = cap;
	rc = invoke(t, TA_PQCHSM_CMD_SIGN, &op, &origin);
	*sig_len = op.params[3].tmpref.size;
	free(frame);
	return rc;
}
