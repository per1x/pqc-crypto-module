/* pqchsm/slot.h —— 槽位管理器（slot / token / object / session）
 *
 * 对标 PKCS#11 v3.2 的对象模型，实现的设计规格：
 *   slot    = 逻辑插槽
 *   token   = 插槽中的密钥容器
 *   object  = 密钥 + 属性，对外只以**句柄**暴露
 *   session = 登录会话，携带角色（SO / User）
 *
 * 安全边界：
 *   明文密钥此阶段位于进程内存（best-effort mlock + 用后清零），
 *   上层与调用方**只见句柄**，永远拿不到明文私钥指针 —— 这条现在就成立，
 *   所以 把存储换成 PL 的 Key Vault 时，本头文件不用改。
 *
 * 密码运算一律经 pqchsm/pqc.h 的后端 vtable，不直接调 liboqs。
 */
#ifndef PQCHSM_SLOT_H
#define PQCHSM_SLOT_H

#include <stddef.h>
#include <stdint.h>

#include "pqchsm/pqc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 状态码 ------------------------------------------------------------- */
typedef enum {
	HSM_OK = 0,
	HSM_ERR_BAD_ARG,
	HSM_ERR_BAD_STATE,        /* 非法状态转移 */
	HSM_ERR_NOT_AUTHORIZED,   /* ACL 拒绝：角色不对或未登录 */
	HSM_ERR_PIN_INCORRECT,
	HSM_ERR_PIN_LOCKED,       /* 失败计数超限，仅 SO 可解锁 */
	HSM_ERR_BAD_HANDLE,       /* 句柄无效或已因 destroy/zeroize 失效 */
	HSM_ERR_USAGE_DENIED,     /* 用途位不允许该操作 */
	HSM_ERR_POLICY,           /* 策略位禁止（如不可导出） */
	HSM_ERR_INTEGRITY,        /* 元数据 KMAC 校验失败 —— 疑似离线篡改 */
	HSM_ERR_SLOT_BUSY,
	HSM_ERR_CRYPTO,
	HSM_ERR_NOMEM,
	HSM_ERR_FULL,
} hsm_status_t;

const char *hsm_strerror(hsm_status_t st);

/* ---- 生命周期状态机---------------------------------------------- */
typedef enum {
	SLOT_ST_INVALID = -1,
	SLOT_ST_UNINIT  = 0,   /* 未初始化 */
	SLOT_ST_EMPTY,         /* 空（已 init_token） */
	SLOT_ST_LOADED,        /* 已装载 */
	SLOT_ST_IN_USE,        /* 使用中 */
	SLOT_ST_LOCKED,        /* 冻结/锁定 */
	SLOT_ST__COUNT
} slot_state_t;

typedef enum {
	SLOT_EV_INIT_TOKEN = 0,
	SLOT_EV_GENERATE,      /* 片内生成密钥 */
	SLOT_EV_LOAD,          /* 由种子/包裹装载 */
	SLOT_EV_USE_BEGIN,
	SLOT_EV_USE_END,
	SLOT_EV_DESTROY,       /* 销毁对象，槽位回到"空" */
	SLOT_EV_ZEROIZE,       /* 任意状态可达且不可逆 */
	SLOT_EV_PIN_LOCKOUT,   /* PIN 连续错误超限 */
	SLOT_EV_SO_UNLOCK,
	SLOT_EV__COUNT
} slot_event_t;

/* 纯函数形式的转移表：非法转移返回 SLOT_ST_INVALID。
 * 所有真实操作都必须经过它，这样 test_slot_fsm 的穷举才有意义。
 *
 * 注意 SLOT_EV_SO_UNLOCK：直观上是"解锁→回已装载"，但若锁定发生在
 * 空槽位上，回到"已装载"就是在谎报槽位有密钥。因此本实现解锁后**恢复到
 * 锁定前的状态**，该状态由 slot_fsm_unlock_target() 给出。 */
slot_state_t slot_fsm_next(slot_state_t cur, slot_event_t ev);
slot_state_t slot_fsm_unlock_target(slot_state_t pre_lock);

const char *slot_state_name(slot_state_t s);
const char *slot_event_name(slot_event_t e);

/* ---- 角色与访问控制---------------------------------------------- */
typedef enum {
	HSM_ROLE_PUBLIC = 0,   /* 未登录 */
	HSM_ROLE_SO,           /* 安全官：初始化 / 解锁 / 清零 / 备份恢复 */
	HSM_ROLE_USER,         /* 日常密钥操作 */
} hsm_role_t;

