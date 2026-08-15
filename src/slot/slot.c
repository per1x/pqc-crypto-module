/* slot.c —— 槽位管理器实现
 *
 * 四条贯穿全文件的规矩：
 *   1. 任何状态变化都必须经 slot_fsm_next()，非法转移返回 HSM_ERR_BAD_STATE；
 *   2. 任何元数据变化后立刻重新盖 KMAC 标签；任何读取前先验标签；
 *   3. 明文私钥只在 pqc_* 调用的那一瞬间存在于本文件的栈/堆缓冲里，
 *      用完即 pqc_secure_zero —— 对外接口一律句柄进句柄出；
 *   4. 并发：会话表由 tok->tlock 保护，每个槽位有自己的 lock。
 *      **锁序恒为 tlock → slot->lock，绝不反向**；需要同时改会话与槽位时，
 *      先做完槽位那半、放掉槽位锁，再去拿会话锁。
 */
#include "pqchsm/slot.h"

#include "slot_internal.h"
#include "pqchsm/audit.h"
#include "pqchsm/kdf.h"
#include "pqchsm/kdr.h"
#include "pqchsm/util.h"

#include <openssl/rand.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 私有结构与锁宏见 slot_internal.h */

const char *hsm_strerror(hsm_status_t st)
{
	switch (st) {
	case HSM_OK:                 return "ok";
	case HSM_ERR_BAD_ARG:        return "bad argument";
	case HSM_ERR_BAD_STATE:      return "illegal state transition";
	case HSM_ERR_NOT_AUTHORIZED: return "not authorized";
	case HSM_ERR_PIN_INCORRECT:  return "incorrect PIN";
	case HSM_ERR_PIN_LOCKED:     return "slot locked";
	case HSM_ERR_BAD_HANDLE:     return "invalid handle";
	case HSM_ERR_USAGE_DENIED:   return "key usage denied";
	case HSM_ERR_POLICY:         return "policy denied";
	case HSM_ERR_INTEGRITY:      return "metadata integrity failure";
	case HSM_ERR_SLOT_BUSY:      return "slot busy";
	case HSM_ERR_CRYPTO:         return "crypto failure";
	case HSM_ERR_NOMEM:          return "out of memory";
	case HSM_ERR_FULL:           return "no free session";
	}
	return "unknown";
}

/* ---- 内部辅助（调用方须已持有对应槽位锁）------------------------------- */

static uint64_t now_secs(void)
{
	return (uint64_t)time(NULL);
}

hsm_status_t slot_reseal(slot_t *s)
{
	return slot_meta_seal(&s->meta, s->meta_tag) == 0 ? HSM_OK : HSM_ERR_CRYPTO;
}

hsm_status_t slot_check_integrity(const slot_t *s)
{
	return slot_meta_verify(&s->meta, s->meta_tag) == 0 ? HSM_OK : HSM_ERR_INTEGRITY;
}

static hsm_status_t fsm_apply(slot_t *s, slot_event_t ev)
{
	slot_state_t nxt = slot_fsm_next(s->meta.state, ev);
	if (nxt == SLOT_ST_INVALID) {
		return HSM_ERR_BAD_STATE;
	}
	if (ev == SLOT_EV_PIN_LOCKOUT) {
		s->pre_lock = s->meta.state;
	} else if (ev == SLOT_EV_SO_UNLOCK) {
		nxt = slot_fsm_unlock_target(s->pre_lock);
	}
	s->meta.state = nxt;
	return HSM_OK;
}

void slot_wipe_key_material(slot_t *s)
{
	if (s->pk) {
		pqc_secure_free(s->pk, s->pk_len);
		s->pk = NULL;
	}
	if (s->sk) {
		pqc_secure_free(s->sk, s->sk_len);
		s->sk = NULL;
	}
	s->pk_len = s->sk_len = 0;
	pqc_secure_zero(s->seed, sizeof(s->seed));
	s->seed_len = 0;
	s->has_seed = 0;
	/* 硬件槽的句柄也要跟着作废。**这里只作废本进程的引用** ——
	 * PL 金库里那份 dk 由硬件的 zeroize 或下一次 PL 重配清掉，
	 * 不是这个函数能做到的事。留着一个指向已销毁对象的句柄更危险：
	 * 槽位复用之后它会指向别人的私钥。 */
	s->hw_handle = 0;
	s->hw_resident = 0;
}

void slot_wipe_pins(slot_t *s)
{
	pqc_secure_zero(s->pin_key, sizeof(s->pin_key));
	pqc_secure_zero(s->so_salt, sizeof(s->so_salt));
	pqc_secure_zero(s->so_verifier, sizeof(s->so_verifier));
	pqc_secure_zero(s->user_salt, sizeof(s->user_salt));
	pqc_secure_zero(s->user_verifier, sizeof(s->user_verifier));
	s->has_so_pin = s->has_user_pin = 0;
}

static hsm_handle_t make_handle(const slot_t *s)
{
	return ((hsm_handle_t)s->meta.generation << 32) | (hsm_handle_t)(s->meta.slot_id + 1);
}

/* ---- 会话表访问（内部自取 tlock，返回值拷贝而非指针）------------------- */

static hsm_status_t session_info(hsm_token_t *tok, hsm_session_t sess,
                                 hsm_slot_id_t *slot, hsm_role_t *role)
{
	if (!tok || sess == 0) {
		return HSM_ERR_BAD_ARG;
	}
	uint32_t idx = (uint32_t)(sess & 0xffffffffu);
	uint32_t gen = (uint32_t)(sess >> 32);
	if (idx == 0 || idx > MAX_SESSIONS) {
		return HSM_ERR_BAD_ARG;
	}
	hsm_status_t st = HSM_ERR_BAD_ARG;
	pthread_mutex_lock(&tok->tlock);
	session_t *s = &tok->sessions[idx - 1];
	if (s->open && s->gen == gen) {
		if (slot) {
			*slot = s->slot;
		}
		if (role) {
			*role = s->role;
		}
		st = HSM_OK;
	}
	pthread_mutex_unlock(&tok->tlock);
	return st;
}

