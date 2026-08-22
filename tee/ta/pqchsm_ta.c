/* pqchsm_ta.c —— pqc-hsm 可信应用：私钥运算在 S-EL1 完成
 *
 * 安全边界：私钥明文不出 TA。KEYGEN 返回 (公钥, PWRP 包裹私钥 blob)，
 * SIGN/DECAPS 以 blob 为输入，TA 内解包即用即焚（pqchsm_bzero）。
 * 包裹 KEK 由 KDR 派生；KDR = PTA_SYSTEM_DERIVE_TA_UNIQUE_KEY 派生的
 * 设备×TA 唯一密钥（HUK 不出芯片，普通世界拿不到）。
 *
 * 防护声明（必须写进文档）：防普通世界**应用/内核态读私钥明文**的前提是
 * 普通世界内核可信；若 REE 内核被完全攻陷，它可以重放/滥用 TA 服务
 * （但私钥明文仍不出 TA）。即"防普通世界应用，不防普通世界内核"。
 *
 * 命令协议见 tee/include/pqchsm_ta_proto.h 与 docs/reference/tee-protocol.zh-CN.md。
 */
#include <tee_api.h>
#include <string.h>

#include "pqchsm_ta_proto.h"
#include "ta_fips202.h"
#include "ta_kdf.h"
#include "ta_pqc.h"
#include "ta_wrap.h"

/* OP-TEE system PTA（与 optee_os lib/libutee/include/pta_system.h 一致，
 * 这里自己定义以避免依赖 dev kit 是否导出该头） */
#define PQCHSM_PTA_SYSTEM_UUID \
	{ 0x3a2f8978, 0x5dc0, 0x11e8, \
	  { 0x9c, 0x2d, 0xfa, 0x7a, 0xe0, 0x1b, 0xbe, 0xbc } }
#define PQCHSM_PTA_DERIVE_TA_UNIQUE_KEY 1

/* KEK 派生标签：与 src/store/wrap.c 的 pqc_kek_derive 一致 */
#define PQCHSM_KEK_LABEL "pqc-hsm/storage-kek"

/* KDR 派生时的 extra data：域分隔 + 版本，改动即整库密钥失效 */
static const uint8_t kdr_extra[] = "pqchsm-kdr-v1";

/* 实例级缓存：KDR 与 KEK（SINGLE_INSTANCE，全会话共享；KEK 是设备级
 * 存储主密钥，不属于某个会话） */
static uint8_t g_kdr[32];
static int     g_kdr_ready;
static uint8_t g_kek[TA_KEK_LEN];
static int     g_kek_ready;

/* KDR：设备×TA 唯一密钥，懒派生 */
static TEE_Result kdr_get(void)
{
	static const TEE_UUID pta_uuid = PQCHSM_PTA_SYSTEM_UUID;
	TEE_TASessionHandle   sess;
	TEE_Param             params[2];
	TEE_Result            res;
	uint32_t              types;

	if (g_kdr_ready)
		return TEE_SUCCESS;

	res = TEE_OpenTASession(&pta_uuid, TEE_TIMEOUT_INFINITE,
	                        TEE_PARAM_TYPES(TEE_PARAM_TYPE_NONE,
	                                        TEE_PARAM_TYPE_NONE,
	                                        TEE_PARAM_TYPE_NONE,
	                                        TEE_PARAM_TYPE_NONE),
	                        NULL, &sess, NULL);
	if (res != TEE_SUCCESS)
		return res;

	memset(params, 0, sizeof(params));
	params[0].memref.buffer = (void *)kdr_extra;
	params[0].memref.size   = sizeof(kdr_extra) - 1;
	params[1].memref.buffer = g_kdr;
	params[1].memref.size   = sizeof(g_kdr);
	types = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
	                        TEE_PARAM_TYPE_MEMREF_OUTPUT,
	                        TEE_PARAM_TYPE_NONE, TEE_PARAM_TYPE_NONE);
	res = TEE_InvokeTACommand(sess, TEE_TIMEOUT_INFINITE,
	                          PQCHSM_PTA_DERIVE_TA_UNIQUE_KEY,
	                          types, params, NULL);
	TEE_CloseTASession(sess);
	if (res == TEE_SUCCESS && params[1].memref.size != sizeof(g_kdr))
		res = TEE_ERROR_GENERIC;
	if (res == TEE_SUCCESS)
		g_kdr_ready = 1;
	else
		pqchsm_bzero(g_kdr, sizeof(g_kdr));
	return res;
}

/* ---- 命令实现 ---------------------------------------------------------- */

