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
	res = TEEC_OpenSession(&t->ctx, &t->sess, &uuid,
	                       TEEC_LOGIN_PUBLIC, NULL, &op, &origin);
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

int pqchsm_ta_kdf(pqchsm_ta *t, const char *label,
                  const uint8_t *salt, size_t salt_len,
                  uint8_t *out, size_t out_len)
{
	TEEC_Operation op;
	uint32_t       origin;

	if (!label || !out || !out_len)
		return -1;
	memset(&op, 0, sizeof(op));
	op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT,
	                                 TEEC_MEMREF_TEMP_INPUT,
	                                 TEEC_MEMREF_TEMP_OUTPUT, TEEC_NONE);
	op.params[0].tmpref.buffer = (void *)label;
	op.params[0].tmpref.size   = strlen(label) + 1;
	op.params[1].tmpref.buffer = (void *)salt;
	op.params[1].tmpref.size   = salt_len;
	op.params[2].tmpref.buffer = out;
	op.params[2].tmpref.size   = out_len;
	return invoke(t, TA_PQCHSM_CMD_KDF_DERIVE, &op, &origin);
}

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

int pqchsm_ta_wrap(pqchsm_ta *t,
                   const uint8_t *aad, size_t aad_len,
                   const uint8_t *pt, size_t pt_len,
                   uint8_t *blob, size_t cap, size_t *blob_len)
{
	TEEC_Operation op;
	uint32_t       origin;
	int            rc;

	if (!blob || !blob_len)
		return -1;
	memset(&op, 0, sizeof(op));
	op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT,
	                                 TEEC_MEMREF_TEMP_INPUT,
	                                 TEEC_MEMREF_TEMP_INOUT, TEEC_NONE);
	op.params[0].tmpref.buffer = (void *)aad;
	op.params[0].tmpref.size   = aad_len;
	op.params[1].tmpref.buffer = (void *)pt;
	op.params[1].tmpref.size   = pt_len;
	op.params[2].tmpref.buffer = blob;
	op.params[2].tmpref.size   = cap;
	rc = invoke(t, TA_PQCHSM_CMD_WRAP, &op, &origin);
	*blob_len = op.params[2].tmpref.size;
	return rc;
}

int pqchsm_ta_unwrap(pqchsm_ta *t,
                     const uint8_t *aad, size_t aad_len,
                     const uint8_t *blob, size_t blob_len,
                     uint8_t *pt, size_t cap, size_t *pt_len)
{
	TEEC_Operation op;
	uint32_t       origin;
	int            rc;

	if (!blob || !pt_len)
		return -1;
	memset(&op, 0, sizeof(op));
	op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT,
	                                 TEEC_MEMREF_TEMP_INPUT,
	                                 TEEC_MEMREF_TEMP_INOUT, TEEC_NONE);
	op.params[0].tmpref.buffer = (void *)aad;
	op.params[0].tmpref.size   = aad_len;
	op.params[1].tmpref.buffer = (void *)blob;
	op.params[1].tmpref.size   = blob_len;
	op.params[2].tmpref.buffer = pt;
	op.params[2].tmpref.size   = cap;
	rc = invoke(t, TA_PQCHSM_CMD_UNWRAP, &op, &origin);
	*pt_len = op.params[2].tmpref.size;
	return rc;
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