/* ---- 密钥用途与策略---------------------------------------------- */
typedef enum {
	KEY_USAGE_ENCAP  = 1u << 0,
	KEY_USAGE_DECAP  = 1u << 1,
	KEY_USAGE_SIGN   = 1u << 2,
	KEY_USAGE_VERIFY = 1u << 3,
} key_usage_t;

typedef enum {
	SLOT_POLICY_EXTRACTABLE  = 1u << 0,  /* 允许明文导出 —— 默认关，红线 */
	SLOT_POLICY_BACKUPABLE   = 1u << 1,  /* 允许被 KEK 包裹备份（第 3/4 步） */
	SLOT_POLICY_INJECTABLE   = 1u << 2,  /* 允许注入更新 */
	SLOT_POLICY_SEED_STORAGE = 1u << 3,  /* 只存种子，用时重展开 */
} slot_policy_t;

/* ---- 槽位元数据-------------------------------------------------- */
#define SLOT_LABEL_MAX   32
#define SLOT_META_VERSION 1u

typedef struct {
	uint32_t     version;
	uint32_t     slot_id;
	char         label[SLOT_LABEL_MAX];
	pqc_alg_t    alg;
	uint32_t     usage;        /* key_usage_t 位组合 */
	uint32_t     policy;       /* slot_policy_t 位组合 */
	slot_state_t state;
	uint64_t     use_count;
	uint32_t     so_pin_fails;
	uint32_t     user_pin_fails;
	uint64_t     created_at;
	uint64_t     last_used_at;
	uint32_t     generation;   /* 句柄代数；destroy/zeroize 递增使旧句柄失效 */
} slot_meta_t;

/* ---- 句柄 --------------------------------------------------------------- */
typedef uint32_t hsm_slot_id_t;
typedef uint64_t hsm_handle_t;    /* 对象句柄；0 恒为无效 */
typedef uint64_t hsm_session_t;   /* 会话句柄；0 恒为无效 */

#define HSM_INVALID_HANDLE ((hsm_handle_t)0)

/* ---- Token ------------------------------------------------------------- */
typedef struct hsm_token hsm_token_t;

#define HSM_PIN_MIN_LEN   4
#define HSM_PIN_MAX_LEN   64
#define HSM_PIN_MAX_FAILS 3      /* 超过即锁定 */

hsm_token_t *hsm_token_new(size_t n_slots);
void         hsm_token_free(hsm_token_t *tok);
size_t       hsm_token_slot_count(const hsm_token_t *tok);

/* 读一份元数据快照（含 KMAC 校验）。校验失败返回 HSM_ERR_INTEGRITY。 */
hsm_status_t hsm_slot_get_meta(hsm_token_t *tok, hsm_slot_id_t slot, slot_meta_t *out);
hsm_status_t hsm_slot_get_state(hsm_token_t *tok, hsm_slot_id_t slot, slot_state_t *out);

/* 查询 SO / User PIN 是否已设置。PKCS#11 的 C_GetTokenInfo 需要这两位
 * （CKF_TOKEN_INITIALIZED / CKF_USER_PIN_INITIALIZED）。
 * 刻意不放进 slot_meta_t：那会改变元数据的 wire 格式，而这两位在包裹内已经存了。 */
hsm_status_t hsm_slot_pin_status(hsm_token_t *tok, hsm_slot_id_t slot,
                                 int *so_pin_set, int *user_pin_set);

/* ---- 初始化与 PIN ------------------------------------------------------- */
/* 设备级供应操作：UNINIT → EMPTY，同时确立 SO PIN。
 * 起需由"制造模式"门控，此处先不要求会话。 */
hsm_status_t hsm_slot_init_token(hsm_token_t *tok, hsm_slot_id_t slot,
                                 const char *label, const char *so_pin);

/* 需 SO 会话 */
hsm_status_t hsm_slot_set_user_pin(hsm_token_t *tok, hsm_session_t sess, const char *user_pin);

/* ---- 会话 --------------------------------------------------------------- */
hsm_status_t hsm_session_open(hsm_token_t *tok, hsm_slot_id_t slot, hsm_session_t *out);
hsm_status_t hsm_session_close(hsm_token_t *tok, hsm_session_t sess);
hsm_status_t hsm_session_login(hsm_token_t *tok, hsm_session_t sess,
                               hsm_role_t role, const char *pin);
hsm_status_t hsm_session_logout(hsm_token_t *tok, hsm_session_t sess);
hsm_status_t hsm_session_role(hsm_token_t *tok, hsm_session_t sess, hsm_role_t *out);

/* ---- 对象操作（全部句柄进、句柄出）------------------------------------- */
/* 需 User 会话；EMPTY → LOADED。usage 必须与 alg 的种类相符且不跨类。 */
hsm_status_t hsm_slot_generate(hsm_token_t *tok, hsm_session_t sess,
                               pqc_alg_t alg, uint32_t usage, uint32_t policy,
                               hsm_handle_t *out);

