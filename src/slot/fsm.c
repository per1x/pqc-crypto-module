/* fsm.c —— 槽位生命周期状态机
 *
 * 刻意写成一张显式的表 + 纯函数：
 *   1. 非法转移是"表里没有"，而不是"某个 if 忘了写"；
 *   2. test_slot_fsm 可以穷举 5 状态 × 9 事件 = 45 种组合逐一断言
 *      。
 */
#include "pqchsm/slot.h"

/* [当前状态][事件] → 目标状态；SLOT_ST_INVALID 表示非法转移 */
static const slot_state_t TRANSITION[SLOT_ST__COUNT][SLOT_EV__COUNT] = {
	/*                 INIT_TOKEN      GENERATE         LOAD             USE_BEGIN        USE_END          DESTROY          ZEROIZE          PIN_LOCKOUT      SO_UNLOCK      */
	/* UNINIT  */ { SLOT_ST_EMPTY,   SLOT_ST_INVALID, SLOT_ST_INVALID, SLOT_ST_INVALID, SLOT_ST_INVALID, SLOT_ST_INVALID, SLOT_ST_UNINIT,  SLOT_ST_INVALID, SLOT_ST_INVALID },
	/* EMPTY   */ { SLOT_ST_INVALID, SLOT_ST_LOADED,  SLOT_ST_LOADED,  SLOT_ST_INVALID, SLOT_ST_INVALID, SLOT_ST_INVALID, SLOT_ST_UNINIT,  SLOT_ST_LOCKED,  SLOT_ST_INVALID },
	/* LOADED  */ { SLOT_ST_INVALID, SLOT_ST_INVALID, SLOT_ST_INVALID, SLOT_ST_IN_USE,  SLOT_ST_INVALID, SLOT_ST_EMPTY,   SLOT_ST_UNINIT,  SLOT_ST_LOCKED,  SLOT_ST_INVALID },
	/* IN_USE  */ { SLOT_ST_INVALID, SLOT_ST_INVALID, SLOT_ST_INVALID, SLOT_ST_INVALID, SLOT_ST_LOADED,  SLOT_ST_INVALID, SLOT_ST_UNINIT,  SLOT_ST_LOCKED,  SLOT_ST_INVALID },
	/* LOCKED  */ { SLOT_ST_INVALID, SLOT_ST_INVALID, SLOT_ST_INVALID, SLOT_ST_INVALID, SLOT_ST_INVALID, SLOT_ST_INVALID, SLOT_ST_UNINIT,  SLOT_ST_INVALID, SLOT_ST_LOADED  },
};

slot_state_t slot_fsm_next(slot_state_t cur, slot_event_t ev)
{
	if (cur < 0 || cur >= SLOT_ST__COUNT) {
		return SLOT_ST_INVALID;
	}
	if (ev < 0 || ev >= SLOT_EV__COUNT) {
		return SLOT_ST_INVALID;
	}
	return TRANSITION[cur][ev];
}

/* SO 解锁后回到哪个状态。
 * 的图写的是"回已装载"，但锁定也可能发生在空槽位上 ——
 * 那时回到"已装载"等于谎报槽位里有密钥。所以恢复锁定前的状态；
 * 仅当锁定前状态不是 EMPTY/LOADED（理论上不会）时兜底到 EMPTY。 */
slot_state_t slot_fsm_unlock_target(slot_state_t pre_lock)
{
	switch (pre_lock) {
	case SLOT_ST_EMPTY:
		return SLOT_ST_EMPTY;
	case SLOT_ST_LOADED:
	case SLOT_ST_IN_USE:   /* 使用中被锁 → 解锁后回到已装载，而不是使用中 */
		return SLOT_ST_LOADED;
	default:
		return SLOT_ST_EMPTY;
	}
}

const char *slot_state_name(slot_state_t s)
{
	switch (s) {
	case SLOT_ST_UNINIT:  return "UNINIT";
	case SLOT_ST_EMPTY:   return "EMPTY";
	case SLOT_ST_LOADED:  return "LOADED";
	case SLOT_ST_IN_USE:  return "IN_USE";
	case SLOT_ST_LOCKED:  return "LOCKED";
	case SLOT_ST_INVALID:
	case SLOT_ST__COUNT:
		break;
	}
	return "INVALID";
}

const char *slot_event_name(slot_event_t e)
{
	switch (e) {
	case SLOT_EV_INIT_TOKEN:  return "INIT_TOKEN";
	case SLOT_EV_GENERATE:    return "GENERATE";
	case SLOT_EV_LOAD:        return "LOAD";
	case SLOT_EV_USE_BEGIN:   return "USE_BEGIN";
	case SLOT_EV_USE_END:     return "USE_END";
	case SLOT_EV_DESTROY:     return "DESTROY";
	case SLOT_EV_ZEROIZE:     return "ZEROIZE";
	case SLOT_EV_PIN_LOCKOUT: return "PIN_LOCKOUT";
	case SLOT_EV_SO_UNLOCK:   return "SO_UNLOCK";
	case SLOT_EV__COUNT:
		break;
	}
	return "?";
}