static TEE_Result cmd_get_info(uint32_t types, TEE_Param params[4])
{
	if (types != TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_OUTPUT,
	                             TEE_PARAM_TYPE_NONE, TEE_PARAM_TYPE_NONE,
	                             TEE_PARAM_TYPE_NONE))
		return TEE_ERROR_BAD_PARAMETERS;
	params[0].value.a = PQCHSM_TA_PROTO_VERSION;
	/* bit0(KDF) 与 bit1(WRAP/UNWRAP) 恒 0：那两条命令已经删掉（PS-22/24）。
	 * 位的**位置**保留，不让后面的位往前挪 —— 挪了的话旧 host 会把
	 * "支持 ML-KEM" 读成 "支持 KDF"。 */
	params[0].value.b = 0xC; /* ML-KEM | ML-DSA */
	return TEE_SUCCESS;
}

static TEE_Result cmd_kek_set(uint32_t types, TEE_Param params[4])
{
	TEE_Result res;

	if (types != TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
	                             TEE_PARAM_TYPE_NONE, TEE_PARAM_TYPE_NONE,
	                             TEE_PARAM_TYPE_NONE))
		return TEE_ERROR_BAD_PARAMETERS;
	if (!params[0].memref.buffer || params[0].memref.size == 0 ||
	    params[0].memref.size > TA_PQCHSM_MAX_SALT)
		return TEE_ERROR_BAD_PARAMETERS;

	res = kdr_get();
	if (res != TEE_SUCCESS)
		return res;
	if (ta_kdf_derive(g_kdr, sizeof(g_kdr),
	                  params[0].memref.buffer, params[0].memref.size,
	                  PQCHSM_KEK_LABEL, g_kek, sizeof(g_kek)) != 0)
		return TEE_ERROR_GENERIC;
	g_kek_ready = 1;
	return TEE_SUCCESS;
}

/* 解包私钥 blob 到栈缓冲，用完调用方负责 pqchsm_bzero */
static TEE_Result unwrap_sk(const void *blob, size_t blob_len,
                            uint8_t *sk, size_t sk_len)
{
	if (!blob || blob_len > TA_PQCHSM_MAX_BLOB)
		return TEE_ERROR_BAD_PARAMETERS;
	switch (ta_wrap_open(g_kek, NULL, 0, blob, blob_len,
	                     sk, sk_len, &sk_len)) {
	case 0:
		return TEE_SUCCESS;
	case -2:
		return TEE_ERROR_MAC_INVALID;
	default:
		return TEE_ERROR_GENERIC;
	}
}

/* KEYGEN 与 KEYGEN_FROM_SEED 的公共收尾：包裹私钥并填输出 */
static TEE_Result keygen_finish(uint8_t *sk, size_t sk_len,
                                TEE_Param *blob_param)
{
	size_t blob_len = 0;

	if (blob_param->memref.size < ta_wrap_blob_len(sk_len)) {
		blob_param->memref.size = ta_wrap_blob_len(sk_len);
		return TEE_ERROR_SHORT_BUFFER;
	}
	if (ta_wrap_seal(g_kek, NULL, 0, sk, sk_len,
	                 blob_param->memref.buffer, blob_param->memref.size,
	                 &blob_len) != 0)
		return TEE_ERROR_GENERIC;
	blob_param->memref.size = blob_len;
	return TEE_SUCCESS;
}

static TEE_Result cmd_keygen(uint32_t types, TEE_Param params[4])
{
	const ta_pqc_dims_t *d;
	uint8_t             sk[TA_PQCHSM_MAX_PT];
	TEE_Result          res;

	if (types != TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
	                             TEE_PARAM_TYPE_MEMREF_OUTPUT,
	                             TEE_PARAM_TYPE_MEMREF_INOUT,
	                             TEE_PARAM_TYPE_NONE))
		return TEE_ERROR_BAD_PARAMETERS;
	if (!g_kek_ready)
		return TEE_ERROR_BAD_STATE;
	d = ta_pqc_dims(params[0].value.a);
	if (!d)
		return TEE_ERROR_BAD_PARAMETERS;
	if (params[1].memref.size < d->pk_len) {
		params[1].memref.size = d->pk_len;
		return TEE_ERROR_SHORT_BUFFER;
	}
	if (ta_pqc_keypair(d->alg, params[1].memref.buffer, sk) != 0)
		return TEE_ERROR_GENERIC;
	params[1].memref.size = d->pk_len;
	res = keygen_finish(sk, d->sk_len, &params[2]);
	pqchsm_bzero(sk, sizeof(sk));
	return res;
}

