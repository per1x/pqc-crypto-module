/* p11_module.c —— PKCS#11 v3.2 前端
 *
 * 把已有的槽位管理器包装成一个标准 PKCS#11 动态库，用 OpenSC 的 pkcs11-tool
 * 驱动、与 SoftHSMv2 对比行为。
 *
 * 【为什么这一层几乎是"翻译"而不是"实现"】
 * 的 slot/token/object/session 模型本来就是照 PKCS#11 设计的，所以映射是一一对应的：
 *
 *   PKCS#11 slot i        ↔  hsm_slot_id_t i
 *   C_OpenSession         ↔  hsm_session_open
 *   C_Login(CKU_SO/USER)  ↔  hsm_session_login(HSM_ROLE_SO/USER)
 *   C_GenerateKeyPair     ↔  hsm_slot_generate
 *   C_Sign                ↔  hsm_object_sign
 *   C_DestroyObject       ↔  hsm_object_destroy
 *
 * 对象句柄：本项目一槽位一密钥对，所以一个已装载的槽位恰好对外呈现两个对象。
 * 句柄由槽位号与 generation 直接编出，公钥句柄只比私钥句柄多一个标志位，
 * 两者可互相推导，不需要额外的对象表。编码本身见下方 P11_IDX_BITS 那段
 * ——它必须限制在 32 位以内，因为 CK_ULONG 在 32 位 ABI 上只有 4 字节。
 *
 * 【进程模型】
 * pkcs11-tool 每条命令都是一个新进程，所以状态必须落盘才有意义：
 * C_Initialize 载入密钥库，任何改动状态的操作立刻存回。
 *   PQCHSM_KEYSTORE  密钥库路径（默认 $HOME/.pqchsm/keystore.bin）
 *   PQCHSM_SLOTS     槽位数（默认 4）
 *
 * 【范围】
 * 实现的是**能把 pkcs11-tool 常用流程跑通**的子集，其余返回
 * CKR_FUNCTION_NOT_SUPPORTED。未实现的部分见 docs/design/status-and-roadmap.md。
 *
 * 【多段签名 / 对象导入 / KEM 封装的三处设计取舍，都写在各自函数上方】
 *   C_SignUpdate/Final        累积后单次签 —— ML-DSA 本来就不是 hash-then-sign
 *   C_CreateObject            只收种子，**不收明文私钥**
 *   C_Encapsulate/Decapsulate 共享秘密落成**会话对象**，不占槽位
 */
#include "p11_config.h"

#include "pqchsm/hwrng.h"
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

/* ---- 对象句柄编码 ---------------------------------------------------------
 * CK_OBJECT_HANDLE 就是 CK_ULONG，而 CK_ULONG 的定义是 unsigned long：
 * 在 x86_64 / aarch64 上是 8 字节，在 armv7l（Zynq-7000 的 Cortex-A9）上只有
 * 4 字节。所以句柄编码必须整体落在 32 位以内 —— 不能把核心层的 64 位
 * hsm_handle_t（generation << 32 | slot+1）原样透给 PKCS#11 调用方。
 *
 *   bit 31      公钥对象
 *   bit 30      会话密钥对象（KEM 共享秘密，不占槽位）
 *   bits 12-29  槽位 generation 的低 18 位
 *   bits 0-11   索引：槽位对象为 slot+1，会话密钥对象为对象表下标+1
 *
 * generation 只带低 18 位：它在句柄里的唯一作用是让密钥重生成后的旧句柄失效，
 * 而完整值随时能从槽位元数据读回。代价是同一槽位重生成 2^18 次之后，
 * 旧句柄的这 18 位会重新对上；核心层仍然做完整的 64 位比较，
 * 这里的截断只影响本模块自己的前置检查。 */
#define P11_IDX_BITS   12
#define P11_IDX_MASK   ((CK_OBJECT_HANDLE)((1u << P11_IDX_BITS) - 1u))
#define P11_GEN_MASK   ((CK_OBJECT_HANDLE)0x3ffffu)
#define PUB_BIT        ((CK_OBJECT_HANDLE)1u << 31)

/* ---- 厂商自定义属性 --------------------------------------------------------
 * PKCS#11 **没有**"这把密钥可否被 KEK 包裹备份"这个标准属性：
 * CKA_EXTRACTABLE 说的是"可否明文导出"，与这里要表达的完全不是一回事
 * （本模块任何情况下都不导出明文私钥）。所以按规范用 CKA_VENDOR_DEFINED 区段。
 *
 * **默认值是"可备份"** —— 一个所有密钥都进不了备份的 token，
 * 会让 的整条恢复链对 PKCS#11 应用完全失效。想要"纯 sealed 密钥"
 * （如设备身份钥，设备损坏就该跟着消失）就在模板里显式置 CK_FALSE。 */
#define CKA_PQCHSM_BACKUPABLE   (CKA_VENDOR_DEFINED | 0x01UL)
#define CKA_PQCHSM_SEED_STORAGE (CKA_VENDOR_DEFINED | 0x02UL)

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static hsm_token_t    *g_tok;
static int             g_init;
static char            g_ks_path[512];
static CK_ULONG        g_n_slots = DEFAULT_SLOTS;

/* 可增长的字节缓冲 —— 多段签名的累积用 */
typedef struct {
	uint8_t *p;
	size_t   len, cap;
} p11_buf_t;

static void buf_free(p11_buf_t *b)
{
	if (b->p) {
		pqc_secure_zero(b->p, b->cap);
		free(b->p);
	}
	b->p = NULL;
	b->len = b->cap = 0;
}

static int buf_append(p11_buf_t *b, const uint8_t *d, size_t n)
{
	if (n == 0) {
		return 0;
	}
	if (b->len + n > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 256;
		while (cap < b->len + n) {
			cap *= 2;
		}
		uint8_t *np = malloc(cap);
		if (!np) {
			return -1;
		}
		if (b->p) {
			memcpy(np, b->p, b->len);
			pqc_secure_zero(b->p, b->cap);
			free(b->p);
		}
		b->p = np;
		b->cap = cap;
	}
	memcpy(b->p + b->len, d, n);
	b->len += n;
	return 0;
}

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
	/* 无需清零：这两个是对象句柄（槽位号 + 代数），密钥材料始终留在槽位里 */
	CK_OBJECT_HANDLE sign_key, verify_key;
	/* C_SignUpdate / C_VerifyUpdate 的累积缓冲（理由见 C_SignUpdate 上方） */
	p11_buf_t     sign_acc, verify_acc;
} p11_session_t;