/* 由种子装载；同样 EMPTY → LOADED。 */
hsm_status_t hsm_slot_load_seed(hsm_token_t *tok, hsm_session_t sess,
                                pqc_alg_t alg, uint32_t usage, uint32_t policy,
                                const uint8_t *seed, size_t seed_len,
                                hsm_handle_t *out);

/* 公钥可以出边界；私钥永远不行 —— 本头文件不提供任何导出私钥的入口。 */
hsm_status_t hsm_object_public_key(hsm_token_t *tok, hsm_session_t sess,
                                   hsm_handle_t h, uint8_t *out, size_t cap, size_t *out_len);

hsm_status_t hsm_object_sign(hsm_token_t *tok, hsm_session_t sess, hsm_handle_t h,
                             const uint8_t *msg, size_t msg_len,
                             const uint8_t *ctx, size_t ctx_len,
                             uint8_t *sig, size_t cap, size_t *sig_len);

hsm_status_t hsm_object_decaps(hsm_token_t *tok, hsm_session_t sess, hsm_handle_t h,
                               const uint8_t *ct, size_t ct_len,
                               uint8_t *ss, size_t cap, size_t *ss_len);

/* 销毁对象：LOADED → EMPTY，generation 递增使旧句柄立即失效。需 User。 */
hsm_status_t hsm_object_destroy(hsm_token_t *tok, hsm_session_t sess, hsm_handle_t h);

/* ---- zeroize 与解锁 ----------------------------------------------------- */
/* 任意状态可达、不可逆：清密钥材料 + 元数据 + PIN，回到 UNINIT。需 SO。 */
hsm_status_t hsm_slot_zeroize(hsm_token_t *tok, hsm_session_t sess, hsm_slot_id_t slot);

/* 无会话的设备级紧急清零（对应硬件 tamper 线，）。 */
hsm_status_t hsm_slot_zeroize_forced(hsm_token_t *tok, hsm_slot_id_t slot);

/* 解锁并重置 User PIN 失败计数；恢复到锁定前的状态。需 SO。 */
hsm_status_t hsm_slot_unlock(hsm_token_t *tok, hsm_session_t sess, hsm_slot_id_t slot);

/* ---- 安全状态的同步持久化 ----------------------------------------------
 *
 * 【为什么必须有这一层，而不是"退出时存一次"】
 * PIN 失败计数、锁定状态、已销毁的对象、策略位 —— 这些都是**安全状态**。
 * 只改内存、等 CMD_SAVE 才落盘的话，攻击者拔一次电就能把它们全部回到从前：
 * 试三次 PIN → 拔电 → 再试三次，锁定形同虚设。
 *
 * 所以槽位层在**每一次安全状态变化之后**回调这个钩子，由上层（daemon / p11）
 * 决定"落盘"具体是什么。钩子在**放掉所有槽位锁之后**调用，因此实现里可以
 * 放心地再去读所有槽位（hsm_keystore_save 就是这么做的）。
 *
 * 返回非 0 表示落盘失败。槽位层会把它变成 HSM_ERR_CRYPTO 交回调用方 ——
 * fail-closed：宁可让这次操作报错，也不要"内存里锁了、盘上没锁"。
 *
 * fn 为 NULL 解除挂接（默认就是没挂，纯内存 token 不受影响）。 */
typedef int (*hsm_persist_fn)(hsm_token_t *tok, void *user);
void hsm_token_set_persist_hook(hsm_token_t *tok, hsm_persist_fn fn, void *user);

/* ---- 审计-------------------------------------------------------- */
/* 挂接 append-only 哈希链日志。挂上之后所有敏感操作自动落审计；
 * log 的生命周期由调用方管理，传 NULL 解除挂接。
 * 前向声明避免 slot.h 依赖 audit.h。 */
struct audit_log;
void hsm_token_attach_audit(hsm_token_t *tok, struct audit_log *log);

/* ---- 测试用内省接口 ----------------------------------------------------- */
/* 直接驱动状态机（绕过 ACL），供 test_slot_fsm 做全状态 × 全事件穷举。
 * 只在测试里用；正式路径一律走上面的操作函数。 */
hsm_status_t hsm_slot_force_state(hsm_token_t *tok, hsm_slot_id_t slot, slot_state_t st);

/* 篡改探测测试用：拿到元数据 KMAC 标签的副本 */
hsm_status_t hsm_slot_meta_tag(hsm_token_t *tok, hsm_slot_id_t slot, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
#endif /* PQCHSM_SLOT_H */
