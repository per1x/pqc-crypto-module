/* p11_module.c —— PKCS#11 v3.2 前端（路线图 Phase 9 第 1 项）
 *
 * 把已有的槽位管理器包装成一个标准 PKCS#11 动态库，用 OpenSC 的 pkcs11-tool
 * 驱动、与 SoftHSMv2 对比行为。
 *
 * 【为什么这一层几乎是"翻译"而不是"实现"】
 * §7 的 slot/token/object/session 模型本来就是照 PKCS#11 设计的，所以映射是一一对应的：
 *
 *   PKCS#11 slot i        ↔  hsm_slot_id_t i
 *   C_OpenSession         ↔  hsm_session_open
 *   C_Login(CKU_SO/USER)  ↔  hsm_session_login(HSM_ROLE_SO/USER)
 *   C_GenerateKeyPair     ↔  hsm_slot_generate
 *   C_Sign                ↔  hsm_object_sign
 *   C_DestroyObject       ↔  hsm_object_destroy
 *
 * 对象句柄：本项目一槽位一密钥对（§7.6 的 8 KB/槽预算），所以一个已装载的槽位
 * 恰好对外呈现两个对象。私钥对象句柄直接用 hsm_handle_t，公钥对象句柄是它
 * 或上最高位——两者可互相推导，不需要额外的对象表。
 *
 * 【进程模型】
 * pkcs11-tool 每条命令都是一个新进程，所以状态必须落盘才有意义：
 * C_Initialize 载入密钥库，任何改动状态的操作立刻存回。
 *   PQCHSM_KEYSTORE  密钥库路径（默认 $HOME/.pqchsm/keystore.bin）
 *   PQCHSM_SLOTS     槽位数（默认 4）
 *
 * 【范围】
 * 实现的是**能把 pkcs11-tool 常用流程跑通**的子集，其余返回
 * CKR_FUNCTION_NOT_SUPPORTED。未实现的部分见 doc/现状与后续计划.md。
 * 摘要签名（C_SignUpdate/Final）、对象导入（C_CreateObject）、
 * 封装/解封装（C_EncapsulateKey/C_DecapsulateKey）目前未做。
 */
#include "p11_config.h"

#include "pqchsm/keystore.h"
#include "pqchsm/slot.h"
#include "pqchsm/util.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define P11_MANUFACTURER "pqc-hsm prototype"
#define P11_LIBRARY_DESC "pqc-hsm PKCS#11 v3.2 front-end"
#define P11_MODEL        "pqc-hsm-sw"
#define DEFAULT_SLOTS    4

/* 公钥对象句柄 = 私钥句柄 | 最高位 */
#define PUB_BIT (1ULL << 63)

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static hsm_token_t    *g_tok;
static int             g_init;
static char            g_ks_path[512];
static CK_ULONG        g_n_slots = DEFAULT_SLOTS;

/* 每个会话的查找游标与运算上下文 */
typedef struct {
	hsm_session_t sess;
	CK_SLOT_ID    slot;
	int           in_use;
	/* C_FindObjects */
	int           find_active;
	CK_OBJECT_HANDLE found[2];
	CK_ULONG      n_found;
	CK_ULONG      found_pos;
	/* C_SignInit / C_VerifyInit */
	int           sign_active, verify_active;
	CK_OBJECT_HANDLE sign_key, verify_key;
} p11_session_t;

#define MAX_P11_SESSIONS 32
static p11_session_t g_sessions[MAX_P11_SESSIONS];

/* ---- 小工具 ------------------------------------------------------------- */

/* PKCS#11 的定长字段用空格填充（不是 NUL），这点很容易写错 */
static void pad_str(CK_UTF8CHAR *dst, size_t n, const char *src)
{
	size_t l = src ? strlen(src) : 0;
	if (l > n) {
		l = n;
	}
	memcpy(dst, src, l);
	memset(dst + l, ' ', n - l);
}

static CK_RV map_status(hsm_status_t st)
{
	switch (st) {
	case HSM_OK:                 return CKR_OK;
	case HSM_ERR_BAD_ARG:        return CKR_ARGUMENTS_BAD;
	case HSM_ERR_BAD_STATE:      return CKR_OPERATION_NOT_INITIALIZED;
	case HSM_ERR_NOT_AUTHORIZED: return CKR_USER_NOT_LOGGED_IN;
	case HSM_ERR_PIN_INCORRECT:  return CKR_PIN_INCORRECT;
	case HSM_ERR_PIN_LOCKED:     return CKR_PIN_LOCKED;
	case HSM_ERR_BAD_HANDLE:     return CKR_OBJECT_HANDLE_INVALID;
	case HSM_ERR_USAGE_DENIED:   return CKR_KEY_FUNCTION_NOT_PERMITTED;
	case HSM_ERR_POLICY:         return CKR_ACTION_PROHIBITED;
	case HSM_ERR_INTEGRITY:      return CKR_DEVICE_ERROR;
	case HSM_ERR_SLOT_BUSY:      return CKR_OPERATION_ACTIVE;
	case HSM_ERR_CRYPTO:         return CKR_DEVICE_ERROR;
	case HSM_ERR_NOMEM:          return CKR_HOST_MEMORY;
	case HSM_ERR_FULL:           return CKR_SESSION_COUNT;
	}
	return CKR_GENERAL_ERROR;
}

static pqc_alg_t alg_from_param(CK_MECHANISM_TYPE mech, CK_ULONG param_set)
{
	if (mech == CKM_ML_DSA_KEY_PAIR_GEN || mech == CKM_ML_DSA) {
		switch (param_set) {
		case CKP_ML_DSA_44: return PQC_ALG_ML_DSA_44;
		case CKP_ML_DSA_65: return PQC_ALG_ML_DSA_65;
		case CKP_ML_DSA_87: return PQC_ALG_ML_DSA_87;
		default:            return PQC_ALG_NONE;
		}
	}
	if (mech == CKM_ML_KEM_KEY_PAIR_GEN || mech == CKM_ML_KEM) {
		switch (param_set) {
		case CKP_ML_KEM_512:  return PQC_ALG_ML_KEM_512;
		case CKP_ML_KEM_768:  return PQC_ALG_ML_KEM_768;
		case CKP_ML_KEM_1024: return PQC_ALG_ML_KEM_1024;
		default:              return PQC_ALG_NONE;
		}
	}
	return PQC_ALG_NONE;
}

