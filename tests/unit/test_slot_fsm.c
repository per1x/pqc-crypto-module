/* 槽位生命周期状态机穷举测试（路线图 §7.1 / §5.7.3）
 *
 * 关键：下面这张 EXPECT 表是**照着 §7.1 的图独立写第二遍**的，
 * 不是从 src/slot/fsm.c 抄的。两份独立表述一致，才说明状态机是对的；
 * 如果直接引用实现里的表，这个测试就只是在证明"表等于它自己"。
 */
#include "testlib.h"
#include "pqchsm/slot.h"

#define X SLOT_ST_INVALID

static const slot_state_t EXPECT[SLOT_ST__COUNT][SLOT_EV__COUNT] = {
	/*            INIT_TOKEN     GENERATE       LOAD           USE_BEGIN      USE_END        DESTROY        ZEROIZE        PIN_LOCKOUT    SO_UNLOCK    */
	/*UNINIT*/ { SLOT_ST_EMPTY,  X,             X,             X,             X,             X,             SLOT_ST_UNINIT, X,             X              },
	/*EMPTY */ { X,              SLOT_ST_LOADED, SLOT_ST_LOADED, X,            X,             X,             SLOT_ST_UNINIT, SLOT_ST_LOCKED, X             },
	/*LOADED*/ { X,              X,             X,             SLOT_ST_IN_USE, X,            SLOT_ST_EMPTY, SLOT_ST_UNINIT, SLOT_ST_LOCKED, X             },
	/*IN_USE*/ { X,              X,             X,             X,             SLOT_ST_LOADED, X,            SLOT_ST_UNINIT, SLOT_ST_LOCKED, X             },
	/*LOCKED*/ { X,              X,             X,             X,             X,             X,             SLOT_ST_UNINIT, X,             SLOT_ST_LOADED },
};

int main(void)
{
	/* ---- 5 状态 × 9 事件 = 45 种组合，逐一断言 ---- */
	int legal = 0, illegal = 0;
	for (int s = 0; s < SLOT_ST__COUNT; s++) {
		for (int e = 0; e < SLOT_EV__COUNT; e++) {
			static char name[64];
			snprintf(name, sizeof(name), "%s + %s",
			         slot_state_name((slot_state_t)s), slot_event_name((slot_event_t)e));
			TCASE(name);
			slot_state_t got = slot_fsm_next((slot_state_t)s, (slot_event_t)e);
			CHECK_EQ_INT(got, EXPECT[s][e]);
			if (EXPECT[s][e] == SLOT_ST_INVALID) {
				illegal++;
			} else {
				legal++;
			}
		}
	}
	TCASE("组合计数");
	CHECK_EQ_INT(legal + illegal, SLOT_ST__COUNT * SLOT_EV__COUNT);
	CHECK_EQ_INT(legal + illegal, 45);
	/* 非法转移应当远多于合法转移 —— 若某次改动让这个数掉下来，说明放宽了限制 */
	CHECK_EQ_INT(illegal, 30);

	/* ---- 性质断言（比逐格比对更能防住"表改错了但两边一起改错"）---- */

	TCASE("zeroize 从任意状态可达且目标恒为 UNINIT");
	for (int s = 0; s < SLOT_ST__COUNT; s++) {
		CHECK_EQ_INT(slot_fsm_next((slot_state_t)s, SLOT_EV_ZEROIZE), SLOT_ST_UNINIT);
	}

	TCASE("zeroize 不可逆：UNINIT 出不去，除非重新 init_token");
	for (int e = 0; e < SLOT_EV__COUNT; e++) {
		slot_state_t got = slot_fsm_next(SLOT_ST_UNINIT, (slot_event_t)e);
		if (e != SLOT_EV_INIT_TOKEN && e != SLOT_EV_ZEROIZE) {
			CHECK_EQ_INT(got, SLOT_ST_INVALID);
		}
	}

	TCASE("锁定态只有两条出路：zeroize 或 SO 解锁");
	for (int e = 0; e < SLOT_EV__COUNT; e++) {
		slot_state_t got = slot_fsm_next(SLOT_ST_LOCKED, (slot_event_t)e);
		if (e != SLOT_EV_ZEROIZE && e != SLOT_EV_SO_UNLOCK) {
			CHECK_EQ_INT(got, SLOT_ST_INVALID);
		}
	}

	TCASE("PIN 锁定可从 EMPTY/LOADED/IN_USE 触发，但 UNINIT/LOCKED 不行");
	CHECK_EQ_INT(slot_fsm_next(SLOT_ST_EMPTY,  SLOT_EV_PIN_LOCKOUT), SLOT_ST_LOCKED);
	CHECK_EQ_INT(slot_fsm_next(SLOT_ST_LOADED, SLOT_EV_PIN_LOCKOUT), SLOT_ST_LOCKED);
	CHECK_EQ_INT(slot_fsm_next(SLOT_ST_IN_USE, SLOT_EV_PIN_LOCKOUT), SLOT_ST_LOCKED);
	CHECK_EQ_INT(slot_fsm_next(SLOT_ST_UNINIT, SLOT_EV_PIN_LOCKOUT), SLOT_ST_INVALID);
	CHECK_EQ_INT(slot_fsm_next(SLOT_ST_LOCKED, SLOT_EV_PIN_LOCKOUT), SLOT_ST_INVALID);

	TCASE("解锁回到锁定前的状态，而不是一律回 LOADED");
	/* §7.1 的图写"回已装载"；空槽位被锁后回 LOADED 会谎报槽位有密钥 */
	CHECK_EQ_INT(slot_fsm_unlock_target(SLOT_ST_EMPTY),  SLOT_ST_EMPTY);
	CHECK_EQ_INT(slot_fsm_unlock_target(SLOT_ST_LOADED), SLOT_ST_LOADED);
	CHECK_EQ_INT(slot_fsm_unlock_target(SLOT_ST_IN_USE), SLOT_ST_LOADED);

	TCASE("越界输入必须返回 INVALID 而不是越界读表");
	CHECK_EQ_INT(slot_fsm_next((slot_state_t)-1, SLOT_EV_ZEROIZE), SLOT_ST_INVALID);
	CHECK_EQ_INT(slot_fsm_next(SLOT_ST__COUNT, SLOT_EV_ZEROIZE), SLOT_ST_INVALID);
	CHECK_EQ_INT(slot_fsm_next(SLOT_ST_EMPTY, (slot_event_t)-1), SLOT_ST_INVALID);
	CHECK_EQ_INT(slot_fsm_next(SLOT_ST_EMPTY, SLOT_EV__COUNT), SLOT_ST_INVALID);
	CHECK_EQ_INT(slot_fsm_next(SLOT_ST_INVALID, SLOT_EV_ZEROIZE), SLOT_ST_INVALID);

	TCASE("名字表无空洞");
	for (int s = 0; s < SLOT_ST__COUNT; s++) {
		CHECK(strcmp(slot_state_name((slot_state_t)s), "INVALID") != 0);
	}
	for (int e = 0; e < SLOT_EV__COUNT; e++) {
		CHECK(strcmp(slot_event_name((slot_event_t)e), "?") != 0);
	}

	return test_report("test_slot_fsm");
}