static hsm_status_t require_role(hsm_token_t *tok, hsm_session_t sess,
                                 hsm_role_t need, hsm_slot_id_t *slot)
{
	hsm_role_t role;
	hsm_status_t st = session_info(tok, sess, slot, &role);
	if (st != HSM_OK) {
		return st;
	}
	return role == need ? HSM_OK : HSM_ERR_NOT_AUTHORIZED;   /* 默认拒绝 */
}

static void set_session_role(hsm_token_t *tok, hsm_session_t sess, hsm_role_t role)
{
	uint32_t idx = (uint32_t)(sess & 0xffffffffu);
	uint32_t gen = (uint32_t)(sess >> 32);
	if (idx == 0 || idx > MAX_SESSIONS) {
		return;
	}
	pthread_mutex_lock(&tok->tlock);
	session_t *s = &tok->sessions[idx - 1];
	if (s->open && s->gen == gen) {
		s->role = role;
	}
	pthread_mutex_unlock(&tok->tlock);
}

/* 某槽位被清零后，把所有指向它的会话降级为未登录 */
static void drop_sessions_on_slot(hsm_token_t *tok, hsm_slot_id_t slot)
{
	pthread_mutex_lock(&tok->tlock);
	for (int i = 0; i < MAX_SESSIONS; i++) {
		if (tok->sessions[i].open && tok->sessions[i].slot == slot) {
			tok->sessions[i].role = HSM_ROLE_PUBLIC;
		}
	}
	pthread_mutex_unlock(&tok->tlock);
}

/* ：用途互斥，禁止一钥多用 —— 位必须落在该算法种类的允许集合内 */
static hsm_status_t validate_usage(pqc_alg_t alg, uint32_t usage)
{
	const pqc_alg_info_t *info = pqc_alg_info(alg);
	if (!info || usage == 0) {
		return HSM_ERR_BAD_ARG;
	}
	uint32_t allowed = (info->kind == PQC_KIND_KEM)
	                   ? (uint32_t)(KEY_USAGE_ENCAP | KEY_USAGE_DECAP)
	                   : (uint32_t)(KEY_USAGE_SIGN | KEY_USAGE_VERIFY);
	return (usage & ~allowed) ? HSM_ERR_USAGE_DENIED : HSM_OK;
}

/* PIN 验证值 = KMAC256(pin_key, slot_id ‖ role ‖ salt ‖ pin)。
 * 不存明文、不存可离线爆破的哈希：pin_key 是每槽位随机的 32 字节，
 * 且只存在于 KEK/BEK 包裹内部，攻击者拿到密钥库文件也无法离线枚举 PIN。
 * （若直接用 KDR 派生，跨设备恢复后将无法登录 —— 见 persist.c） */
static int pin_verifier(const uint8_t pin_key[32], uint32_t slot_id, hsm_role_t role,
                        const uint8_t *salt, const char *pin, uint8_t out[VERIFIER_LEN])
{
	uint8_t buf[4 + 4 + PIN_SALT_LEN + HSM_PIN_MAX_LEN];
	size_t pin_len = strlen(pin);
	if (pin_len > HSM_PIN_MAX_LEN) {
		return -1;
	}
	size_t n = 0;
	for (int i = 0; i < 4; i++) {
		buf[n++] = (uint8_t)(slot_id >> (8 * i));
	}
	uint32_t r = (uint32_t)role;
	for (int i = 0; i < 4; i++) {
		buf[n++] = (uint8_t)(r >> (8 * i));
	}
	memcpy(buf + n, salt, PIN_SALT_LEN);
	n += PIN_SALT_LEN;
	memcpy(buf + n, pin, pin_len);
	n += pin_len;

	int rc = pqc_kmac256(pin_key, 32, buf, n, "pqc-hsm/pin-verifier", out, VERIFIER_LEN);
	pqc_secure_zero(buf, sizeof(buf));
	return rc;
}

static int valid_pin(const char *pin)
{
	if (!pin) {
		return 0;
	}
	size_t n = strlen(pin);
	return n >= HSM_PIN_MIN_LEN && n <= HSM_PIN_MAX_LEN;
}

void hsm_token_attach_audit(hsm_token_t *tok, struct audit_log *log)
{
	if (!tok) {
		return;
	}
	pthread_mutex_lock(&tok->audit_lock);
	tok->audit = log;
	pthread_mutex_unlock(&tok->audit_lock);
}

void slot_audit(hsm_token_t *tok, int op, hsm_role_t role,
                hsm_slot_id_t slot, hsm_status_t result, const char *detail)
{
	if (!tok) {
		return;
	}
	pthread_mutex_lock(&tok->audit_lock);
	if (tok->audit) {
		/* 失败也要落审计。
		 * detail 只放算法名/标签这类非敏感短文本。 */
		(void)audit_append(tok->audit, now_secs(), (audit_op_t)op,
		                   (uint32_t)role, (uint32_t)slot, (uint32_t)result, detail);
	}
	pthread_mutex_unlock(&tok->audit_lock);
}

/* ---- Token 生命周期 ----------------------------------------------------- */