static CK_ULONG param_from_alg(pqc_alg_t a)
{
	switch (a) {
	case PQC_ALG_ML_DSA_44:   return CKP_ML_DSA_44;
	case PQC_ALG_ML_DSA_65:   return CKP_ML_DSA_65;
	case PQC_ALG_ML_DSA_87:   return CKP_ML_DSA_87;
	case PQC_ALG_ML_KEM_512:  return CKP_ML_KEM_512;
	case PQC_ALG_ML_KEM_768:  return CKP_ML_KEM_768;
	case PQC_ALG_ML_KEM_1024: return CKP_ML_KEM_1024;
	default:                  return 0;
	}
}

static CK_KEY_TYPE keytype_from_alg(pqc_alg_t a)
{
	const pqc_alg_info_t *i = pqc_alg_info(a);
	if (!i) {
		return CKK_VENDOR_DEFINED;
	}
	return i->kind == PQC_KIND_KEM ? CKK_ML_KEM : CKK_ML_DSA;
}

static void persist(void)
{
	if (g_tok && g_ks_path[0]) {
		(void)hsm_keystore_save(g_tok, g_ks_path);
	}
}

static p11_session_t *sess_at(CK_SESSION_HANDLE h)
{
	if (h == 0 || h > MAX_P11_SESSIONS) {
		return NULL;
	}
	p11_session_t *s = &g_sessions[h - 1];
	return s->in_use ? s : NULL;
}

static hsm_slot_id_t slot_of_handle(CK_OBJECT_HANDLE h)
{
	return (hsm_slot_id_t)(((h & ~PUB_BIT) & 0xffffffffu) - 1);
}

/* 由槽位当前 generation 推出私钥对象句柄；未装载返回 0 */
static CK_OBJECT_HANDLE priv_handle(hsm_slot_id_t slot)
{
	slot_meta_t m;
	if (hsm_slot_get_meta(g_tok, slot, &m) != HSM_OK) {
		return 0;
	}
	if (m.state != SLOT_ST_LOADED && m.state != SLOT_ST_IN_USE) {
		return 0;
	}
	return ((CK_OBJECT_HANDLE)m.generation << 32) | (CK_OBJECT_HANDLE)(slot + 1);
}

/* ---- 通用接口 ----------------------------------------------------------- */