#define MAX_P11_SESSIONS 32
static p11_session_t g_sessions[MAX_P11_SESSIONS];

/* ---- 会话密钥对象（KEM 共享秘密的落点）---------------------------------
 * C_EncapsulateKey/C_DecapsulateKey 按规范要产出一个**密钥对象句柄**，
 * 而不是把共享秘密直接吐给调用方。本项目的槽位是"一槽一密钥对"的持久结构
 * ，把一堆临时的 32 B 共享秘密塞进槽位既浪费也不对 ——
 * 它们本就是**会话生命期**的东西。所以另开一张会话对象表：
 *   · 只在内存里，不落盘、不占槽位；
 *   · C_CloseSession / C_Finalize 时连同缓冲一起清零；
 *   · 句柄用 SECRET_BIT 与槽位对象句柄区分开。
 * 这与 PKCS#11 的 CKA_TOKEN=CK_FALSE（会话对象）语义一致。 */
#define SECRET_BIT  ((CK_OBJECT_HANDLE)1u << 30)
#define MAX_SECRETS 32
#define SECRET_MAX_LEN 64

typedef struct {
	int               in_use;
	CK_SESSION_HANDLE owner;
	uint8_t           val[SECRET_MAX_LEN];
	size_t            len;
	CK_KEY_TYPE       key_type;
	int               sensitive;
	int               extractable;
	char              label[32];
} p11_secret_t;

static p11_secret_t g_secrets[MAX_SECRETS];

static p11_secret_t *secret_at(CK_OBJECT_HANDLE h)
{
	if (!(h & SECRET_BIT)) {
		return NULL;
	}
	CK_ULONG idx = (CK_ULONG)(h & P11_IDX_MASK);
	if (idx == 0 || idx > MAX_SECRETS) {
		return NULL;
	}
	p11_secret_t *k = &g_secrets[idx - 1];
	return k->in_use ? k : NULL;
}

/* 分配一个会话密钥对象；满了返回 NULL */
static p11_secret_t *secret_alloc(CK_SESSION_HANDLE owner, CK_OBJECT_HANDLE *out)
{
	for (int i = 0; i < MAX_SECRETS; i++) {
		if (!g_secrets[i].in_use) {
			memset(&g_secrets[i], 0, sizeof(g_secrets[i]));
			g_secrets[i].in_use = 1;
			g_secrets[i].owner = owner;
			*out = SECRET_BIT | (CK_OBJECT_HANDLE)(i + 1);
			return &g_secrets[i];
		}
	}
	return NULL;
}

/* 会话结束时清掉它名下的密钥对象 —— 明确清零，不只是标记空闲 */
static void secrets_drop_owner(CK_SESSION_HANDLE owner)
{
	for (int i = 0; i < MAX_SECRETS; i++) {
		if (g_secrets[i].in_use && (owner == 0 || g_secrets[i].owner == owner)) {
			pqc_secure_zero(&g_secrets[i], sizeof(g_secrets[i]));
		}
	}
}

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
	return (hsm_slot_id_t)((h & P11_IDX_MASK) - 1u);
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
	return ((CK_OBJECT_HANDLE)(m.generation & P11_GEN_MASK) << P11_IDX_BITS)
	       | (CK_OBJECT_HANDLE)(slot + 1);
}

/* 核心句柄 → P11 对象句柄（私钥形态）。编不出来时返回 0。 */
static CK_OBJECT_HANDLE p11_handle_of_core(hsm_handle_t core)
{
	if (core == HSM_INVALID_HANDLE) {
		return 0;
	}
	CK_OBJECT_HANDLE idx = (CK_OBJECT_HANDLE)(core & 0xffffffffu);
	if (idx == 0 || idx > P11_IDX_MASK) {
		return 0;
	}
	CK_OBJECT_HANDLE gen = (CK_OBJECT_HANDLE)((core >> 32) & P11_GEN_MASK);
	return (gen << P11_IDX_BITS) | idx;
}

/* P11 对象句柄 → 核心句柄。索引越界、generation 已过期、或者传进来的是
 * 会话密钥对象（不在槽位里）时返回 HSM_INVALID_HANDLE。 */