hsm_token_t *hsm_token_new(size_t n_slots)
{
	if (n_slots == 0 || n_slots > 4096) {
		return NULL;
	}
	hsm_token_t *tok = calloc(1, sizeof(*tok));
	if (!tok) {
		return NULL;
	}
	tok->slots = calloc(n_slots, sizeof(slot_t));
	if (!tok->slots) {
		free(tok);
		return NULL;
	}
	pthread_mutex_init(&tok->tlock, NULL);
	pthread_mutex_init(&tok->audit_lock, NULL);
	tok->n_slots = n_slots;
	for (size_t i = 0; i < n_slots; i++) {
		slot_t *s = &tok->slots[i];
		pthread_mutex_init(&s->lock, NULL);
		s->meta.version = SLOT_META_VERSION;
		s->meta.slot_id = (uint32_t)i;
		s->meta.alg     = PQC_ALG_NONE;
		s->meta.state   = SLOT_ST_UNINIT;
		s->pre_lock     = SLOT_ST_EMPTY;
		if (slot_reseal(s) != HSM_OK) {
			tok->n_slots = i + 1;
			hsm_token_free(tok);
			return NULL;
		}
	}
	return tok;
}

void hsm_token_free(hsm_token_t *tok)
{
	if (!tok) {
		return;
	}
	if (tok->slots) {
		for (size_t i = 0; i < tok->n_slots; i++) {
			slot_t *s = &tok->slots[i];
			slot_wipe_key_material(s);
			slot_wipe_pins(s);
			pthread_mutex_destroy(&s->lock);
		}
		pqc_secure_zero(tok->slots, tok->n_slots * sizeof(slot_t));
		free(tok->slots);
	}
	pthread_mutex_destroy(&tok->tlock);
	pthread_mutex_destroy(&tok->audit_lock);
	pqc_secure_zero(tok, sizeof(*tok));
	free(tok);
}

size_t hsm_token_slot_count(const hsm_token_t *tok)
{
	return tok ? tok->n_slots : 0;
}

slot_t *slot_at(hsm_token_t *tok, hsm_slot_id_t id)
{
	if (!tok || id >= tok->n_slots) {
		return NULL;
	}
	return &tok->slots[id];
}

hsm_status_t hsm_slot_get_meta(hsm_token_t *tok, hsm_slot_id_t id, slot_meta_t *out)
{
	slot_t *s = slot_at(tok, id);
	if (!s || !out) {
		return HSM_ERR_BAD_ARG;
	}
	SLOCK(s);
	hsm_status_t st = slot_check_integrity(s);
	if (st == HSM_OK) {
		*out = s->meta;
	}
	SUNLOCK(s);
	return st;
}

hsm_status_t hsm_slot_get_state(hsm_token_t *tok, hsm_slot_id_t id, slot_state_t *out)
{
	slot_t *s = slot_at(tok, id);
	if (!s || !out) {
		return HSM_ERR_BAD_ARG;
	}
	SLOCK(s);
	hsm_status_t st = slot_check_integrity(s);
	if (st == HSM_OK) {
		*out = s->meta.state;
	}
	SUNLOCK(s);
	return st;
}

hsm_status_t hsm_slot_pin_status(hsm_token_t *tok, hsm_slot_id_t slot,
                                 int *so_pin_set, int *user_pin_set)
{
	slot_t *s = slot_at(tok, slot);
	if (!s) {
		return HSM_ERR_BAD_ARG;
	}
	SLOCK(s);
	if (so_pin_set) {
		*so_pin_set = s->has_so_pin;
	}
	if (user_pin_set) {
		*user_pin_set = s->has_user_pin;
	}
	SUNLOCK(s);
	return HSM_OK;
}

hsm_status_t hsm_slot_meta_tag(hsm_token_t *tok, hsm_slot_id_t id, uint8_t out[32])
{
	slot_t *s = slot_at(tok, id);
	if (!s || !out) {
		return HSM_ERR_BAD_ARG;
	}
	SLOCK(s);
	memcpy(out, s->meta_tag, SLOT_META_TAG_LEN);
	SUNLOCK(s);
	return HSM_OK;
}

hsm_status_t hsm_slot_force_state(hsm_token_t *tok, hsm_slot_id_t id, slot_state_t want_state)
{
	slot_t *s = slot_at(tok, id);
	if (!s) {
		return HSM_ERR_BAD_ARG;
	}
	if (want_state < 0 || want_state >= SLOT_ST__COUNT) {
		return HSM_ERR_BAD_ARG;
	}
	SLOCK(s);
	s->meta.state = want_state;
	hsm_status_t st = slot_reseal(s);
	SUNLOCK(s);
	return st;
}

/* ---- 初始化与 PIN ------------------------------------------------------- */

hsm_status_t hsm_slot_init_token(hsm_token_t *tok, hsm_slot_id_t id,
                                 const char *label, const char *so_pin)
{
	slot_t *s = slot_at(tok, id);
	if (!s) {
		return HSM_ERR_BAD_ARG;
	}
	if (!label || !valid_pin(so_pin)) {
		return HSM_ERR_BAD_ARG;
	}
	SLOCK(s);
	hsm_status_t st = slot_check_integrity(s);
	if (st != HSM_OK) {
		goto out;
	}
	st = fsm_apply(s, SLOT_EV_INIT_TOKEN);
	if (st != HSM_OK) {
		goto out;
	}
	/* init_token 时给这个槽位生成一把全新的 PIN 密钥 */
	if (RAND_bytes(s->pin_key, sizeof(s->pin_key)) != 1 ||
	    RAND_bytes(s->so_salt, PIN_SALT_LEN) != 1 ||
	    pin_verifier(s->pin_key, s->meta.slot_id, HSM_ROLE_SO, s->so_salt,
	                 so_pin, s->so_verifier) != 0) {
		st = HSM_ERR_CRYPTO;
		goto out;
	}
	s->has_so_pin = 1;
	memset(s->meta.label, 0, SLOT_LABEL_MAX);
	strncpy(s->meta.label, label, SLOT_LABEL_MAX - 1);
	s->meta.created_at     = now_secs();
	s->meta.so_pin_fails   = 0;
	s->meta.user_pin_fails = 0;
	s->meta.use_count      = 0;
	st = slot_reseal(s);
out:
	SUNLOCK(s);
	slot_audit(tok, AUDIT_OP_INIT_TOKEN, HSM_ROLE_SO, id, st, label);
	return st;
}

