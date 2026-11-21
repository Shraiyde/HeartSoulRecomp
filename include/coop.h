#ifndef GUARD_COOP_H
#define GUARD_COOP_H

#include "global.h"

/* Native two-player session. The link cable code is a separate path.
 * Outside the port these are empty, so the GBA ROM does not gain a link. */

#ifdef PORTABLE

void Coop_Boot(void);
void Coop_Frame(void);
void Coop_OnBattleStarted(void);
bool8 Coop_BattleAbortPending(void);
/* Restores party, money, position and clears a trainer win, then returns
 * to the field. Consumes the abort. */
void Coop_LeaveAbortedBattle(void);
void Coop_OnFlagChanged(u16 flagId, bool8 set);
void Coop_OnVarChanged(u16 varId, u16 value);
bool8 Coop_IsReservedPresenceObjectEventId(u8 objectEventId);
bool8 Coop_IsCoopLocalId(u8 localId);
u8 Coop_TakePreferredObjectEventId(void);

#else

static inline void Coop_Boot(void) {}
static inline void Coop_Frame(void) {}
static inline void Coop_OnBattleStarted(void) {}
static inline bool8 Coop_BattleAbortPending(void) { return FALSE; }
static inline void Coop_LeaveAbortedBattle(void) {}
static inline void Coop_OnFlagChanged(u16 flagId, bool8 set) { (void)flagId; (void)set; }
static inline void Coop_OnVarChanged(u16 varId, u16 value) { (void)varId; (void)value; }
static inline bool8 Coop_IsReservedPresenceObjectEventId(u8 objectEventId) { (void)objectEventId; return FALSE; }
static inline bool8 Coop_IsCoopLocalId(u8 localId) { (void)localId; return FALSE; }
static inline u8 Coop_TakePreferredObjectEventId(void) { return 0; }

#endif

#endif