static hsm_handle_t core_handle_of(CK_OBJECT_HANDLE h)
{
	if (h & SECRET_BIT) {
		return HSM_INVALID_HANDLE;
	}
	CK_OBJECT_HANDLE idx = h & P11_IDX_MASK;
	if (idx == 0) {
		return HSM_INVALID_HANDLE;
	}
	slot_meta_t m;
	if (hsm_slot_get_meta(g_tok, (hsm_slot_id_t)(idx - 1u), &m) != HSM_OK) {
		return HSM_INVALID_HANDLE;
	}
	if (((h >> P11_IDX_BITS) & P11_GEN_MASK) != (m.generation & P11_GEN_MASK)) {
		return HSM_INVALID_HANDLE;
	}
	return ((hsm_handle_t)m.generation << 32) | (hsm_handle_t)idx;
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
	/* ---- 熵源：PQCHSM_HWRNG=sdfe 时改由 FPGA 供随机数 ----
	 *
	 * 装上之后 pqc_random_bytes()（以及 liboqs 的随机源）走
	 *   本模块 → libsdfe → pqchsm_fpgad → /dev/secmmio → EL3 → trng_axi
	 * 也就是说 C_GenerateRandom 与所有密钥生成的随机性都来自 PL 里的环振
	 * 噪声源，不再是 OpenSSL。
	 *
	 * **不设这个变量时行为一个字不变**（走软件源）—— 这条路要显式打开，
	 * 因为它需要一台在跑的密码机；默认打开会让没有板子的人一上来就失败。
	 *
	 * 而一旦打开，取不到熵就是硬错误，不会悄悄回退（见 hwrng_sdfe.c）。
	 * 这是本项目一贯的纪律：宁可停机，也不要让"熵来自硬件"这句话
	 * 在最要紧的时刻悄悄变成假话。
	 */
	{
		const char *hw = getenv("PQCHSM_HWRNG");

		if (hw && !strcmp(hw, "sdfe")) {
			hwrng_set_byte_source(hwrng_byte_source_sdfe());
			if (!hwrng_is_hardware()) {
				rv = CKR_DEVICE_ERROR;
				goto out;
			}
		}
	}

	/* ---- 算法后端：PQCHSM_BACKEND=sdfe 时 ML-KEM 整条走 FPGA ----
	 *
	 * 装上之后 C_GenerateKeyPair 生成的 ML-KEM 私钥**留在 PL 的片内金库**，
	 * 本进程只拿到公钥和一个句柄；C_Decapsulate 把句柄交给硬件，
	 * 私钥连指针都不存在。
	 *
	 * 与熵源那个开关分开，是因为它们的失败方式不同：熵源取不到就该停机，
	 * 而算法后端不支持某个算法（比如 ML-DSA，硬件里还没有）时要能回落到
	 * 软件 —— 后端的 vtable 里那几项填 NULL，pqc_* 包装会回
	 * PQC_ERR_UNSUPPORTED，上层据此选择。**回落是显式的，不是静默的。**
	 */
	{
		const char *be = getenv("PQCHSM_BACKEND");

		if (be && !strcmp(be, "sdfe")) {
			pqc_set_backend(pqc_backend_sdfe());
			if (!pqc_backend_has_hw_keys()) {
				rv = CKR_DEVICE_ERROR;
				goto out;
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
		for (int i = 0; i < MAX_P11_SESSIONS; i++) {
			buf_free(&g_sessions[i].sign_acc);
			buf_free(&g_sessions[i].verify_acc);
		}
		secrets_drop_owner(0);
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
	buf_free(&s->sign_acc);
	buf_free(&s->verify_acc);
	secrets_drop_owner(hSession);
	memset(s, 0, sizeof(*s));
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

/* 只关**该槽位**的会话：规范里 C_CloseAllSessions 是按槽位的，
 * 其他槽位上的会话不应被牵连。 */
CK_DEFINE_FUNCTION(CK_RV, C_CloseAllSessions)(CK_SLOT_ID slotID)
{
	pthread_mutex_lock(&g_lock);
	for (int i = 0; i < MAX_P11_SESSIONS; i++) {
		if (g_sessions[i].in_use && g_sessions[i].slot == slotID) {
			(void)hsm_session_close(g_tok, g_sessions[i].sess);
			buf_free(&g_sessions[i].sign_acc);
			buf_free(&g_sessions[i].verify_acc);
			secrets_drop_owner((CK_SESSION_HANDLE)(i + 1));
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

/* 读模板里的 CK_BBOOL；缺省时返回 dflt */
static int attr_bool(CK_ATTRIBUTE_PTR t, CK_ULONG n, CK_ATTRIBUTE_TYPE type, int dflt)
{
	const CK_ATTRIBUTE *a = find_attr(t, n, type);
	if (!a || !a->pValue || a->ulValueLen != sizeof(CK_BBOOL)) {
		return dflt;
	}
	return *(CK_BBOOL *)a->pValue != CK_FALSE;
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

		/* 策略位：两个厂商属性都可以放在公钥或私钥模板里 */
		uint32_t policy = 0;
		int backupable = attr_bool(pPrivateKeyTemplate, ulPrivateKeyAttributeCount,
		                           CKA_PQCHSM_BACKUPABLE,
		                           attr_bool(pPublicKeyTemplate, ulPublicKeyAttributeCount,
		                                     CKA_PQCHSM_BACKUPABLE, 1));
		int seed_storage = attr_bool(pPrivateKeyTemplate, ulPrivateKeyAttributeCount,
		                             CKA_PQCHSM_SEED_STORAGE,
		                             attr_bool(pPublicKeyTemplate, ulPublicKeyAttributeCount,
		                                       CKA_PQCHSM_SEED_STORAGE, 0));
		if (backupable) {
			policy |= SLOT_POLICY_BACKUPABLE;
		}
		if (seed_storage) {
			policy |= SLOT_POLICY_SEED_STORAGE;
		}

		hsm_handle_t h = HSM_INVALID_HANDLE;
		hsm_status_t st = hsm_slot_generate(g_tok, s->sess, alg, usage, policy, &h);
		if (st != HSM_OK) {
			rv = map_status(st);
			goto out;
		}
		persist();
		CK_OBJECT_HANDLE ph = p11_handle_of_core(h);
		if (ph == 0) {
			rv = CKR_GENERAL_ERROR;
			goto out;
		}
		*phPrivateKey = ph;
		*phPublicKey  = ph | PUB_BIT;
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
	if (hObject & SECRET_BIT) {
		p11_secret_t *k = secret_at(hObject);
		if (!k) {
			rv = CKR_OBJECT_HANDLE_INVALID;
		} else {
			pqc_secure_zero(k, sizeof(*k));   /* 清零，不只是标记空闲 */
		}
		goto out;
	}
	/* 公钥与私钥是同一对，销毁任一即销毁整对 —— 本模型一槽一对 */
	hsm_handle_t core = core_handle_of(hObject);
	if (core == HSM_INVALID_HANDLE) {
		rv = CKR_OBJECT_HANDLE_INVALID;
		goto out;
	}
	rv = map_status(hsm_object_destroy(g_tok, s->sess, core));
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
		CK_ULONG want_class = attr_ulong(cls, (CK_ULONG)-1);

		s->n_found = 0;
		s->found_pos = 0;
		/* 会话绑定在某个槽位上，所以只可能找到该槽位的对象（一槽一对） */
		{
			CK_OBJECT_HANDLE priv = priv_handle((hsm_slot_id_t)s->slot);
			if (priv) {
				if (want_class == (CK_ULONG)-1 || want_class == CKO_PUBLIC_KEY) {
					s->found[s->n_found++] = priv | PUB_BIT;
				}
				if (want_class == (CK_ULONG)-1 || want_class == CKO_PRIVATE_KEY) {
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
	/* 会话密钥对象（KEM 共享秘密）走单独一支 —— 它不在任何槽位里 */
	if (hObject & SECRET_BIT) {
		p11_secret_t *k = secret_at(hObject);
		if (!k) {
			rv = CKR_OBJECT_HANDLE_INVALID;
			goto out;
		}
		for (CK_ULONG i = 0; i < ulCount; i++) {
			CK_ATTRIBUTE *a = &pTemplate[i];
			CK_RV r = CKR_OK;
			switch (a->type) {
			case CKA_CLASS: {
				CK_OBJECT_CLASS c = CKO_SECRET_KEY;
				r = fill_attr(a, &c, sizeof(c));
				break;
			}
			case CKA_KEY_TYPE:
				r = fill_attr(a, &k->key_type, sizeof(k->key_type));
				break;
			case CKA_VALUE_LEN: {
				CK_ULONG n = (CK_ULONG)k->len;
				r = fill_attr(a, &n, sizeof(n));
				break;
			}
			case CKA_LABEL:
				r = fill_attr(a, k->label, strlen(k->label));
				break;
			case CKA_TOKEN: {
				/* 会话对象，不落盘 */
				CK_BBOOL b = CK_FALSE;
				r = fill_attr(a, &b, sizeof(b));
				break;
			}
			case CKA_PRIVATE: {
				CK_BBOOL b = CK_TRUE;
				r = fill_attr(a, &b, sizeof(b));
				break;
			}
			case CKA_SENSITIVE: {
				CK_BBOOL b = k->sensitive ? CK_TRUE : CK_FALSE;
				r = fill_attr(a, &b, sizeof(b));
				break;
			}
			case CKA_EXTRACTABLE: {
				CK_BBOOL b = k->extractable ? CK_TRUE : CK_FALSE;
				r = fill_attr(a, &b, sizeof(b));
				break;
			}
			case CKA_VALUE:
				/* 默认不可读。要读必须**同时**是 extractable 且非 sensitive ——
				 * 与 PKCS#11 对 CKR_ATTRIBUTE_SENSITIVE 的规定一致。 */
				if (!k->extractable || k->sensitive) {
					a->ulValueLen = CK_UNAVAILABLE_INFORMATION;
					r = CKR_ATTRIBUTE_SENSITIVE;
					break;
				}
				r = fill_attr(a, k->val, k->len);
				break;
			default:
				a->ulValueLen = CK_UNAVAILABLE_INFORMATION;
				r = CKR_ATTRIBUTE_TYPE_INVALID;
				break;
			}
			if (r != CKR_OK && rv == CKR_OK) {
				rv = r;
			}
		}
		goto out;
	}
	{
		int is_pub = (hObject & PUB_BIT) != 0;
		hsm_handle_t priv = core_handle_of(hObject);
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
			case CKA_PQCHSM_BACKUPABLE: {
				CK_BBOOL b = (m.policy & SLOT_POLICY_BACKUPABLE) ? CK_TRUE : CK_FALSE;
				r = fill_attr(a, &b, sizeof(b));
				break;
			}
			case CKA_PQCHSM_SEED_STORAGE: {
				CK_BBOOL b = (m.policy & SLOT_POLICY_SEED_STORAGE) ? CK_TRUE : CK_FALSE;
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
		hsm_status_t st = hsm_object_sign(g_tok, s->sess, core_handle_of(s->sign_key),
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

/* 验签主体：C_Verify 与 C_VerifyFinal 共用。
 * 调用时 g_lock 已持有；本函数不碰 verify_active（由调用方负责结束操作）。
 * 验签只用公钥，不需要动私钥。 */
static CK_RV verify_with_pubkey(p11_session_t *s, CK_OBJECT_HANDLE key,
                                const CK_BYTE *data, CK_ULONG data_len,
                                const CK_BYTE *sig, CK_ULONG sig_len)
{
	slot_meta_t m;
	if (hsm_slot_get_meta(g_tok, slot_of_handle(key), &m) != HSM_OK) {
		return CKR_OBJECT_HANDLE_INVALID;
	}
	const pqc_alg_info_t *info = pqc_alg_info(m.alg);
	if (!info || info->kind != PQC_KIND_SIG) {
		return CKR_KEY_TYPE_INCONSISTENT;
	}
	uint8_t *pk = malloc(info->pk_len);
	if (!pk) {
		return CKR_HOST_MEMORY;
	}
	size_t plen = 0;
	if (hsm_object_public_key(g_tok, s->sess, core_handle_of(key),
	                          pk, info->pk_len, &plen) != HSM_OK) {
		free(pk);
		return CKR_OBJECT_HANDLE_INVALID;
	}
	pqc_status_t vs = pqc_verify(m.alg, pk, data, data_len, NULL, 0, sig, sig_len);
	free(pk);
	return (vs == PQC_OK) ? CKR_OK : CKR_SIGNATURE_INVALID;
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
	rv = verify_with_pubkey(s, s->verify_key, pData, ulDataLen,
	                        pSignature, ulSignatureLen);
	s->verify_active = 0;
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

/* 未实现的接口一律不填进函数表 —— PKCS#11 允许表项为 NULL，
 * 调用方据此判断"不支持"，比返回 CKR_FUNCTION_NOT_SUPPORTED 更符合规范。 */

/* ---- 函数表 ------------------------------------------------------------- */

/* ---- C_CreateObject（对象导入）------------------------------------------
 *
 * 【只收种子，不收明文私钥 —— 这是刻意的】
 * 常规 HSM 的 C_CreateObject 允许灌一把明文私钥进去。本项目底下的槽位管理器
 * **根本没有这条通路**：include/pqchsm/slot.h 里不存在任何"把 sk 字节装进槽位"
 * 的入口，只有 hsm_slot_generate（内部生成）与 hsm_slot_load_seed（由种子展开）。
 * 这是槽位管理器的设计结论，不是这一层能绕过的，也不该绕过。
 *
 * 于是：
 *   CKA_SEED  → hsm_slot_load_seed，导入成功
 *   CKA_VALUE → CKR_ATTRIBUTE_TYPE_INVALID，并且**如实说明原因**
 *               （想灌明文密钥请走 的 hsm_inject_* 注入通道）
 *
 * FIPS 203/204 的密钥本来就由种子完全决定（ML-KEM 64 B 的 d‖z、ML-DSA 32 B 的 ξ），
 * 所以"只收种子"并没有削弱能力 —— 反而少搬了几千字节的敏感数据。
 *
 * 另外支持 CKO_SECRET_KEY：建一个会话密钥对象。这条主要是给
 * C_Encapsulate/Decapsulate 的对端使用（比如把对方算出的共享秘密导入进来比对）。
 */
CK_DEFINE_FUNCTION(CK_RV, C_CreateObject)(CK_SESSION_HANDLE hSession,
                                          CK_ATTRIBUTE_PTR pTemplate, CK_ULONG ulCount,
                                          CK_OBJECT_HANDLE_PTR phObject)
{
	pthread_mutex_lock(&g_lock);
	CK_RV rv = CKR_OK;
	p11_session_t *s = NULL;
	if (!g_init) {
		rv = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto out;
	}
	s = sess_at(hSession);
	if (!s || !pTemplate || !phObject) {
		rv = s ? CKR_ARGUMENTS_BAD : CKR_SESSION_HANDLE_INVALID;
		goto out;
	}
	{
		CK_ULONG cls = attr_ulong(find_attr(pTemplate, ulCount, CKA_CLASS), (CK_ULONG)-1);

		if (cls == CKO_SECRET_KEY) {
			const CK_ATTRIBUTE *v = find_attr(pTemplate, ulCount, CKA_VALUE);
			if (!v || !v->pValue || v->ulValueLen == 0) {
				rv = CKR_TEMPLATE_INCOMPLETE;
				goto out;
			}
			if (v->ulValueLen > SECRET_MAX_LEN) {
				rv = CKR_ATTRIBUTE_VALUE_INVALID;
				goto out;
			}
			CK_OBJECT_HANDLE h = 0;
			p11_secret_t *k = secret_alloc(hSession, &h);
			if (!k) {
				rv = CKR_HOST_MEMORY;
				goto out;
			}
			memcpy(k->val, v->pValue, v->ulValueLen);
			k->len = v->ulValueLen;
			k->key_type = attr_ulong(find_attr(pTemplate, ulCount, CKA_KEY_TYPE),
			                         CKK_GENERIC_SECRET);
			k->sensitive   = attr_bool(pTemplate, ulCount, CKA_SENSITIVE, 1);
			k->extractable = attr_bool(pTemplate, ulCount, CKA_EXTRACTABLE, 0);
			{
				const CK_ATTRIBUTE *lb = find_attr(pTemplate, ulCount, CKA_LABEL);
				if (lb && lb->pValue) {
					size_t n = lb->ulValueLen;
					if (n >= sizeof(k->label)) {
						n = sizeof(k->label) - 1;
					}
					memcpy(k->label, lb->pValue, n);
				}
			}
			*phObject = h;
			goto out;
		}

		if (cls != CKO_PRIVATE_KEY) {
			/* 公钥单独导入没有意义：本模型里公钥是私钥的派生物，
			 * 一槽一对，导入一个孤立的公钥无处安放。 */
			rv = (cls == (CK_ULONG)-1) ? CKR_TEMPLATE_INCOMPLETE
			                           : CKR_ATTRIBUTE_VALUE_INVALID;
			goto out;
		}

		if (find_attr(pTemplate, ulCount, CKA_VALUE)) {
			/* 明文私钥导入：本项目**没有**这条通路，如实拒绝而不是假装成功。
			 * 要灌密钥请走 的注入通道（hsm_inject_build/apply）。 */
			rv = CKR_ATTRIBUTE_TYPE_INVALID;
			goto out;
		}

		const CK_ATTRIBUTE *sd = find_attr(pTemplate, ulCount, CKA_SEED);
		if (!sd || !sd->pValue) {
			rv = CKR_TEMPLATE_INCOMPLETE;
			goto out;
		}
		CK_ULONG kt = attr_ulong(find_attr(pTemplate, ulCount, CKA_KEY_TYPE),
		                         (CK_ULONG)-1);
		CK_ULONG pset = attr_ulong(find_attr(pTemplate, ulCount, CKA_PARAMETER_SET), 0);
		CK_MECHANISM_TYPE as_mech = (kt == CKK_ML_KEM) ? CKM_ML_KEM
		                          : (kt == CKK_ML_DSA) ? CKM_ML_DSA
		                                               : (CK_MECHANISM_TYPE)-1;
		if (as_mech == (CK_MECHANISM_TYPE)-1) {
			rv = (kt == (CK_ULONG)-1) ? CKR_TEMPLATE_INCOMPLETE
			                          : CKR_ATTRIBUTE_VALUE_INVALID;
			goto out;
		}
		pqc_alg_t alg = alg_from_param(as_mech, pset);
		if (alg == PQC_ALG_NONE) {
			rv = (pset == 0) ? CKR_TEMPLATE_INCOMPLETE : CKR_ATTRIBUTE_VALUE_INVALID;
			goto out;
		}
		const pqc_alg_info_t *info = pqc_alg_info(alg);
		if (sd->ulValueLen != info->seed_len) {
			rv = CKR_ATTRIBUTE_VALUE_INVALID;
			goto out;
		}
		uint32_t usage = (info->kind == PQC_KIND_SIG)
		                 ? (uint32_t)KEY_USAGE_SIGN : (uint32_t)KEY_USAGE_DECAP;
		uint32_t policy = 0;
		if (attr_bool(pTemplate, ulCount, CKA_PQCHSM_BACKUPABLE, 1)) {
			policy |= SLOT_POLICY_BACKUPABLE;
		}
		if (attr_bool(pTemplate, ulCount, CKA_PQCHSM_SEED_STORAGE, 0)) {
			policy |= SLOT_POLICY_SEED_STORAGE;
		}
		hsm_handle_t h = HSM_INVALID_HANDLE;
		hsm_status_t st = hsm_slot_load_seed(g_tok, s->sess, alg, usage, policy,
		                                     (const uint8_t *)sd->pValue,
		                                     sd->ulValueLen, &h);
		if (st != HSM_OK) {
			rv = map_status(st);
			goto out;
		}
		persist();
		*phObject = p11_handle_of_core(h);
		if (*phObject == 0) {
			rv = CKR_GENERAL_ERROR;
			goto out;
		}
	}
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

/* ---- 多段签名 / 验签 -----------------------------------------------------
 *
 * 【为什么是"先攒起来、最后一次签"，而不是流式摘要】
 * RSA/ECDSA 那套 C_SignUpdate 之所以能真正流式，是因为它们是 hash-then-sign：
 * Update 推进摘要状态，Final 只对 32 B 的摘要做一次公钥运算。
 *
 * **ML-DSA 不是这个结构**。FIPS 204 里签名要算 μ = H(BytesToBits(tr) ‖ M'),
 * 而 M' 还带域分隔与 context 前缀；更关键的是拒绝采样循环每次重试都要用到 μ，
 * 消息不能"边读边扔"。所以 PKCS#11 v3.2 对 CKM_ML_DSA 的多段接口，语义上
 * 就是"把各段拼起来再整体签一次"。
 *
 * 这里如实照此实现：Update 往会话缓冲里追加，Final 调一次 hsm_object_sign。
 * **不假装流式**——内存占用与消息等长这件事，调用方应该知道。
 * 真要处理超大消息，正确做法是外部先 SHA3-512 再签摘要（HashML-DSA，
 * 对应 CKM_HASH_ML_DSA_*，本模块尚未实现，见 docs/design/status-and-roadmap.md）。
 */
CK_DEFINE_FUNCTION(CK_RV, C_SignUpdate)(CK_SESSION_HANDLE hSession,
                                        CK_BYTE_PTR pPart, CK_ULONG ulPartLen)
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
	if (!s->sign_active) {
		rv = CKR_OPERATION_NOT_INITIALIZED;
		goto out;
	}
	if (ulPartLen && !pPart) {
		rv = CKR_ARGUMENTS_BAD;
		goto out;
	}
	if (buf_append(&s->sign_acc, pPart, ulPartLen) != 0) {
		buf_free(&s->sign_acc);
		s->sign_active = 0;
		rv = CKR_HOST_MEMORY;
	}
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

CK_DEFINE_FUNCTION(CK_RV, C_SignFinal)(CK_SESSION_HANDLE hSession,
                                       CK_BYTE_PTR pSignature,
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
		/* 只问长度：按规范此时**不能**结束操作 */
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
		hsm_status_t st = hsm_object_sign(g_tok, s->sess, core_handle_of(s->sign_key),
		                                  s->sign_acc.p, s->sign_acc.len, NULL, 0,
		                                  pSignature, *pulSignatureLen, &sl);
		buf_free(&s->sign_acc);
		s->sign_active = 0;
		if (st != HSM_OK) {
			rv = map_status(st);
			goto out;
		}
		*pulSignatureLen = (CK_ULONG)sl;
		persist();
	}
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

CK_DEFINE_FUNCTION(CK_RV, C_VerifyUpdate)(CK_SESSION_HANDLE hSession,
                                          CK_BYTE_PTR pPart, CK_ULONG ulPartLen)
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
	if (ulPartLen && !pPart) {
		rv = CKR_ARGUMENTS_BAD;
		goto out;
	}
	if (buf_append(&s->verify_acc, pPart, ulPartLen) != 0) {
		buf_free(&s->verify_acc);
		s->verify_active = 0;
		rv = CKR_HOST_MEMORY;
	}
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

CK_DEFINE_FUNCTION(CK_RV, C_VerifyFinal)(CK_SESSION_HANDLE hSession,
                                         CK_BYTE_PTR pSignature,
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
	if (!s || !pSignature) {
		rv = s ? CKR_ARGUMENTS_BAD : CKR_SESSION_HANDLE_INVALID;
		goto out;
	}
	if (!s->verify_active) {
		rv = CKR_OPERATION_NOT_INITIALIZED;
		goto out;
	}
	rv = verify_with_pubkey(s, s->verify_key, s->verify_acc.p, s->verify_acc.len,
	                        pSignature, ulSignatureLen);
	buf_free(&s->verify_acc);
	s->verify_active = 0;
out:
	pthread_mutex_unlock(&g_lock);
	return rv;
}

/* ---- KEM 封装 / 解封装（PKCS#11 v3.2 新增的两个函数）---------------------
 *
 * 【为什么共享秘密要落成"会话对象"而不是直接返回字节】
 * 规范里这两个函数的出参是 CK_OBJECT_HANDLE：共享秘密应当留在 token 内、
 * 由后续的 C_DeriveKey / C_Encrypt 使用，而不是穿过边界。本模块照此实现——
 * 默认 CKA_SENSITIVE=TRUE、CKA_EXTRACTABLE=FALSE，此时读 CKA_VALUE 会拿到
 * CKR_ATTRIBUTE_SENSITIVE。演示程序若确实要看到共享秘密（比如证明两端一致），
 * 在模板里显式给 CKA_EXTRACTABLE=TRUE 且 CKA_SENSITIVE=FALSE。
 *
 * 落点是会话对象表而不是槽位，理由见 SECRET_BIT 上方那段。
 */
CK_DEFINE_FUNCTION(CK_RV, C_EncapsulateKey)(
	CK_SESSION_HANDLE hSession, CK_MECHANISM_PTR pMechanism,
	CK_OBJECT_HANDLE hPublicKey, CK_ATTRIBUTE_PTR pTemplate, CK_ULONG ulAttributeCount,
	CK_BYTE_PTR pCiphertext, CK_ULONG_PTR pulCiphertextLen, CK_OBJECT_HANDLE_PTR phKey)
{
	pthread_mutex_lock(&g_lock);
	CK_RV rv = CKR_OK;
	p11_session_t *s = NULL;
	uint8_t *pk = NULL;
	uint8_t ss[SECRET_MAX_LEN];
	if (!g_init) {
		rv = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto out;
	}
	s = sess_at(hSession);
	if (!s || !pMechanism || !pulCiphertextLen) {
		rv = s ? CKR_ARGUMENTS_BAD : CKR_SESSION_HANDLE_INVALID;
		goto out;
	}
	if (pMechanism->mechanism != CKM_ML_KEM) {
		rv = CKR_MECHANISM_INVALID;
		goto out;
	}
	if (!(hPublicKey & PUB_BIT)) {
		rv = CKR_KEY_TYPE_INCONSISTENT;   /* 封装要用公钥 */
		goto out;
	}
	{
		slot_meta_t m;
		if (hsm_slot_get_meta(g_tok, slot_of_handle(hPublicKey), &m) != HSM_OK) {
			rv = CKR_OBJECT_HANDLE_INVALID;
			goto out;
		}
		const pqc_alg_info_t *info = pqc_alg_info(m.alg);
		if (!info || info->kind != PQC_KIND_KEM) {
			rv = CKR_KEY_TYPE_INCONSISTENT;
			goto out;
		}
		/* 只问长度 */
		if (!pCiphertext) {
			*pulCiphertextLen = (CK_ULONG)info->ct_len;
			goto out;
		}
		if (*pulCiphertextLen < info->ct_len) {
			*pulCiphertextLen = (CK_ULONG)info->ct_len;
			rv = CKR_BUFFER_TOO_SMALL;
			goto out;
		}
		if (!phKey) {
			rv = CKR_ARGUMENTS_BAD;
			goto out;
		}
		if (info->ss_len > SECRET_MAX_LEN) {
			rv = CKR_GENERAL_ERROR;
			goto out;
		}
		pk = malloc(info->pk_len);
		if (!pk) {
			rv = CKR_HOST_MEMORY;
			goto out;
		}
		size_t plen = 0;
		hsm_status_t hst = hsm_object_public_key(g_tok, s->sess,
		                                         core_handle_of(hPublicKey),
		                                         pk, info->pk_len, &plen);
		if (hst != HSM_OK) {
			rv = map_status(hst);
			goto out;
		}
		/* 封装只用公钥 —— 与签名验证一样，本身不需要 token 里的秘密 */
		if (pqc_encaps(m.alg, pk, pCiphertext, ss) != PQC_OK) {
			rv = CKR_DEVICE_ERROR;
			goto out;
		}
		CK_OBJECT_HANDLE h = 0;
		p11_secret_t *k = secret_alloc(hSession, &h);
		if (!k) {
			rv = CKR_HOST_MEMORY;
			goto out;
		}
		memcpy(k->val, ss, info->ss_len);
		k->len = info->ss_len;
		k->key_type    = attr_ulong(find_attr(pTemplate, ulAttributeCount, CKA_KEY_TYPE),
		                            CKK_GENERIC_SECRET);
		k->sensitive   = attr_bool(pTemplate, ulAttributeCount, CKA_SENSITIVE, 1);
		k->extractable = attr_bool(pTemplate, ulAttributeCount, CKA_EXTRACTABLE, 0);
		*pulCiphertextLen = (CK_ULONG)info->ct_len;
		*phKey = h;
	}
out:
	free(pk);
	pqc_secure_zero(ss, sizeof(ss));
	pthread_mutex_unlock(&g_lock);
	return rv;
}

CK_DEFINE_FUNCTION(CK_RV, C_DecapsulateKey)(
	CK_SESSION_HANDLE hSession, CK_MECHANISM_PTR pMechanism,
	CK_OBJECT_HANDLE hPrivateKey, CK_ATTRIBUTE_PTR pTemplate, CK_ULONG ulAttributeCount,
	CK_BYTE_PTR pCiphertext, CK_ULONG ulCiphertextLen, CK_OBJECT_HANDLE_PTR phKey)
{
	pthread_mutex_lock(&g_lock);
	CK_RV rv = CKR_OK;
	p11_session_t *s = NULL;
	uint8_t ss[SECRET_MAX_LEN];
	if (!g_init) {
		rv = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto out;
	}
	s = sess_at(hSession);
	if (!s || !pMechanism || !pCiphertext || !phKey) {
		rv = s ? CKR_ARGUMENTS_BAD : CKR_SESSION_HANDLE_INVALID;
		goto out;
	}
	if (pMechanism->mechanism != CKM_ML_KEM) {
		rv = CKR_MECHANISM_INVALID;
		goto out;
	}
	if (hPrivateKey & PUB_BIT) {
		rv = CKR_KEY_TYPE_INCONSISTENT;   /* 解封装要用私钥 */
		goto out;
	}
	{
		slot_meta_t m;
		if (hsm_slot_get_meta(g_tok, slot_of_handle(hPrivateKey), &m) != HSM_OK) {
			rv = CKR_OBJECT_HANDLE_INVALID;
			goto out;
		}
		const pqc_alg_info_t *info = pqc_alg_info(m.alg);
		if (!info || info->kind != PQC_KIND_KEM) {
			rv = CKR_KEY_TYPE_INCONSISTENT;
			goto out;
		}
		if (ulCiphertextLen != info->ct_len) {
			rv = CKR_ENCRYPTED_DATA_LEN_RANGE;
			goto out;
		}
		size_t sl = 0;
		hsm_status_t hst = hsm_object_decaps(g_tok, s->sess, core_handle_of(hPrivateKey),
		                                     pCiphertext, ulCiphertextLen,
		                                     ss, sizeof(ss), &sl);
		if (hst != HSM_OK) {
			rv = map_status(hst);
			goto out;
		}
		CK_OBJECT_HANDLE h = 0;
		p11_secret_t *k = secret_alloc(hSession, &h);
		if (!k) {
			rv = CKR_HOST_MEMORY;
			goto out;
		}
		memcpy(k->val, ss, sl);
		k->len = sl;
		k->key_type    = attr_ulong(find_attr(pTemplate, ulAttributeCount, CKA_KEY_TYPE),
		                            CKK_GENERIC_SECRET);
		k->sensitive   = attr_bool(pTemplate, ulAttributeCount, CKA_SENSITIVE, 1);
		k->extractable = attr_bool(pTemplate, ulAttributeCount, CKA_EXTRACTABLE, 0);
		*phKey = h;
		persist();   /* use_count 变了 */
	}
out:
	pqc_secure_zero(ss, sizeof(ss));
	pthread_mutex_unlock(&g_lock);
	return rv;
}

/* ---- 两张函数表 ---------------------------------------------------------
 * CK_FUNCTION_LIST 是 **2.40 形状**的表：它里面根本没有 C_EncapsulateKey /
 * C_DecapsulateKey 这两个字段（v3.2 新增的函数只出现在 CK_FUNCTION_LIST_3_2 里）。
 * 所以只填 C_GetFunctionList 的话，上面刚实现的 KEM 封装接口对调用方是**不可达的**。
 *
 * 正确做法是 v3.0 引入的 C_GetInterface / C_GetInterfaceList：
 *   老应用   → C_GetFunctionList        拿到 2.40 表（本模块仍然完整支持）
 *   v3.x 应用 → C_GetInterface("PKCS 11") 拿到 3.2 表，里面才有 Encapsulate 等
 * 两张表指向同一批函数实现，只是字段集合不同。 */
static CK_FUNCTION_LIST     g_function_list;
static CK_FUNCTION_LIST_3_2 g_function_list_32;
static CK_INTERFACE         g_interface;
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
	/* 这四个 2.40 就有，两张表都填 */
	g_function_list.C_CreateObject      = C_CreateObject;
	g_function_list.C_SignUpdate        = C_SignUpdate;
	g_function_list.C_SignFinal         = C_SignFinal;
	g_function_list.C_VerifyUpdate      = C_VerifyUpdate;
	g_function_list.C_VerifyFinal       = C_VerifyFinal;

	/* 3.2 表：逐字段复制 2.40 那批，再补 v3.x 独有的。
	 * 两个 struct 的字段名一致（都由 pkcs11f.h 生成），所以这里是纯搬运。 */
	memset(&g_function_list_32, 0, sizeof(g_function_list_32));
	g_function_list_32.version.major = CRYPTOKI_VERSION_MAJOR;
	g_function_list_32.version.minor = CRYPTOKI_VERSION_MINOR;
#define COPY_FN(f) g_function_list_32.f = g_function_list.f
	COPY_FN(C_Initialize);        COPY_FN(C_Finalize);
	COPY_FN(C_GetInfo);           COPY_FN(C_GetFunctionList);
	COPY_FN(C_GetSlotList);       COPY_FN(C_GetSlotInfo);
	COPY_FN(C_GetTokenInfo);      COPY_FN(C_GetMechanismList);
	COPY_FN(C_GetMechanismInfo);  COPY_FN(C_InitToken);
	COPY_FN(C_InitPIN);           COPY_FN(C_OpenSession);
	COPY_FN(C_CloseSession);      COPY_FN(C_CloseAllSessions);
	COPY_FN(C_GetSessionInfo);    COPY_FN(C_Login);
	COPY_FN(C_Logout);            COPY_FN(C_CreateObject);
	COPY_FN(C_DestroyObject);     COPY_FN(C_GetAttributeValue);
	COPY_FN(C_FindObjectsInit);   COPY_FN(C_FindObjects);
	COPY_FN(C_FindObjectsFinal);  COPY_FN(C_SignInit);
	COPY_FN(C_Sign);              COPY_FN(C_SignUpdate);
	COPY_FN(C_SignFinal);         COPY_FN(C_VerifyInit);
	COPY_FN(C_Verify);            COPY_FN(C_VerifyUpdate);
	COPY_FN(C_VerifyFinal);       COPY_FN(C_GenerateKeyPair);
#undef COPY_FN
	/* 只有 3.2 表里才有的字段 */
	g_function_list_32.C_GetInterfaceList = C_GetInterfaceList;
	g_function_list_32.C_GetInterface     = C_GetInterface;
	g_function_list_32.C_EncapsulateKey   = C_EncapsulateKey;
	g_function_list_32.C_DecapsulateKey   = C_DecapsulateKey;

	g_interface.pInterfaceName = (CK_UTF8CHAR_PTR)"PKCS 11";
	g_interface.pFunctionList  = &g_function_list_32;
	g_interface.flags          = 0;

	g_flist_ready = 1;
}

/* C_GetInterfaceList：本模块只暴露一个接口 —— 标准的 "PKCS 11" v3.2 表。 */
CK_DEFINE_FUNCTION(CK_RV, C_GetInterfaceList)(CK_INTERFACE_PTR pInterfacesList,
                                              CK_ULONG_PTR pulCount)
{
	if (!pulCount) {
		return CKR_ARGUMENTS_BAD;
	}
	build_function_list();
	if (!pInterfacesList) {
		*pulCount = 1;
		return CKR_OK;
	}
	if (*pulCount < 1) {
		*pulCount = 1;
		return CKR_BUFFER_TOO_SMALL;
	}
	pInterfacesList[0] = g_interface;
	*pulCount = 1;
	return CKR_OK;
}

/* C_GetInterface：名字为 NULL 表示"给我默认接口"。
 * 版本给定时必须精确匹配 —— 宁可让调用方明确失败，也不要给一张它没预期的表。 */
CK_DEFINE_FUNCTION(CK_RV, C_GetInterface)(CK_UTF8CHAR_PTR pInterfaceName,
                                          CK_VERSION_PTR pVersion,
                                          CK_INTERFACE_PTR_PTR ppInterface,
                                          CK_FLAGS flags)
{
	if (!ppInterface) {
		return CKR_ARGUMENTS_BAD;
	}
	build_function_list();
	if (pInterfaceName && strcmp((const char *)pInterfaceName, "PKCS 11") != 0) {
		return CKR_ARGUMENTS_BAD;
	}
	if (pVersion && (pVersion->major != CRYPTOKI_VERSION_MAJOR ||
	                 pVersion->minor != CRYPTOKI_VERSION_MINOR)) {
		return CKR_ARGUMENTS_BAD;
	}
	if (flags & ~(CK_FLAGS)CKF_INTERFACE_FORK_SAFE) {
		return CKR_ARGUMENTS_BAD;
	}
	if (flags & CKF_INTERFACE_FORK_SAFE) {
		/* 本模块不保证 fork 安全（密钥库句柄、pthread 锁都不是）——
		 * 如实拒绝，不要给一个做不到的承诺。 */
		return CKR_FUNCTION_FAILED;
	}
	*ppInterface = &g_interface;
	return CKR_OK;
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