hsm_status_t hsm_slot_set_user_pin(hsm_token_t *tok, hsm_session_t sess, const char *user_pin)
{
	hsm_slot_id_t id;
	hsm_status_t st = require_role(tok, sess, HSM_ROLE_SO, &id);
	if (st != HSM_OK) {
		return st;
	}
	if (!valid_pin(user_pin)) {
		return HSM_ERR_BAD_ARG;
	}
	slot_t *s = slot_at(tok, id);
	if (!s) {
		return HSM_ERR_BAD_ARG;
	}
	SLOCK(s);
	st = slot_check_integrity(s);
	if (st != HSM_OK) {
		goto out;
	}
	if (s->meta.state == SLOT_ST_UNINIT) {
		st = HSM_ERR_BAD_STATE;
		goto out;
	}
	if (RAND_bytes(s->user_salt, PIN_SALT_LEN) != 1 ||
	    pin_verifier(s->pin_key, s->meta.slot_id, HSM_ROLE_USER, s->user_salt, user_pin,
	                 s->user_verifier) != 0) {
		st = HSM_ERR_CRYPTO;
		goto out;
	}
	s->has_user_pin = 1;
	s->meta.user_pin_fails = 0;
	st = slot_reseal(s);
out:
	SUNLOCK(s);
	slot_audit(tok, AUDIT_OP_UNLOCK, HSM_ROLE_SO, id, st, NULL);
	return st;
}

/* ---- 会话 --------------------------------------------------------------- */

hsm_status_t hsm_session_open(hsm_token_t *tok, hsm_slot_id_t id, hsm_session_t *out)
{
	if (!slot_at(tok, id) || !out) {
		return HSM_ERR_BAD_ARG;
	}
	hsm_status_t st = HSM_ERR_FULL;
	pthread_mutex_lock(&tok->tlock);
	for (int i = 0; i < MAX_SESSIONS; i++) {
		if (!tok->sessions[i].open) {
			tok->sessions[i].open = 1;
			tok->sessions[i].slot = id;
			tok->sessions[i].role = HSM_ROLE_PUBLIC;
			tok->sessions[i].gen  = ++tok->session_gen;
			*out = ((hsm_session_t)tok->sessions[i].gen << 32) | (hsm_session_t)(i + 1);
			st = HSM_OK;
			break;
		}
	}
	pthread_mutex_unlock(&tok->tlock);
	return st;
}

hsm_status_t hsm_session_close(hsm_token_t *tok, hsm_session_t sess)
{
	hsm_status_t st = session_info(tok, sess, NULL, NULL);
	if (st != HSM_OK) {
		return st;
	}
	uint32_t idx = (uint32_t)(sess & 0xffffffffu);
	pthread_mutex_lock(&tok->tlock);
	memset(&tok->sessions[idx - 1], 0, sizeof(session_t));
	pthread_mutex_unlock(&tok->tlock);
	return HSM_OK;
}

hsm_status_t hsm_session_role(hsm_token_t *tok, hsm_session_t sess, hsm_role_t *out)
{
	if (!out) {
		return HSM_ERR_BAD_ARG;
	}
	return session_info(tok, sess, NULL, out);
}

hsm_status_t hsm_session_logout(hsm_token_t *tok, hsm_session_t sess)
{
	hsm_status_t st = session_info(tok, sess, NULL, NULL);
	if (st != HSM_OK) {
		return st;
	}
	set_session_role(tok, sess, HSM_ROLE_PUBLIC);
	return HSM_OK;
}

hsm_status_t hsm_session_login(hsm_token_t *tok, hsm_session_t sess,
                               hsm_role_t role, const char *pin)
{
	hsm_slot_id_t id;
	hsm_status_t st = session_info(tok, sess, &id, NULL);
	if (st != HSM_OK) {
		return st;
	}
	if (!pin || (role != HSM_ROLE_SO && role != HSM_ROLE_USER)) {
		return HSM_ERR_BAD_ARG;
	}
	slot_t *s = slot_at(tok, id);
	if (!s) {
		return HSM_ERR_BAD_ARG;
	}

	int login_ok = 0;
	SLOCK(s);
	st = slot_check_integrity(s);
	if (st != HSM_OK) {
		goto out;
	}
	if (s->meta.state == SLOT_ST_UNINIT) {
		st = HSM_ERR_BAD_STATE;      /* 还没有 PIN 可验 */
		goto out;
	}
	/* 槽位锁定时 User 一律拒绝；SO 仍可登录 —— 否则没人能解锁 */
	if (s->meta.state == SLOT_ST_LOCKED && role == HSM_ROLE_USER) {
		st = HSM_ERR_PIN_LOCKED;
		goto out;
	}
	{
		const uint8_t *salt = (role == HSM_ROLE_SO) ? s->so_salt : s->user_salt;
		const uint8_t *want = (role == HSM_ROLE_SO) ? s->so_verifier : s->user_verifier;
		int has = (role == HSM_ROLE_SO) ? s->has_so_pin : s->has_user_pin;
		if (!has) {
			st = HSM_ERR_NOT_AUTHORIZED;
			goto out;
		}
		uint8_t got[VERIFIER_LEN];
		if (pin_verifier(s->pin_key, s->meta.slot_id, role, salt, pin, got) != 0) {
			st = HSM_ERR_CRYPTO;
			goto out;
		}
		int ok = pqc_ct_equal(got, want, VERIFIER_LEN);
		pqc_secure_zero(got, sizeof(got));

		if (ok) {
			if (role == HSM_ROLE_SO) {
				s->meta.so_pin_fails = 0;
			} else {
				s->meta.user_pin_fails = 0;
			}
			login_ok = 1;
			st = slot_reseal(s);
			goto out;
		}
		/* 失败计数持久化在元数据里（并进 KMAC），断电重置绕不过去 */
		if (role == HSM_ROLE_SO) {
			/* SO 失败只计数不锁槽位：锁了就没人能解锁，设备直接变砖。
			 * SO 凭证的兜底恢复归 的 Shamir M-of-N 仪式管。 */
			s->meta.so_pin_fails++;
			(void)slot_reseal(s);
			st = HSM_ERR_PIN_INCORRECT;
			goto out;
		}
		s->meta.user_pin_fails++;
		if (s->meta.user_pin_fails >= HSM_PIN_MAX_FAILS) {
			hsm_status_t lst = fsm_apply(s, SLOT_EV_PIN_LOCKOUT);
			(void)slot_reseal(s);
			st = (lst == HSM_OK) ? HSM_ERR_PIN_LOCKED : HSM_ERR_PIN_INCORRECT;
			goto out;
		}
		(void)slot_reseal(s);
		st = HSM_ERR_PIN_INCORRECT;
	}
out:
	SUNLOCK(s);
	if (login_ok) {
		set_session_role(tok, sess, role);   /* 放掉槽位锁后再动会话表，保持锁序 */
	}
	/* 成功/失败/锁定三种结果分别落审计 */
	slot_audit(tok, login_ok ? AUDIT_OP_LOGIN
	                : (st == HSM_ERR_PIN_LOCKED ? AUDIT_OP_LOCKOUT : AUDIT_OP_LOGIN_FAIL),
	           role, id, st, role == HSM_ROLE_SO ? "SO" : "User");
	return st;
}

