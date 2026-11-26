#ifndef GUARD_COOP_H
#define GUARD_COOP_H

#include "global.h"

/* Native two-player session. The link cable code is a separate path.
 * Outside the port these are empty, so the GBA ROM does not gain a link. */

#ifdef PORTABLE

struct CoopBattleAction
{
    u8 action;
    u8 data1;
    u8 data2;
    u8 data3;
    u8 switchIndex;
    u8 movePosition;
    u16 moveTarget;
    u32 rngSeed;
};

bool8 Coop_BattleTakeRemoteAction(struct CoopBattleAction *out);
bool8 Coop_BattleSendLocalAction(const struct CoopBattleAction *action);
void Coop_BattleTurnComplete(void);

bool8 Coop_IsStorySyncReady(void);
bool8 Coop_PartnersNearby(void);
bool8 Coop_BlockTrainerApproach(void);
u8 Coop_ChooseTrainerApproach(u8 localDistance, u8 partnerDistance);
void Coop_PrepareSharedTrainerBattle(void);
void Coop_RestoreSharedBattleParty(bool8 fullRestore);
bool8 Coop_PartnerVisible(void);
bool8 Coop_CopyPartnerTile(s16 *x, s16 *y);
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
/* True while this player, or the partner on this map, is inside a script.
 * Wild encounters and trainer approach wait. */
bool8 Coop_BlockFieldEvent(void);
/* The injected partner lead is not one of this save's Pokémon. */
bool8 Coop_OwnsBattleMon(struct Pokemon *mon);
void Coop_DropSavedMon(struct Pokemon *mon);
/* Puts the real party back, keeping EXP on the mons that fought. */
void Coop_FinishSharedBattle(void);

#else

struct CoopBattleAction
{
    u8 action;
    u8 data1;
    u8 data2;
    u8 data3;
    u8 switchIndex;
    u8 movePosition;
    u16 moveTarget;
    u32 rngSeed;
};
static inline bool8 Coop_BattleTakeRemoteAction(struct CoopBattleAction *out) { (void)out; return FALSE; }
static inline bool8 Coop_BattleSendLocalAction(const struct CoopBattleAction *action) { (void)action; return FALSE; }
static inline void Coop_BattleTurnComplete(void) {}

static inline bool8 Coop_IsStorySyncReady(void) { return FALSE; }
static inline bool8 Coop_PartnersNearby(void) { return FALSE; }
static inline bool8 Coop_BlockTrainerApproach(void) { return FALSE; }
static inline u8 Coop_ChooseTrainerApproach(u8 localDistance, u8 partnerDistance)
{
    (void)partnerDistance;
    return localDistance;
}
static inline void Coop_PrepareSharedTrainerBattle(void) {}
static inline void Coop_RestoreSharedBattleParty(bool8 fullRestore) { (void)fullRestore; }
static inline bool8 Coop_PartnerVisible(void) { return FALSE; }
static inline bool8 Coop_CopyPartnerTile(s16 *x, s16 *y) { (void)x; (void)y; return FALSE; }
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
static inline bool8 Coop_BlockFieldEvent(void) { return FALSE; }
static inline bool8 Coop_OwnsBattleMon(struct Pokemon *mon) { (void)mon; return TRUE; }
static inline void Coop_DropSavedMon(struct Pokemon *mon) { (void)mon; }
static inline void Coop_FinishSharedBattle(void) {}

#endif

#endif