static TEE_Result cmd_keygen_from_seed(uint32_t types, TEE_Param params[4])
{
	const ta_pqc_dims_t *d;
	uint8_t             sk[TA_PQCHSM_MAX_PT];
	TEE_Result          res;

	if (types != TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
	                             TEE_PARAM_TYPE_MEMREF_INPUT,
	                             TEE_PARAM_TYPE_MEMREF_OUTPUT,
	                             TEE_PARAM_TYPE_MEMREF_INOUT))
		return TEE_ERROR_BAD_PARAMETERS;
	if (!g_kek_ready)
		return TEE_ERROR_BAD_STATE;
	d = ta_pqc_dims(params[0].value.a);
	if (!d)
		return TEE_ERROR_BAD_PARAMETERS;
	if (!params[1].memref.buffer ||
	    params[1].memref.size != d->seed_len)
		return TEE_ERROR_BAD_PARAMETERS;
	if (params[2].memref.size < d->pk_len) {
		params[2].memref.size = d->pk_len;
		return TEE_ERROR_SHORT_BUFFER;
	}
	if (ta_pqc_keypair_from_seed(d->alg,
	                             params[1].memref.buffer,
	                             params[1].memref.size,
	                             params[2].memref.buffer, sk) != 0)
		return TEE_ERROR_GENERIC;
	params[2].memref.size = d->pk_len;
	res = keygen_finish(sk, d->sk_len, &params[3]);
	pqchsm_bzero(sk, sizeof(sk));
	return res;
}

static TEE_Result cmd_decaps(uint32_t types, TEE_Param params[4])
{
	const ta_pqc_dims_t *d;
	uint8_t             sk[TA_PQCHSM_MAX_PT];
	TEE_Result          res;

	if (types != TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
	                             TEE_PARAM_TYPE_MEMREF_INPUT,
	                             TEE_PARAM_TYPE_MEMREF_INPUT,
	                             TEE_PARAM_TYPE_MEMREF_OUTPUT))
		return TEE_ERROR_BAD_PARAMETERS;
	if (!g_kek_ready)
		return TEE_ERROR_BAD_STATE;
	d = ta_pqc_dims(params[0].value.a);
	if (!d || !d->is_kem)
		return TEE_ERROR_BAD_PARAMETERS;
	if (!params[2].memref.buffer || params[2].memref.size != d->ct_len)
		return TEE_ERROR_BAD_PARAMETERS;
	if (params[3].memref.size < d->ss_len) {
		params[3].memref.size = d->ss_len;
		return TEE_ERROR_SHORT_BUFFER;
	}
	res = unwrap_sk(params[1].memref.buffer, params[1].memref.size,
	                sk, d->sk_len);
	if (res != TEE_SUCCESS)
		return res;
	if (ta_pqc_decaps(d->alg, sk, params[2].memref.buffer,
	                  params[3].memref.buffer) != 0)
		res = TEE_ERROR_GENERIC;
	else
		params[3].memref.size = d->ss_len;
	pqchsm_bzero(sk, sizeof(sk));
	return res;
}

static TEE_Result cmd_sign(uint32_t types, TEE_Param params[4])
{
	const ta_pqc_dims_t *d;
	const uint8_t       *frame;
	uint8_t             sk[TA_PQCHSM_MAX_PT];
	size_t              frame_len, ctx_len, msg_len, sig_len;
	TEE_Result          res;

	if (types != TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
	                             TEE_PARAM_TYPE_MEMREF_INPUT,
	                             TEE_PARAM_TYPE_MEMREF_INPUT,
	                             TEE_PARAM_TYPE_MEMREF_INOUT))
		return TEE_ERROR_BAD_PARAMETERS;
	if (!g_kek_ready)
		return TEE_ERROR_BAD_STATE;
	d = ta_pqc_dims(params[0].value.a);
	if (!d || d->is_kem)
		return TEE_ERROR_BAD_PARAMETERS;

	/* 消息帧：ctx_len(1B) ‖ ctx ‖ msg */
	frame     = params[2].memref.buffer;
	frame_len = params[2].memref.size;
	if (!frame || frame_len < 1 || frame_len > TA_PQCHSM_MAX_MSG + 1)
		return TEE_ERROR_BAD_PARAMETERS;
	ctx_len = frame[0];
	if (frame_len < 1 + ctx_len)
		return TEE_ERROR_BAD_PARAMETERS;
	msg_len = frame_len - 1 - ctx_len;

	if (params[3].memref.size < d->sig_len) {
		params[3].memref.size = d->sig_len;
		return TEE_ERROR_SHORT_BUFFER;
	}
	res = unwrap_sk(params[1].memref.buffer, params[1].memref.size,
	                sk, d->sk_len);
	if (res != TEE_SUCCESS)
		return res;
	sig_len = d->sig_len;
	if (ta_pqc_sign(d->alg, sk, frame + 1 + ctx_len, msg_len,
	                ctx_len ? frame + 1 : NULL, ctx_len,
	                params[3].memref.buffer, &sig_len) != 0)
		res = TEE_ERROR_GENERIC;
	else
		params[3].memref.size = sig_len;
	pqchsm_bzero(sk, sizeof(sk));
	return res;
}