/* ---- 对象操作 ----------------------------------------------------------- */

/* 调用方须持槽位锁 */
static hsm_status_t install_key(slot_t *s, pqc_alg_t alg, uint32_t usage, uint32_t policy,
                                const uint8_t *seed, size_t seed_len, hsm_handle_t *out)
{
	const pqc_alg_info_t *info = pqc_alg_info(alg);
	hsm_status_t st = validate_usage(alg, usage);
	if (st != HSM_OK) {
		return st;
	}
	/* ---- 私钥留在硬件里的那条路 ----------------------------------------
	 * 条件有三个，缺一不可：
	 *   · 后端两个句柄操作都在（pqc_backend_has_hw_keys）；
	 *   · 是"生成"而不是"由种子装载"—— 种子装载要求私钥可复现，
	 *     而硬件生成的 d/z 取自 PL 自己的 TRNG，复现不了；
	 *   · 没要求种子存储策略 —— 那条策略的前提就是存着种子。
	 * 任何一条不满足就照旧走软件路径，**不猜、不降级到"假装在硬件里"**。 */
	if (!seed && !(policy & SLOT_POLICY_SEED_STORAGE) && pqc_backend_has_hw_keys()) {
		uint8_t *hpk = pqc_secure_alloc(info->pk_len);
		uint32_t hh = 0;

		if (!hpk) {
			return HSM_ERR_NOMEM;
		}
		if (pqc_keypair_hw(alg, hpk, &hh) == PQC_OK) {
			slot_wipe_key_material(s);
			s->pk = hpk;
			s->pk_len = info->pk_len;
			s->sk = NULL;
			s->sk_len = 0;
			s->has_seed = 0;
			s->hw_handle = hh;
			s->hw_resident = 1;

			s->meta.alg          = alg;
			s->meta.usage        = usage;
			s->meta.policy       = policy;
			s->meta.use_count    = 0;
			s->meta.last_used_at = 0;
			{
				hsm_status_t rs = slot_reseal(s);

				if (rs != HSM_OK) {
					return rs;
				}
			}
			if (out) {
				*out = make_handle(s);
			}
			return HSM_OK;
		}
		/* 硬件路径失败：**不静默退回软件**。上层要的是"私钥在硬件里"，
		 * 悄悄给一把软件密钥是把承诺变成谎话。 */
		pqc_secure_free(hpk, info->pk_len);
		return HSM_ERR_CRYPTO;
	}

	uint8_t *pk = pqc_secure_alloc(info->pk_len);
	uint8_t *sk = pqc_secure_alloc(info->sk_len);
	if (!pk || !sk) {
		pqc_secure_free(pk, info->pk_len);
		pqc_secure_free(sk, info->sk_len);
		return HSM_ERR_NOMEM;
	}

	uint8_t local_seed[64];
	size_t  local_seed_len = info->seed_len;
	if (seed) {
		if (seed_len != info->seed_len) {
			pqc_secure_free(pk, info->pk_len);
			pqc_secure_free(sk, info->sk_len);
			return HSM_ERR_BAD_ARG;
		}
		memcpy(local_seed, seed, seed_len);
	} else if (RAND_bytes(local_seed, (int)local_seed_len) != 1) {
		pqc_secure_zero(local_seed, sizeof(local_seed));
		pqc_secure_free(pk, info->pk_len);
		pqc_secure_free(sk, info->sk_len);
		return HSM_ERR_CRYPTO;
	}

	/* 一律走种子生成：这样"生成"和"由种子装载"是同一条代码路径，
	 * 的种子存储策略才不会成为一条没人走的旁路。 */
	pqc_status_t cst = pqc_keypair_from_seed(alg, local_seed, local_seed_len, pk, sk);
	if (cst != PQC_OK) {
		pqc_secure_zero(local_seed, sizeof(local_seed));
		pqc_secure_free(pk, info->pk_len);
		pqc_secure_free(sk, info->sk_len);
		return HSM_ERR_CRYPTO;
	}

	slot_wipe_key_material(s);
	s->pk = pk;
	s->pk_len = info->pk_len;

	if (policy & SLOT_POLICY_SEED_STORAGE) {
		/* ：只留种子，私钥用时再展开。这里立刻把刚算出的 sk 清掉，
		 * 证明后续签名确实是从种子重展开来的，而不是偷偷留了副本。 */
		memcpy(s->seed, local_seed, local_seed_len);
		s->seed_len = local_seed_len;
		s->has_seed = 1;
		pqc_secure_free(sk, info->sk_len);
		s->sk = NULL;
		s->sk_len = 0;
	} else {
		s->sk = sk;
		s->sk_len = info->sk_len;
	}
	pqc_secure_zero(local_seed, sizeof(local_seed));

	s->meta.alg          = alg;
	s->meta.usage        = usage;
	s->meta.policy       = policy;
	s->meta.use_count    = 0;
	s->meta.last_used_at = 0;
	hsm_status_t rs = slot_reseal(s);
	if (rs != HSM_OK) {
		return rs;
	}
	if (out) {
		*out = make_handle(s);
	}
	return HSM_OK;
}