CK_DEFINE_FUNCTION(CK_RV, C_Initialize)(CK_VOID_PTR pInitArgs)
{
	(void)pInitArgs;
	pthread_mutex_lock(&g_lock);
	CK_RV rv = CKR_OK;
	if (g_init) {
		rv = CKR_CRYPTOKI_ALREADY_INITIALIZED;
		goto out;
	}
	{
		const char *p = getenv("PQCHSM_KEYSTORE");
		if (p && *p) {
			snprintf(g_ks_path, sizeof(g_ks_path), "%s", p);
		} else {
			const char *home = getenv("HOME");
			snprintf(g_ks_path, sizeof(g_ks_path), "%s/.pqchsm", home ? home : "/tmp");
			(void)mkdir(g_ks_path, 0700);
			snprintf(g_ks_path, sizeof(g_ks_path), "%s/.pqchsm/keystore.bin",
			         home ? home : "/tmp");
		}
		const char *ns = getenv("PQCHSM_SLOTS");
		if (ns && *ns) {
			long v = strtol(ns, NULL, 10);
			if (v > 0 && v <= 64) {
				g_n_slots = (CK_ULONG)v;
			}
		}
	}
	g_tok = hsm_token_new((size_t)g_n_slots);
	if (!g_tok) {
		rv = CKR_HOST_MEMORY;
		goto out;
	}
	/* 库存在就载入；不存在是正常的首次运行 */
	if (hsm_keystore_load(g_tok, g_ks_path) != HSM_OK) {
		/* 载入失败可能是文件不存在，也可能是被篡改。前者正常，后者必须暴露：
		 * 用文件是否存在来区分。 */
		FILE *f = fopen(g_ks_path, "rb");
		if (f) {
			fclose(f);
			hsm_token_free(g_tok);
			g_tok = NULL;
			rv = CKR_DEVICE_ERROR;   /* 库存在但打不开 = 完整性问题 */
			goto out;
		}
	}
	memset(g_sessions, 0, sizeof(g_sessions));
	g_init = 1;
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

CK_DEFINE_FUNCTION(CK_RV, C_Finalize)(CK_VOID_PTR pReserved)
{
	if (pReserved) {
		return CKR_ARGUMENTS_BAD;
	}
	pthread_mutex_lock(&g_lock);
	CK_RV rv = CKR_OK;
	if (!g_init) {
		rv = CKR_CRYPTOKI_NOT_INITIALIZED;
	} else {
		persist();
		hsm_token_free(g_tok);
		g_tok = NULL;
		memset(g_sessions, 0, sizeof(g_sessions));
		g_init = 0;
	}
	pthread_mutex_unlock(&g_lock);
	return rv;
}

CK_DEFINE_FUNCTION(CK_RV, C_GetInfo)(CK_INFO_PTR pInfo)
{
	if (!pInfo) {
		return CKR_ARGUMENTS_BAD;
	}
	memset(pInfo, 0, sizeof(*pInfo));
	pInfo->cryptokiVersion.major = CRYPTOKI_VERSION_MAJOR;
	pInfo->cryptokiVersion.minor = CRYPTOKI_VERSION_MINOR;
	pad_str(pInfo->manufacturerID, sizeof(pInfo->manufacturerID), P11_MANUFACTURER);
	pad_str(pInfo->libraryDescription, sizeof(pInfo->libraryDescription), P11_LIBRARY_DESC);
	pInfo->libraryVersion.major = 0;
	pInfo->libraryVersion.minor = 1;
	return CKR_OK;
}

CK_DEFINE_FUNCTION(CK_RV, C_GetSlotList)(CK_BBOOL tokenPresent, CK_SLOT_ID_PTR pSlotList,
                                         CK_ULONG_PTR pulCount)
{
	(void)tokenPresent;   /* 本实现的槽位恒定存在（软件槽位，不可插拔） */
	if (!pulCount) {
		return CKR_ARGUMENTS_BAD;
	}
	pthread_mutex_lock(&g_lock);
	CK_RV rv = CKR_OK;
	if (!g_init) {
		rv = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto out;
	}
	if (!pSlotList) {
		*pulCount = g_n_slots;
		goto out;
	}
	if (*pulCount < g_n_slots) {
		*pulCount = g_n_slots;
		rv = CKR_BUFFER_TOO_SMALL;
		goto out;
	}
	for (CK_ULONG i = 0; i < g_n_slots; i++) {
		pSlotList[i] = i;
	}
	*pulCount = g_n_slots;
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

CK_DEFINE_FUNCTION(CK_RV, C_GetSlotInfo)(CK_SLOT_ID slotID, CK_SLOT_INFO_PTR pInfo)
{
	if (!pInfo) {
		return CKR_ARGUMENTS_BAD;
	}
	pthread_mutex_lock(&g_lock);
	CK_RV rv = CKR_OK;
	if (!g_init) {
		rv = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto out;
	}
	if (slotID >= g_n_slots) {
		rv = CKR_SLOT_ID_INVALID;
		goto out;
	}
	memset(pInfo, 0, sizeof(*pInfo));
	{
		char d[64];
		snprintf(d, sizeof(d), "pqc-hsm software slot %lu", (unsigned long)slotID);
		pad_str(pInfo->slotDescription, sizeof(pInfo->slotDescription), d);
	}
	pad_str(pInfo->manufacturerID, sizeof(pInfo->manufacturerID), P11_MANUFACTURER);
	pInfo->flags = CKF_TOKEN_PRESENT;
	pInfo->hardwareVersion.major = 0;
	pInfo->firmwareVersion.major = 0;
	pInfo->firmwareVersion.minor = 1;
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

CK_DEFINE_FUNCTION(CK_RV, C_GetTokenInfo)(CK_SLOT_ID slotID, CK_TOKEN_INFO_PTR pInfo)
{
	if (!pInfo) {
		return CKR_ARGUMENTS_BAD;
	}
	pthread_mutex_lock(&g_lock);
	CK_RV rv = CKR_OK;
	if (!g_init) {
		rv = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto out;
	}
	if (slotID >= g_n_slots) {
		rv = CKR_SLOT_ID_INVALID;
		goto out;
	}
	{
		slot_meta_t m;
		int so_set = 0, user_set = 0;
		if (hsm_slot_get_meta(g_tok, (hsm_slot_id_t)slotID, &m) != HSM_OK) {
			rv = CKR_DEVICE_ERROR;
			goto out;
		}
		(void)hsm_slot_pin_status(g_tok, (hsm_slot_id_t)slotID, &so_set, &user_set);

		memset(pInfo, 0, sizeof(*pInfo));
		pad_str(pInfo->label, sizeof(pInfo->label),
		        m.label[0] ? m.label : "(uninitialized)");
		pad_str(pInfo->manufacturerID, sizeof(pInfo->manufacturerID), P11_MANUFACTURER);
		pad_str(pInfo->model, sizeof(pInfo->model), P11_MODEL);
		{
			char sn[32];
			snprintf(sn, sizeof(sn), "%08lu", (unsigned long)slotID);
			pad_str(pInfo->serialNumber, sizeof(pInfo->serialNumber), sn);
		}
		pInfo->flags = CKF_LOGIN_REQUIRED | CKF_TOKEN_INITIALIZED;
		if (m.state == SLOT_ST_UNINIT) {
			pInfo->flags &= (CK_FLAGS)~CKF_TOKEN_INITIALIZED;
		}
		if (user_set) {
			pInfo->flags |= CKF_USER_PIN_INITIALIZED;
		}
		if (m.state == SLOT_ST_LOCKED) {
			pInfo->flags |= CKF_USER_PIN_LOCKED;
		}
		if (so_set) {
			pInfo->flags |= CKF_RNG;   /* 借位表示已供应；RNG 也确实有 */
		}
		pInfo->ulMaxSessionCount    = MAX_P11_SESSIONS;
		pInfo->ulSessionCount       = CK_UNAVAILABLE_INFORMATION;
		pInfo->ulMaxRwSessionCount  = MAX_P11_SESSIONS;
		pInfo->ulRwSessionCount     = CK_UNAVAILABLE_INFORMATION;
		pInfo->ulMaxPinLen          = HSM_PIN_MAX_LEN;
		pInfo->ulMinPinLen          = HSM_PIN_MIN_LEN;
		pInfo->ulTotalPublicMemory  = CK_UNAVAILABLE_INFORMATION;
		pInfo->ulFreePublicMemory   = CK_UNAVAILABLE_INFORMATION;
		pInfo->ulTotalPrivateMemory = CK_UNAVAILABLE_INFORMATION;
		pInfo->ulFreePrivateMemory  = CK_UNAVAILABLE_INFORMATION;
		pInfo->firmwareVersion.minor = 1;
	}
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

static const CK_MECHANISM_TYPE MECHS[] = {
	CKM_ML_DSA_KEY_PAIR_GEN, CKM_ML_DSA,
	CKM_ML_KEM_KEY_PAIR_GEN, CKM_ML_KEM,
};

CK_DEFINE_FUNCTION(CK_RV, C_GetMechanismList)(CK_SLOT_ID slotID,
                                              CK_MECHANISM_TYPE_PTR pMechanismList,
                                              CK_ULONG_PTR pulCount)
{
	if (!pulCount) {
		return CKR_ARGUMENTS_BAD;
	}
	if (slotID >= g_n_slots) {
		return CKR_SLOT_ID_INVALID;
	}
	CK_ULONG n = sizeof(MECHS) / sizeof(MECHS[0]);
	if (!pMechanismList) {
		*pulCount = n;
		return CKR_OK;
	}
	if (*pulCount < n) {
		*pulCount = n;
		return CKR_BUFFER_TOO_SMALL;
	}
	memcpy(pMechanismList, MECHS, sizeof(MECHS));
	*pulCount = n;
	return CKR_OK;
}

CK_DEFINE_FUNCTION(CK_RV, C_GetMechanismInfo)(CK_SLOT_ID slotID, CK_MECHANISM_TYPE type,
                                              CK_MECHANISM_INFO_PTR pInfo)
{
	if (!pInfo) {
		return CKR_ARGUMENTS_BAD;
	}
	if (slotID >= g_n_slots) {
		return CKR_SLOT_ID_INVALID;
	}
	memset(pInfo, 0, sizeof(*pInfo));
	switch (type) {
	case CKM_ML_DSA_KEY_PAIR_GEN:
		pInfo->flags = CKF_GENERATE_KEY_PAIR;
		break;
	case CKM_ML_DSA:
		pInfo->flags = CKF_SIGN | CKF_VERIFY;
		break;
	case CKM_ML_KEM_KEY_PAIR_GEN:
		pInfo->flags = CKF_GENERATE_KEY_PAIR;
		break;
	case CKM_ML_KEM:
		pInfo->flags = CKF_ENCAPSULATE | CKF_DECAPSULATE;
		break;
	default:
		return CKR_MECHANISM_INVALID;
	}
	return CKR_OK;
}

CK_DEFINE_FUNCTION(CK_RV, C_InitToken)(CK_SLOT_ID slotID, CK_UTF8CHAR_PTR pPin,
                                       CK_ULONG ulPinLen, CK_UTF8CHAR_PTR pLabel)
{
	pthread_mutex_lock(&g_lock);
	CK_RV rv = CKR_OK;
	if (!g_init) {
		rv = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto out;
	}
	if (slotID >= g_n_slots || !pPin) {
		rv = CKR_ARGUMENTS_BAD;
		goto out;
	}
	{
		char pin[HSM_PIN_MAX_LEN + 1], label[SLOT_LABEL_MAX];
		if (ulPinLen > HSM_PIN_MAX_LEN) {
			rv = CKR_PIN_LEN_RANGE;
			goto out;
		}
		memcpy(pin, pPin, ulPinLen);
		pin[ulPinLen] = '\0';
		/* PKCS#11 的 label 是 32 字节空格填充、不带 NUL —— 要自己收尾并去掉尾部空格 */
		memset(label, 0, sizeof(label));
		if (pLabel) {
			memcpy(label, pLabel, SLOT_LABEL_MAX - 1);
			for (int i = SLOT_LABEL_MAX - 2; i >= 0 && label[i] == ' '; i--) {
				label[i] = '\0';
			}
		}
		hsm_status_t st = hsm_slot_init_token(g_tok, (hsm_slot_id_t)slotID,
		                                      label[0] ? label : "token", pin);
		if (st != HSM_OK) {
			/* 已初始化的槽位再 InitToken：PKCS#11 语义是"重新初始化"，
			 * 但那等于销毁全部内容，本实现要求先显式 zeroize —— 拒绝更安全。 */
			rv = (st == HSM_ERR_BAD_STATE) ? CKR_ACTION_PROHIBITED : map_status(st);
			goto out;
		}
		persist();
		pqc_secure_zero(pin, sizeof(pin));
	}
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

CK_DEFINE_FUNCTION(CK_RV, C_InitPIN)(CK_SESSION_HANDLE hSession, CK_UTF8CHAR_PTR pPin,
                                     CK_ULONG ulPinLen)
{
	pthread_mutex_lock(&g_lock);
	CK_RV rv = CKR_OK;
	p11_session_t *s = NULL;
	if (!g_init) {
		rv = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto out;
	}
	s = sess_at(hSession);
	if (!s) {
		rv = CKR_SESSION_HANDLE_INVALID;
		goto out;
	}
	if (!pPin || ulPinLen > HSM_PIN_MAX_LEN) {
		rv = CKR_PIN_LEN_RANGE;
		goto out;
	}
	{
		char pin[HSM_PIN_MAX_LEN + 1];
		memcpy(pin, pPin, ulPinLen);
		pin[ulPinLen] = '\0';
		rv = map_status(hsm_slot_set_user_pin(g_tok, s->sess, pin));
		pqc_secure_zero(pin, sizeof(pin));
		if (rv == CKR_OK) {
			persist();
		}
	}
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

/* ---- 会话 --------------------------------------------------------------- */

CK_DEFINE_FUNCTION(CK_RV, C_OpenSession)(CK_SLOT_ID slotID, CK_FLAGS flags,
                                         CK_VOID_PTR pApplication, CK_NOTIFY Notify,
                                         CK_SESSION_HANDLE_PTR phSession)
{
	(void)pApplication;
	(void)Notify;
	pthread_mutex_lock(&g_lock);
	CK_RV rv = CKR_OK;
	if (!g_init) {
		rv = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto out;
	}
	if (slotID >= g_n_slots || !phSession) {
		rv = CKR_ARGUMENTS_BAD;
		goto out;
	}
	if (!(flags & CKF_SERIAL_SESSION)) {
		rv = CKR_SESSION_PARALLEL_NOT_SUPPORTED;
		goto out;
	}
	{
		int idx = -1;
		for (int i = 0; i < MAX_P11_SESSIONS; i++) {
			if (!g_sessions[i].in_use) {
				idx = i;
				break;
			}
		}
		if (idx < 0) {
			rv = CKR_SESSION_COUNT;
			goto out;
		}
		hsm_session_t hs;
		hsm_status_t st = hsm_session_open(g_tok, (hsm_slot_id_t)slotID, &hs);
		if (st != HSM_OK) {
			rv = map_status(st);
			goto out;
		}
		memset(&g_sessions[idx], 0, sizeof(p11_session_t));
		g_sessions[idx].in_use = 1;
		g_sessions[idx].sess = hs;
		g_sessions[idx].slot = slotID;
		*phSession = (CK_SESSION_HANDLE)(idx + 1);
	}
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

CK_DEFINE_FUNCTION(CK_RV, C_CloseSession)(CK_SESSION_HANDLE hSession)
{
	pthread_mutex_lock(&g_lock);
	CK_RV rv = CKR_OK;
	p11_session_t *s = NULL;
	if (!g_init) {
		rv = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto out;
	}
	s = sess_at(hSession);
	if (!s) {
		rv = CKR_SESSION_HANDLE_INVALID;
		goto out;
	}
	(void)hsm_session_close(g_tok, s->sess);
	memset(s, 0, sizeof(*s));
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

CK_DEFINE_FUNCTION(CK_RV, C_CloseAllSessions)(CK_SLOT_ID slotID)
{
	(void)slotID;
	pthread_mutex_lock(&g_lock);
	for (int i = 0; i < MAX_P11_SESSIONS; i++) {
		if (g_sessions[i].in_use) {
			(void)hsm_session_close(g_tok, g_sessions[i].sess);
			memset(&g_sessions[i], 0, sizeof(p11_session_t));
		}
	}
	pthread_mutex_unlock(&g_lock);
	return CKR_OK;
}

CK_DEFINE_FUNCTION(CK_RV, C_GetSessionInfo)(CK_SESSION_HANDLE hSession,
                                            CK_SESSION_INFO_PTR pInfo)
{
	pthread_mutex_lock(&g_lock);
	CK_RV rv = CKR_OK;
	p11_session_t *s = NULL;
	if (!g_init) {
		rv = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto out;
	}
	s = sess_at(hSession);
	if (!s || !pInfo) {
		rv = s ? CKR_ARGUMENTS_BAD : CKR_SESSION_HANDLE_INVALID;
		goto out;
	}
	{
		hsm_role_t role = HSM_ROLE_PUBLIC;
		(void)hsm_session_role(g_tok, s->sess, &role);
		memset(pInfo, 0, sizeof(*pInfo));
		pInfo->slotID = s->slot;
		pInfo->state = (role == HSM_ROLE_SO)   ? CKS_RW_SO_FUNCTIONS
		             : (role == HSM_ROLE_USER) ? CKS_RW_USER_FUNCTIONS
		                                       : CKS_RW_PUBLIC_SESSION;
		pInfo->flags = CKF_SERIAL_SESSION | CKF_RW_SESSION;
	}
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

CK_DEFINE_FUNCTION(CK_RV, C_Login)(CK_SESSION_HANDLE hSession, CK_USER_TYPE userType,
                                   CK_UTF8CHAR_PTR pPin, CK_ULONG ulPinLen)
{
	pthread_mutex_lock(&g_lock);
	CK_RV rv = CKR_OK;
	p11_session_t *s = NULL;
	if (!g_init) {
		rv = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto out;
	}
	s = sess_at(hSession);
	if (!s) {
		rv = CKR_SESSION_HANDLE_INVALID;
		goto out;
	}
	if (!pPin || ulPinLen > HSM_PIN_MAX_LEN) {
		rv = CKR_PIN_LEN_RANGE;
		goto out;
	}
	{
		hsm_role_t role;
		if (userType == CKU_SO) {
			role = HSM_ROLE_SO;
		} else if (userType == CKU_USER) {
			role = HSM_ROLE_USER;
		} else {
			rv = CKR_USER_TYPE_INVALID;
			goto out;
		}
		char pin[HSM_PIN_MAX_LEN + 1];
		memcpy(pin, pPin, ulPinLen);
		pin[ulPinLen] = '\0';
		hsm_status_t st = hsm_session_login(g_tok, s->sess, role, pin);
		pqc_secure_zero(pin, sizeof(pin));
		rv = map_status(st);
		/* 失败计数与锁定状态是持久化的，登录失败也要落盘 */
		persist();
	}
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

CK_DEFINE_FUNCTION(CK_RV, C_Logout)(CK_SESSION_HANDLE hSession)
{
	pthread_mutex_lock(&g_lock);
	CK_RV rv = CKR_OK;
	p11_session_t *s = NULL;
	if (!g_init) {
		rv = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto out;
	}
	s = sess_at(hSession);
	if (!s) {
		rv = CKR_SESSION_HANDLE_INVALID;
		goto out;
	}
	rv = map_status(hsm_session_logout(g_tok, s->sess));
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

/* ---- 对象 --------------------------------------------------------------- */

/* 在模板里找一个属性 */
static const CK_ATTRIBUTE *find_attr(CK_ATTRIBUTE_PTR t, CK_ULONG n, CK_ATTRIBUTE_TYPE type)
{
	for (CK_ULONG i = 0; i < n; i++) {
		if (t[i].type == type) {
			return &t[i];
		}
	}
	return NULL;
}

static CK_ULONG attr_ulong(const CK_ATTRIBUTE *a, CK_ULONG dflt)
{
	if (!a || !a->pValue || a->ulValueLen != sizeof(CK_ULONG)) {
		return dflt;
	}
	return *(CK_ULONG *)a->pValue;
}

CK_DEFINE_FUNCTION(CK_RV, C_GenerateKeyPair)(
	CK_SESSION_HANDLE hSession, CK_MECHANISM_PTR pMechanism,
	CK_ATTRIBUTE_PTR pPublicKeyTemplate, CK_ULONG ulPublicKeyAttributeCount,
	CK_ATTRIBUTE_PTR pPrivateKeyTemplate, CK_ULONG ulPrivateKeyAttributeCount,
	CK_OBJECT_HANDLE_PTR phPublicKey, CK_OBJECT_HANDLE_PTR phPrivateKey)
{
	pthread_mutex_lock(&g_lock);
	CK_RV rv = CKR_OK;
	p11_session_t *s = NULL;
	if (!g_init) {
		rv = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto out;
	}
	s = sess_at(hSession);
	if (!s || !pMechanism || !phPublicKey || !phPrivateKey) {
		rv = s ? CKR_ARGUMENTS_BAD : CKR_SESSION_HANDLE_INVALID;
		goto out;
	}
	{
		/* 参数集可以放在公钥或私钥模板里，两边都找 */
		const CK_ATTRIBUTE *a = find_attr(pPublicKeyTemplate, ulPublicKeyAttributeCount,
		                                  CKA_PARAMETER_SET);
		if (!a) {
			a = find_attr(pPrivateKeyTemplate, ulPrivateKeyAttributeCount,
			              CKA_PARAMETER_SET);
		}
		CK_ULONG pset = attr_ulong(a, 0);
		pqc_alg_t alg = alg_from_param(pMechanism->mechanism, pset);
		if (alg == PQC_ALG_NONE) {
			rv = (pset == 0) ? CKR_TEMPLATE_INCOMPLETE : CKR_ATTRIBUTE_VALUE_INVALID;
			goto out;
		}
		const pqc_alg_info_t *info = pqc_alg_info(alg);
		uint32_t usage = (info->kind == PQC_KIND_SIG)
		                 ? (uint32_t)KEY_USAGE_SIGN : (uint32_t)KEY_USAGE_DECAP;

		hsm_handle_t h = HSM_INVALID_HANDLE;
		hsm_status_t st = hsm_slot_generate(g_tok, s->sess, alg, usage, 0, &h);
		if (st != HSM_OK) {
			rv = map_status(st);
			goto out;
		}
		persist();
		*phPrivateKey = (CK_OBJECT_HANDLE)h;
		*phPublicKey  = (CK_OBJECT_HANDLE)h | PUB_BIT;
	}
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

CK_DEFINE_FUNCTION(CK_RV, C_DestroyObject)(CK_SESSION_HANDLE hSession,
                                           CK_OBJECT_HANDLE hObject)
{
	pthread_mutex_lock(&g_lock);
	CK_RV rv = CKR_OK;
	p11_session_t *s = NULL;
	if (!g_init) {
		rv = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto out;
	}
	s = sess_at(hSession);
	if (!s) {
		rv = CKR_SESSION_HANDLE_INVALID;
		goto out;
	}
	/* 公钥与私钥是同一对，销毁任一即销毁整对 —— 本模型一槽一对 */
	rv = map_status(hsm_object_destroy(g_tok, s->sess, (hsm_handle_t)(hObject & ~PUB_BIT)));
	if (rv == CKR_OK) {
		persist();
	}
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

CK_DEFINE_FUNCTION(CK_RV, C_FindObjectsInit)(CK_SESSION_HANDLE hSession,
                                             CK_ATTRIBUTE_PTR pTemplate, CK_ULONG ulCount)
{
	pthread_mutex_lock(&g_lock);
	CK_RV rv = CKR_OK;
	p11_session_t *s = NULL;
	if (!g_init) {
		rv = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto out;
	}
	s = sess_at(hSession);
	if (!s) {
		rv = CKR_SESSION_HANDLE_INVALID;
		goto out;
	}
	if (s->find_active) {
		rv = CKR_OPERATION_ACTIVE;
		goto out;
	}
	{
		/* 只支持按 CKA_CLASS 过滤；其余属性忽略（在文档里说明） */
		const CK_ATTRIBUTE *cls = find_attr(pTemplate, ulCount, CKA_CLASS);
		CK_ULONG want = attr_ulong(cls, (CK_ULONG)-1);

		s->n_found = 0;
		s->found_pos = 0;
		/* 会话绑定在某个槽位上，所以只可能找到该槽位的对象（一槽一对） */
		{
			CK_OBJECT_HANDLE priv = priv_handle((hsm_slot_id_t)s->slot);
			if (priv) {
				if (want == (CK_ULONG)-1 || want == CKO_PUBLIC_KEY) {
					s->found[s->n_found++] = priv | PUB_BIT;
				}
				if (want == (CK_ULONG)-1 || want == CKO_PRIVATE_KEY) {
					s->found[s->n_found++] = priv;
				}
			}
		}
		s->find_active = 1;
	}
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

CK_DEFINE_FUNCTION(CK_RV, C_FindObjects)(CK_SESSION_HANDLE hSession,
                                         CK_OBJECT_HANDLE_PTR phObject,
                                         CK_ULONG ulMaxObjectCount,
                                         CK_ULONG_PTR pulObjectCount)
{
	pthread_mutex_lock(&g_lock);
	CK_RV rv = CKR_OK;
	p11_session_t *s = NULL;
	if (!g_init) {
		rv = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto out;
	}
	s = sess_at(hSession);
	if (!s || !phObject || !pulObjectCount) {
		rv = s ? CKR_ARGUMENTS_BAD : CKR_SESSION_HANDLE_INVALID;
		goto out;
	}
	if (!s->find_active) {
		rv = CKR_OPERATION_NOT_INITIALIZED;
		goto out;
	}
	{
		CK_ULONG n = 0;
		while (n < ulMaxObjectCount && s->found_pos < s->n_found) {
			phObject[n++] = s->found[s->found_pos++];
		}
		*pulObjectCount = n;
	}
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

CK_DEFINE_FUNCTION(CK_RV, C_FindObjectsFinal)(CK_SESSION_HANDLE hSession)
{
	pthread_mutex_lock(&g_lock);
	CK_RV rv = CKR_OK;
	p11_session_t *s = NULL;
	if (!g_init) {
		rv = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto out;
	}
	s = sess_at(hSession);
	if (!s) {
		rv = CKR_SESSION_HANDLE_INVALID;
		goto out;
	}
	s->find_active = 0;
	s->n_found = 0;
	s->found_pos = 0;
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

static CK_RV fill_attr(CK_ATTRIBUTE *a, const void *val, size_t len)
{
	if (!a->pValue) {
		a->ulValueLen = (CK_ULONG)len;
		return CKR_OK;
	}
	if (a->ulValueLen < len) {
		a->ulValueLen = CK_UNAVAILABLE_INFORMATION;
		return CKR_BUFFER_TOO_SMALL;
	}
	memcpy(a->pValue, val, len);
	a->ulValueLen = (CK_ULONG)len;
	return CKR_OK;
}

CK_DEFINE_FUNCTION(CK_RV, C_GetAttributeValue)(CK_SESSION_HANDLE hSession,
                                               CK_OBJECT_HANDLE hObject,
                                               CK_ATTRIBUTE_PTR pTemplate, CK_ULONG ulCount)
{
	pthread_mutex_lock(&g_lock);
	CK_RV rv = CKR_OK;
	p11_session_t *s = NULL;
	if (!g_init) {
		rv = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto out;
	}
	s = sess_at(hSession);
	if (!s || !pTemplate) {
		rv = s ? CKR_ARGUMENTS_BAD : CKR_SESSION_HANDLE_INVALID;
		goto out;
	}
	{
		int is_pub = (hObject & PUB_BIT) != 0;
		hsm_handle_t priv = (hsm_handle_t)(hObject & ~PUB_BIT);
		slot_meta_t m;
		if (hsm_slot_get_meta(g_tok, slot_of_handle(hObject), &m) != HSM_OK) {
			rv = CKR_OBJECT_HANDLE_INVALID;
			goto out;
		}
		const pqc_alg_info_t *info = pqc_alg_info(m.alg);
		if (!info) {
			rv = CKR_OBJECT_HANDLE_INVALID;
			goto out;
		}
		for (CK_ULONG i = 0; i < ulCount; i++) {
			CK_ATTRIBUTE *a = &pTemplate[i];
			CK_RV r = CKR_OK;
			switch (a->type) {
			case CKA_CLASS: {
				CK_OBJECT_CLASS c = is_pub ? CKO_PUBLIC_KEY : CKO_PRIVATE_KEY;
				r = fill_attr(a, &c, sizeof(c));
				break;
			}
			case CKA_KEY_TYPE: {
				CK_KEY_TYPE k = keytype_from_alg(m.alg);
				r = fill_attr(a, &k, sizeof(k));
				break;
			}
			case CKA_PARAMETER_SET: {
				CK_ULONG p = param_from_alg(m.alg);
				r = fill_attr(a, &p, sizeof(p));
				break;
			}
			case CKA_LABEL:
				r = fill_attr(a, m.label, strlen(m.label));
				break;
			case CKA_ID: {
				uint8_t id = (uint8_t)m.slot_id;
				r = fill_attr(a, &id, 1);
				break;
			}
			case CKA_TOKEN: {
				CK_BBOOL b = CK_TRUE;
				r = fill_attr(a, &b, sizeof(b));
				break;
			}
			case CKA_PRIVATE: {
				CK_BBOOL b = is_pub ? CK_FALSE : CK_TRUE;
				r = fill_attr(a, &b, sizeof(b));
				break;
			}
			case CKA_SENSITIVE: {
				/* 私钥永远 sensitive —— 本项目根本没有导出明文私钥的入口 */
				CK_BBOOL b = is_pub ? CK_FALSE : CK_TRUE;
				r = fill_attr(a, &b, sizeof(b));
				break;
			}
			case CKA_EXTRACTABLE: {
				CK_BBOOL b = (m.policy & SLOT_POLICY_EXTRACTABLE) ? CK_TRUE : CK_FALSE;
				r = fill_attr(a, &b, sizeof(b));
				break;
			}
			case CKA_SIGN: {
				CK_BBOOL b = (!is_pub && (m.usage & KEY_USAGE_SIGN)) ? CK_TRUE : CK_FALSE;
				r = fill_attr(a, &b, sizeof(b));
				break;
			}
			case CKA_VERIFY: {
				CK_BBOOL b = (is_pub && (m.usage & KEY_USAGE_SIGN)) ? CK_TRUE : CK_FALSE;
				r = fill_attr(a, &b, sizeof(b));
				break;
			}
			case CKA_DECAPSULATE: {
				CK_BBOOL b = (!is_pub && (m.usage & KEY_USAGE_DECAP)) ? CK_TRUE : CK_FALSE;
				r = fill_attr(a, &b, sizeof(b));
				break;
			}
			case CKA_ENCAPSULATE: {
				CK_BBOOL b = (is_pub && (m.usage & KEY_USAGE_DECAP)) ? CK_TRUE : CK_FALSE;
				r = fill_attr(a, &b, sizeof(b));
				break;
			}
			case CKA_VALUE: {
				if (!is_pub) {
					/* 私钥的 CKA_VALUE 永远不可读 —— 这条正是 HSM 的意义 */
					a->ulValueLen = CK_UNAVAILABLE_INFORMATION;
					r = CKR_ATTRIBUTE_SENSITIVE;
					break;
				}
				uint8_t *pk = malloc(info->pk_len);
				size_t plen = 0;
				if (!pk) {
					r = CKR_HOST_MEMORY;
					break;
				}
				if (hsm_object_public_key(g_tok, s->sess, priv, pk, info->pk_len,
				                          &plen) != HSM_OK) {
					free(pk);
					r = CKR_OBJECT_HANDLE_INVALID;
					break;
				}
				r = fill_attr(a, pk, plen);
				free(pk);
				break;
			}
			default:
				a->ulValueLen = CK_UNAVAILABLE_INFORMATION;
				r = CKR_ATTRIBUTE_TYPE_INVALID;
				break;
			}
			if (r != CKR_OK && rv == CKR_OK) {
				rv = r;   /* 记住第一个错误但继续填其余属性（PKCS#11 要求） */
			}
		}
	}
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

/* ---- 签名与验签 --------------------------------------------------------- */

CK_DEFINE_FUNCTION(CK_RV, C_SignInit)(CK_SESSION_HANDLE hSession, CK_MECHANISM_PTR pMechanism,
                                      CK_OBJECT_HANDLE hKey)
{
	pthread_mutex_lock(&g_lock);
	CK_RV rv = CKR_OK;
	p11_session_t *s = NULL;
	if (!g_init) {
		rv = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto out;
	}
	s = sess_at(hSession);
	if (!s || !pMechanism) {
		rv = s ? CKR_ARGUMENTS_BAD : CKR_SESSION_HANDLE_INVALID;
		goto out;
	}
	if (pMechanism->mechanism != CKM_ML_DSA) {
		rv = CKR_MECHANISM_INVALID;
		goto out;
	}
	if (hKey & PUB_BIT) {
		rv = CKR_KEY_TYPE_INCONSISTENT;   /* 拿公钥来签名 */
		goto out;
	}
	s->sign_active = 1;
	s->sign_key = hKey;
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

CK_DEFINE_FUNCTION(CK_RV, C_Sign)(CK_SESSION_HANDLE hSession, CK_BYTE_PTR pData,
                                  CK_ULONG ulDataLen, CK_BYTE_PTR pSignature,
                                  CK_ULONG_PTR pulSignatureLen)
{
	pthread_mutex_lock(&g_lock);
	CK_RV rv = CKR_OK;
	p11_session_t *s = NULL;
	if (!g_init) {
		rv = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto out;
	}
	s = sess_at(hSession);
	if (!s || !pulSignatureLen) {
		rv = s ? CKR_ARGUMENTS_BAD : CKR_SESSION_HANDLE_INVALID;
		goto out;
	}
	if (!s->sign_active) {
		rv = CKR_OPERATION_NOT_INITIALIZED;
		goto out;
	}
	{
		slot_meta_t m;
		if (hsm_slot_get_meta(g_tok, slot_of_handle(s->sign_key), &m) != HSM_OK) {
			rv = CKR_OBJECT_HANDLE_INVALID;
			goto out;
		}
		const pqc_alg_info_t *info = pqc_alg_info(m.alg);
		if (!info || info->kind != PQC_KIND_SIG) {
			rv = CKR_KEY_TYPE_INCONSISTENT;
			goto out;
		}
		/* 只问长度 */
		if (!pSignature) {
			*pulSignatureLen = (CK_ULONG)info->sig_len;
			goto out;
		}
		if (*pulSignatureLen < info->sig_len) {
			*pulSignatureLen = (CK_ULONG)info->sig_len;
			rv = CKR_BUFFER_TOO_SMALL;
			goto out;
		}
		size_t sl = 0;
		hsm_status_t st = hsm_object_sign(g_tok, s->sess, (hsm_handle_t)s->sign_key,
		                                  pData, ulDataLen, NULL, 0,
		                                  pSignature, *pulSignatureLen, &sl);
		if (st != HSM_OK) {
			rv = map_status(st);
			goto out;
		}
		*pulSignatureLen = (CK_ULONG)sl;
		s->sign_active = 0;   /* 单次操作，完成即结束 */
		persist();            /* use_count 变了 */
	}
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

CK_DEFINE_FUNCTION(CK_RV, C_VerifyInit)(CK_SESSION_HANDLE hSession,
                                        CK_MECHANISM_PTR pMechanism, CK_OBJECT_HANDLE hKey)
{
	pthread_mutex_lock(&g_lock);
	CK_RV rv = CKR_OK;
	p11_session_t *s = NULL;
	if (!g_init) {
		rv = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto out;
	}
	s = sess_at(hSession);
	if (!s || !pMechanism) {
		rv = s ? CKR_ARGUMENTS_BAD : CKR_SESSION_HANDLE_INVALID;
		goto out;
	}
	if (pMechanism->mechanism != CKM_ML_DSA) {
		rv = CKR_MECHANISM_INVALID;
		goto out;
	}
	s->verify_active = 1;
	s->verify_key = hKey;
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

CK_DEFINE_FUNCTION(CK_RV, C_Verify)(CK_SESSION_HANDLE hSession, CK_BYTE_PTR pData,
                                    CK_ULONG ulDataLen, CK_BYTE_PTR pSignature,
                                    CK_ULONG ulSignatureLen)
{
	pthread_mutex_lock(&g_lock);
	CK_RV rv = CKR_OK;
	p11_session_t *s = NULL;
	if (!g_init) {
		rv = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto out;
	}
	s = sess_at(hSession);
	if (!s) {
		rv = CKR_SESSION_HANDLE_INVALID;
		goto out;
	}
	if (!s->verify_active) {
		rv = CKR_OPERATION_NOT_INITIALIZED;
		goto out;
	}
	{
		slot_meta_t m;
		if (hsm_slot_get_meta(g_tok, slot_of_handle(s->verify_key), &m) != HSM_OK) {
			rv = CKR_OBJECT_HANDLE_INVALID;
			goto out;
		}
		const pqc_alg_info_t *info = pqc_alg_info(m.alg);
		if (!info || info->kind != PQC_KIND_SIG) {
			rv = CKR_KEY_TYPE_INCONSISTENT;
			goto out;
		}
		/* 验签用公钥，不需要动私钥 —— 直接取出公钥在本地验 */
		uint8_t *pk = malloc(info->pk_len);
		size_t plen = 0;
		if (!pk) {
			rv = CKR_HOST_MEMORY;
			goto out;
		}
		hsm_handle_t priv = (hsm_handle_t)(s->verify_key & ~PUB_BIT);
		if (hsm_object_public_key(g_tok, s->sess, priv, pk, info->pk_len, &plen) != HSM_OK) {
			free(pk);
			rv = CKR_OBJECT_HANDLE_INVALID;
			goto out;
		}
		pqc_status_t vs = pqc_verify(m.alg, pk, pData, ulDataLen, NULL, 0,
		                             pSignature, ulSignatureLen);
		free(pk);
		s->verify_active = 0;
		rv = (vs == PQC_OK) ? CKR_OK : CKR_SIGNATURE_INVALID;
	}
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

/* 未实现的接口一律不填进函数表 —— PKCS#11 允许表项为 NULL，
 * 调用方据此判断"不支持"，比返回 CKR_FUNCTION_NOT_SUPPORTED 更符合规范。 */

/* ---- 函数表 ------------------------------------------------------------- */

static CK_FUNCTION_LIST g_function_list;
static int g_flist_ready;

static void build_function_list(void)
{
	if (g_flist_ready) {
		return;
	}
	memset(&g_function_list, 0, sizeof(g_function_list));
	g_function_list.version.major = CRYPTOKI_VERSION_MAJOR;
	g_function_list.version.minor = CRYPTOKI_VERSION_MINOR;
	g_function_list.C_Initialize        = C_Initialize;
	g_function_list.C_Finalize          = C_Finalize;
	g_function_list.C_GetInfo           = C_GetInfo;
	g_function_list.C_GetFunctionList   = C_GetFunctionList;
	g_function_list.C_GetSlotList       = C_GetSlotList;
	g_function_list.C_GetSlotInfo       = C_GetSlotInfo;
	g_function_list.C_GetTokenInfo      = C_GetTokenInfo;
	g_function_list.C_GetMechanismList  = C_GetMechanismList;
	g_function_list.C_GetMechanismInfo  = C_GetMechanismInfo;
	g_function_list.C_InitToken         = C_InitToken;
	g_function_list.C_InitPIN           = C_InitPIN;
	g_function_list.C_OpenSession       = C_OpenSession;
	g_function_list.C_CloseSession      = C_CloseSession;
	g_function_list.C_CloseAllSessions  = C_CloseAllSessions;
	g_function_list.C_GetSessionInfo    = C_GetSessionInfo;
	g_function_list.C_Login             = C_Login;
	g_function_list.C_Logout            = C_Logout;
	g_function_list.C_DestroyObject     = C_DestroyObject;
	g_function_list.C_GetAttributeValue = C_GetAttributeValue;
	g_function_list.C_FindObjectsInit   = C_FindObjectsInit;
	g_function_list.C_FindObjects       = C_FindObjects;
	g_function_list.C_FindObjectsFinal  = C_FindObjectsFinal;
	g_function_list.C_SignInit          = C_SignInit;
	g_function_list.C_Sign              = C_Sign;
	g_function_list.C_VerifyInit        = C_VerifyInit;
	g_function_list.C_Verify            = C_Verify;
	g_function_list.C_GenerateKeyPair   = C_GenerateKeyPair;
	g_flist_ready = 1;
}

CK_DEFINE_FUNCTION(CK_RV, C_GetFunctionList)(CK_FUNCTION_LIST_PTR_PTR ppFunctionList)
{
	if (!ppFunctionList) {
		return CKR_ARGUMENTS_BAD;
	}
	build_function_list();
	*ppFunctionList = &g_function_list;
	return CKR_OK;
}
