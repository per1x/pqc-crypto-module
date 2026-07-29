/* 内部头：槽位管理器的私有结构。只给 src/slot/ 与 src/store/ 用。
 *
 * 之所以要有这个头：密钥库持久化（persist.c）与备份恢复需要读写槽位的
 * 完整状态（元数据 + PIN 材料 + 密钥材料），但这些结构绝不能出现在
 * pqchsm/slot.h 里 —— 对外只有句柄。
 */
#ifndef PQCHSM_SLOT_INTERNAL_H
#define PQCHSM_SLOT_INTERNAL_H

#include "pqchsm/slot.h"
#include "meta.h"

#include <pthread.h>

#define MAX_SESSIONS 16
#define PIN_SALT_LEN 16
#define VERIFIER_LEN 32

typedef struct {
	int           open;
	hsm_slot_id_t slot;
	hsm_role_t    role;
	uint32_t      gen;
} session_t;

typedef struct {
	pthread_mutex_t lock;
	slot_meta_t  meta;
	uint8_t      meta_tag[SLOT_META_TAG_LEN];
	slot_state_t pre_lock;

	/* 每槽位一把随机 PIN 密钥：PIN 验证值由它派生，而不是直接由 KDR 派生。
	 * 原因见 persist.c ——  KDR 派生的验证值无法跨设备恢复，会让恢复到新设备的
	 * token 永远登录不上。pin_key 本身放在 KEK/BEK 包裹内，因此离线拿到
	 * 密钥库文件的攻击者仍然无法爆破 PIN。 */
	uint8_t pin_key[32];
	uint8_t so_salt[PIN_SALT_LEN],   so_verifier[VERIFIER_LEN];
	uint8_t user_salt[PIN_SALT_LEN], user_verifier[VERIFIER_LEN];
	int     has_so_pin, has_user_pin;

	uint8_t *pk;   size_t pk_len;
	uint8_t *sk;   size_t sk_len;     /* SEED_STORAGE 策略下恒为 NULL */
	uint8_t  seed[64]; size_t seed_len; int has_seed;
} slot_t;

struct hsm_token {
	pthread_mutex_t tlock;      /* 只保护会话表 */
	size_t    n_slots;
	slot_t   *slots;
	session_t sessions[MAX_SESSIONS];
	uint32_t  session_gen;

	/* 审计日志。单独一把锁：审计要 fsync，不该卡住会话表。 */
	pthread_mutex_t audit_lock;
	struct audit_log *audit;
};

/* 落一条审计。detail 只能放非敏感的短文本（算法名、标签），
 * 绝不能放密钥材料/种子/PIN。在**放掉槽位锁之后**调用。 */
void slot_audit(hsm_token_t *tok, int op, hsm_role_t role,
                hsm_slot_id_t slot, hsm_status_t result, const char *detail);

#define SLOCK(s)   pthread_mutex_lock(&(s)->lock)
#define SUNLOCK(s) pthread_mutex_unlock(&(s)->lock)

/* 定义在 slot.c，供 persist.c 复用（调用方须持槽位锁）*/
hsm_status_t slot_reseal(slot_t *s);
hsm_status_t slot_check_integrity(const slot_t *s);
void         slot_wipe_key_material(slot_t *s);
void         slot_wipe_pins(slot_t *s);
slot_t      *slot_at(hsm_token_t *tok, hsm_slot_id_t id);

#endif