static hsm_status_t create_object(hsm_token_t *tok, hsm_session_t sess,
                                  pqc_alg_t alg, uint32_t usage, uint32_t policy,
                                  const uint8_t *seed, size_t seed_len,
                                  slot_event_t ev, hsm_handle_t *out)
{
	hsm_slot_id_t id;
	hsm_status_t st = require_role(tok, sess, HSM_ROLE_USER, &id);
	if (st != HSM_OK) {
		return st;
	}
	if (!out || !pqc_alg_info(alg)) {
		return HSM_ERR_BAD_ARG;
	}
	slot_t *s = slot_at(tok, id);
	if (!s) {
		return HSM_ERR_BAD_ARG;
	}
	SLOCK(s);
	st = slot_check_integrity(s);
	if (st != HSM_OK) {
		goto out;
	}
	{
		slot_state_t saved = s->meta.state;
		st = fsm_apply(s, ev);
		if (st != HSM_OK) {
			goto out;
		}
		st = install_key(s, alg, usage, policy, seed, seed_len, out);
		if (st != HSM_OK) {
			/* 装载失败必须回滚，不能停在 LOADED 却没有密钥 */
			s->meta.state = saved;
			(void)slot_reseal(s);
		}
	}
out:
	SUNLOCK(s);
	{
		const pqc_alg_info_t *ai = pqc_alg_info(alg);
		slot_audit(tok, ev == SLOT_EV_GENERATE ? AUDIT_OP_GENERATE : AUDIT_OP_LOAD,
		           HSM_ROLE_USER, id, st, ai ? ai->name : "?");
	}
	return st;
}

hsm_status_t hsm_slot_generate(hsm_token_t *tok, hsm_session_t sess,
                               pqc_alg_t alg, uint32_t usage, uint32_t policy,
                               hsm_handle_t *out)
{
	return create_object(tok, sess, alg, usage, policy, NULL, 0, SLOT_EV_GENERATE, out);
}

hsm_status_t hsm_slot_load_seed(hsm_token_t *tok, hsm_session_t sess,
                                pqc_alg_t alg, uint32_t usage, uint32_t policy,
                                const uint8_t *seed, size_t seed_len, hsm_handle_t *out)
{
	const pqc_alg_info_t *info = pqc_alg_info(alg);
	/* 便宜的参数校验放在状态机之前：不该因为一个长度写错的调用
	 * 就去动槽位状态（哪怕后面会回滚）。 */
	if (!seed || !info || seed_len != info->seed_len) {
		return HSM_ERR_BAD_ARG;
	}
	return create_object(tok, sess, alg, usage, policy, seed, seed_len, SLOT_EV_LOAD, out);
}

/* 句柄校验（调用方须持该槽位锁）：代数与状态都要对得上 */
static hsm_status_t check_handle(const slot_t *s, hsm_handle_t h)
{
	uint32_t sid = (uint32_t)(h & 0xffffffffu);
	uint32_t gen = (uint32_t)(h >> 32);
	if (h == HSM_INVALID_HANDLE || sid != s->meta.slot_id + 1 || gen != s->meta.generation) {
		return HSM_ERR_BAD_HANDLE;
	}
	if (s->meta.state != SLOT_ST_LOADED && s->meta.state != SLOT_ST_IN_USE) {
		return HSM_ERR_BAD_HANDLE;
	}
	return slot_check_integrity(s);
}

hsm_status_t hsm_object_public_key(hsm_token_t *tok, hsm_session_t sess, hsm_handle_t h,
                                   uint8_t *out, size_t cap, size_t *out_len)
{
	hsm_slot_id_t id;
	hsm_status_t st = session_info(tok, sess, &id, NULL);   /* 公钥不要求登录 */
	if (st != HSM_OK) {
		return st;
	}
	slot_t *s = slot_at(tok, id);
	if (!s) {
		return HSM_ERR_BAD_ARG;
	}
	SLOCK(s);
	st = check_handle(s, h);
	if (st != HSM_OK) {
		goto out;
	}
	if (!out || !out_len || cap < s->pk_len) {
		st = HSM_ERR_BAD_ARG;
		goto out;
	}
	memcpy(out, s->pk, s->pk_len);
	*out_len = s->pk_len;
out:
	SUNLOCK(s);
	return st;
}

/* 取出可用的私钥：普通槽位直接返回内部缓冲；SEED_STORAGE 槽位现场重展开。
 * *tmp 非 NULL 时调用方负责 pqc_secure_free(*tmp, tmp_len)。 */