/* ---- TA 框架入口 -------------------------------------------------------- */

TEE_Result TA_CreateEntryPoint(void)
{
	return TEE_SUCCESS;
}

void TA_DestroyEntryPoint(void)
{
	/* 进程消亡前抹掉缓存的密钥材料 */
	pqchsm_bzero(g_kdr, sizeof(g_kdr));
	pqchsm_bzero(g_kek, sizeof(g_kek));
	g_kdr_ready = 0;
	g_kek_ready = 0;
}

/* ============================================================================
 * 【会话登录类型：不再接受 TEEC_LOGIN_PUBLIC（PS-22）】
 * ============================================================================
 * PUBLIC 的含义是"调用方是谁完全不记录、也不检查" —— 普通世界里**任何**
 * 进程都能开这个 TA 的会话。配上以前那几条谕言机命令，那就是任何本地进程
 * 都能把存储 KEK 要走。命令删掉之后风险小了很多，但"谁都能连"这件事本身
 * 仍然不该是一台密码机的样子：TA 至少要能说出调用方是哪个 uid，
 * 出了事才有得查。
 *
 * 这里要求 TEE_LOGIN_USER（客户端用 TEEC_LOGIN_USER 打开，OP-TEE 把调用
 * 进程的 uid 放进身份里）。GROUP / APPLICATION 那几种更严的也放行 ——
 * 收紧方向的东西不该被这道闸门挡住。
 *
 * ⚠️ 这**不是**访问控制，只是身份记录 + 拒绝"匿名"。真正的按身份授权
 * （上游的 CFG_PKCS11_TA_AUTH_TEE_IDENTITY 那一套）属于批 2，见 FINAL-PLAN §3.2。
 *
 * ⚠️ 上板 pending：这条路径**没有在真 OP-TEE 上跑过**（BL32 入口那个死结还在，
 * 见 FINAL-PLAN §10）。TEE_GetPropertyAsIdentity 的行为按 GP 规范写，
 * 未在硅上验证。
 */
TEE_Result TA_OpenSessionEntryPoint(uint32_t param_types,
                                    TEE_Param params[4] __maybe_unused,
                                    void **session_ctx __maybe_unused)
{
	TEE_Identity id;
	TEE_Result   res;

	if (param_types != TEE_PARAM_TYPES(TEE_PARAM_TYPE_NONE,
	                                   TEE_PARAM_TYPE_NONE,
	                                   TEE_PARAM_TYPE_NONE,
	                                   TEE_PARAM_TYPE_NONE))
		return TEE_ERROR_BAD_PARAMETERS;

	memset(&id, 0, sizeof(id));
	res = TEE_GetPropertyAsIdentity(TEE_PROPSET_CURRENT_CLIENT,
	                                "gpd.client.identity", &id);
	if (res != TEE_SUCCESS)
		return TEE_ERROR_ACCESS_DENIED;   /* 问不出身份就不开会话 */
	if (id.login == TEE_LOGIN_PUBLIC)
		return TEE_ERROR_ACCESS_DENIED;

	return TEE_SUCCESS;
}

void TA_CloseSessionEntryPoint(void *session_ctx __maybe_unused)
{
}

TEE_Result TA_InvokeCommandEntryPoint(void *session_ctx __maybe_unused,
                                      uint32_t cmd_id, uint32_t param_types,
                                      TEE_Param params[4])
{
	switch (cmd_id) {
	case TA_PQCHSM_CMD_GET_INFO:
		return cmd_get_info(param_types, params);
	case TA_PQCHSM_CMD_KEK_SET:
		return cmd_kek_set(param_types, params);
	/* 编号 1/3/4（KDF_DERIVE / WRAP / UNWRAP）已删除，见
	 * pqchsm_ta_proto.h 里那段墓碑注释。**这里不写 case** ——
	 * 它们落到 default 的 TEE_ERROR_NOT_SUPPORTED，与"这个 TA 从来没有
	 * 过这条命令"完全一致。特意不给它们一个"已废弃"的专用错误码：
	 * 那等于向调用方确认这里曾经有个口子。 */
	case TA_PQCHSM_CMD_KEYGEN:
		return cmd_keygen(param_types, params);
	case TA_PQCHSM_CMD_KEYGEN_FROM_SEED:
		return cmd_keygen_from_seed(param_types, params);
	case TA_PQCHSM_CMD_DECAPS:
		return cmd_decaps(param_types, params);
	case TA_PQCHSM_CMD_SIGN:
		return cmd_sign(param_types, params);
	default:
		return TEE_ERROR_NOT_SUPPORTED;
	}
}