static hsm_status_t borrow_secret(slot_t *s, const uint8_t **sk_out,
                                  uint8_t **tmp, size_t *tmp_len)
{
	*tmp = NULL;
	*tmp_len = 0;
	/* 私钥在硬件里：软件侧根本没有可借的东西，这不是错误状态。
	 * 回 NULL 让调用方走句柄路径 —— begin_use 其余的活（句柄校验、
	 * 用途检查、状态机）与私钥在哪无关，仍然要做，所以这里不能提前返回失败。
	 *
	 * 少了这一条的症状是"生成成功、公钥拿得到、一解封装就 BAD_STATE"，
	 * 看起来像密钥坏了，其实是借私钥这一步不认识"没有私钥"这种正常情况。 */
	if (s->hw_resident) {
		*sk_out = NULL;
		return HSM_OK;
	}
	if (s->sk) {
		*sk_out = s->sk;
		return HSM_OK;
	}
	if (!s->has_seed) {
		return HSM_ERR_BAD_STATE;
	}
	const pqc_alg_info_t *info = pqc_alg_info(s->meta.alg);
	uint8_t *pk = pqc_secure_alloc(info->pk_len);
	uint8_t *sk = pqc_secure_alloc(info->sk_len);
	if (!pk || !sk) {
		pqc_secure_free(pk, info->pk_len);
		pqc_secure_free(sk, info->sk_len);
		return HSM_ERR_NOMEM;
	}
	pqc_status_t cst = pqc_keypair_from_seed(s->meta.alg, s->seed, s->seed_len, pk, sk);
	pqc_secure_free(pk, info->pk_len);
	if (cst != PQC_OK) {
		pqc_secure_free(sk, info->sk_len);
		return HSM_ERR_CRYPTO;
	}
	*sk_out = sk;
	*tmp = sk;
	*tmp_len = info->sk_len;
	return HSM_OK;
}

/* 所有用密钥的操作共用的外壳。进入时持锁，退出时仍持锁 —— 由调用方 SUNLOCK。 */
static hsm_status_t begin_use(slot_t *s, hsm_handle_t h, uint32_t need_usage,
                              const uint8_t **sk_out, uint8_t **tmp, size_t *tmp_len)
{
	hsm_status_t st = check_handle(s, h);
	if (st != HSM_OK) {
		return st;
	}
	if (!(s->meta.usage & need_usage)) {
		return HSM_ERR_USAGE_DENIED;
	}
	st = fsm_apply(s, SLOT_EV_USE_BEGIN);
	if (st != HSM_OK) {
		return st;
	}
	st = borrow_secret(s, sk_out, tmp, tmp_len);
	if (st != HSM_OK) {
		(void)fsm_apply(s, SLOT_EV_USE_END);
	}
	return st;
}

static hsm_status_t end_use(slot_t *s, uint8_t *tmp, size_t tmp_len, hsm_status_t op_st)
{
	if (tmp) {
		pqc_secure_free(tmp, tmp_len);   /* 重展开的私钥用后即清 */
	}
	(void)fsm_apply(s, SLOT_EV_USE_END);
	if (op_st == HSM_OK) {
		s->meta.use_count++;
		s->meta.last_used_at = now_secs();
	}
	hsm_status_t rs = slot_reseal(s);
	return op_st != HSM_OK ? op_st : rs;
}

hsm_status_t hsm_object_sign(hsm_token_t *tok, hsm_session_t sess, hsm_handle_t h,
                             const uint8_t *msg, size_t msg_len,
                             const uint8_t *ctx, size_t ctx_len,
                             uint8_t *sig, size_t cap, size_t *sig_len)
{
	hsm_slot_id_t id;
	hsm_status_t st = require_role(tok, sess, HSM_ROLE_USER, &id);
	if (st != HSM_OK) {
		return st;
	}
	slot_t *s = slot_at(tok, id);
	if (!s) {
		return HSM_ERR_BAD_ARG;
	}
	const uint8_t *sk = NULL;
	uint8_t *tmp = NULL;
	size_t tmp_len = 0;

	SLOCK(s);
	st = begin_use(s, h, KEY_USAGE_SIGN, &sk, &tmp, &tmp_len);
	if (st != HSM_OK) {
		SUNLOCK(s);
		return st;
	}
	{
		const pqc_alg_info_t *info = pqc_alg_info(s->meta.alg);
		hsm_status_t op;
		if (!sig || !sig_len || cap < info->sig_len) {
			op = HSM_ERR_BAD_ARG;
		} else {
			size_t n = cap;
			/* rnd = NULL → hedged 签名，由后端取 TRNG（FIPS 204 推荐模式） */
			op = (pqc_sign(s->meta.alg, sk, msg, msg_len, ctx, ctx_len, NULL, sig, &n)
			      == PQC_OK) ? HSM_OK : HSM_ERR_CRYPTO;
			if (op == HSM_OK) {
				*sig_len = n;
			}
		}
		st = end_use(s, tmp, tmp_len, op);
	}
	SUNLOCK(s);
	slot_audit(tok, AUDIT_OP_SIGN, HSM_ROLE_USER, id, st, NULL);
	return st;
}

hsm_status_t hsm_object_decaps(hsm_token_t *tok, hsm_session_t sess, hsm_handle_t h,
                               const uint8_t *ct, size_t ct_len,
                               uint8_t *ss, size_t cap, size_t *ss_len)
{
	hsm_slot_id_t id;
	hsm_status_t st = require_role(tok, sess, HSM_ROLE_USER, &id);
	if (st != HSM_OK) {
		return st;
	}
	slot_t *s = slot_at(tok, id);
	if (!s) {
		return HSM_ERR_BAD_ARG;
	}
	const uint8_t *sk = NULL;
	uint8_t *tmp = NULL;
	size_t tmp_len = 0;

	SLOCK(s);
	st = begin_use(s, h, KEY_USAGE_DECAP, &sk, &tmp, &tmp_len);
	if (st != HSM_OK) {
		SUNLOCK(s);
		return st;
	}
	{
		const pqc_alg_info_t *info = pqc_alg_info(s->meta.alg);
		hsm_status_t op;
		if (!ct || !ss || !ss_len || ct_len != info->ct_len || cap < info->ss_len) {
			op = HSM_ERR_BAD_ARG;
		} else if (s->hw_resident) {
			/* 私钥在 PL 的片内金库里：只把句柄和密文交下去。
			 * 上面 begin_use 借到的 sk 在这条路上恒为 NULL —— 借的动作仍然要做，
			 * 因为它同时管着句柄校验、用途检查和状态机，那几件事与私钥在哪无关。 */
			op = (pqc_decaps_hw(s->meta.alg, s->hw_handle, ct, ss) == PQC_OK)
			     ? HSM_OK : HSM_ERR_CRYPTO;
			if (op == HSM_OK) {
				*ss_len = info->ss_len;
			}
		} else {
			op = (pqc_decaps(s->meta.alg, sk, ct, ss) == PQC_OK) ? HSM_OK : HSM_ERR_CRYPTO;
			if (op == HSM_OK) {
				*ss_len = info->ss_len;
			}
		}
		st = end_use(s, tmp, tmp_len, op);
	}
	SUNLOCK(s);
	slot_audit(tok, AUDIT_OP_DECAPS, HSM_ROLE_USER, id, st, NULL);
	return st;
}

hsm_status_t hsm_object_destroy(hsm_token_t *tok, hsm_session_t sess, hsm_handle_t h)
{
	hsm_slot_id_t id;
	hsm_status_t st = require_role(tok, sess, HSM_ROLE_USER, &id);
	if (st != HSM_OK) {
		return st;
	}
	slot_t *s = slot_at(tok, id);
	if (!s) {
		return HSM_ERR_BAD_ARG;
	}
	SLOCK(s);
	st = check_handle(s, h);
	if (st != HSM_OK) {
		goto out;
	}
	st = fsm_apply(s, SLOT_EV_DESTROY);
	if (st != HSM_OK) {
		goto out;
	}
	slot_wipe_key_material(s);
	s->meta.alg    = PQC_ALG_NONE;
	s->meta.usage  = 0;
	s->meta.policy = 0;
	s->meta.generation++;    /* 旧句柄立即失效 */
	st = slot_reseal(s);
out:
	SUNLOCK(s);
	slot_audit(tok, AUDIT_OP_DESTROY, HSM_ROLE_USER, id, st, NULL);
	return st;
}

/* ---- zeroize 与解锁 ----------------------------------------------------- */

/* 调用方须持槽位锁 */
static hsm_status_t do_zeroize(slot_t *s)
{
	hsm_status_t st = fsm_apply(s, SLOT_EV_ZEROIZE);
	if (st != HSM_OK) {
		return st;
	}
	slot_wipe_key_material(s);
	slot_wipe_pins(s);
	uint32_t id  = s->meta.slot_id;
	uint32_t gen = s->meta.generation + 1;
	pqc_secure_zero(&s->meta, sizeof(s->meta));
	s->meta.version    = SLOT_META_VERSION;
	s->meta.slot_id    = id;
	s->meta.generation = gen;
	s->meta.alg        = PQC_ALG_NONE;
	s->meta.state      = SLOT_ST_UNINIT;
	s->pre_lock        = SLOT_ST_EMPTY;
	return slot_reseal(s);
}

hsm_status_t hsm_slot_zeroize(hsm_token_t *tok, hsm_session_t sess, hsm_slot_id_t id)
{
	hsm_slot_id_t sid;
	hsm_status_t st = require_role(tok, sess, HSM_ROLE_SO, &sid);
	if (st != HSM_OK) {
		return st;
	}
	slot_t *s = slot_at(tok, id);
	if (!s) {
		return HSM_ERR_BAD_ARG;
	}
	if (sid != id) {
		return HSM_ERR_NOT_AUTHORIZED;
	}
	SLOCK(s);
	st = do_zeroize(s);
	SUNLOCK(s);
	if (st == HSM_OK) {
		drop_sessions_on_slot(tok, id);   /* 清零后登录态一并失效 */
	}
	slot_audit(tok, AUDIT_OP_ZEROIZE, HSM_ROLE_SO, id, st, "so");
	return st;
}

hsm_status_t hsm_slot_zeroize_forced(hsm_token_t *tok, hsm_slot_id_t id)
{
	slot_t *s = slot_at(tok, id);
	if (!s) {
		return HSM_ERR_BAD_ARG;
	}
	SLOCK(s);
	hsm_status_t st = do_zeroize(s);
	SUNLOCK(s);
	if (st == HSM_OK) {
		drop_sessions_on_slot(tok, id);
	}
	slot_audit(tok, AUDIT_OP_ZEROIZE, HSM_ROLE_PUBLIC, id, st, "forced");
	return st;
}

hsm_status_t hsm_slot_unlock(hsm_token_t *tok, hsm_session_t sess, hsm_slot_id_t id)
{
	hsm_slot_id_t sid;
	hsm_status_t st = require_role(tok, sess, HSM_ROLE_SO, &sid);
	if (st != HSM_OK) {
		return st;
	}
	slot_t *s = slot_at(tok, id);
	if (!s) {
		return HSM_ERR_BAD_ARG;
	}
	if (sid != id) {
		return HSM_ERR_NOT_AUTHORIZED;
	}
	SLOCK(s);
	st = slot_check_integrity(s);
	if (st != HSM_OK) {
		goto out;
	}
	st = fsm_apply(s, SLOT_EV_SO_UNLOCK);
	if (st != HSM_OK) {
		goto out;
	}
	s->meta.user_pin_fails = 0;
	st = slot_reseal(s);
out:
	SUNLOCK(s);
	return st;
}
