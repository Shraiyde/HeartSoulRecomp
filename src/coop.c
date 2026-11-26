#include "global.h"
#include "random.h"

#ifdef PORTABLE

#include "coop.h"
#include "platform/coop_net.h"
#include "battle.h"
#include "battle_setup.h"
#include "metatile_behavior.h"
#include "event_data.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "field_screen_effect.h"
#include "fieldmap.h"
#include "item.h"
#include "load_save.h"
#include "main.h"
#include "money.h"
#include "overworld.h"
#include "pokemon.h"
#include "script.h"
#include "sprite.h"
#include "trainer_see.h"
#include "constants/battle.h"
#include "constants/event_objects.h"
#include "constants/event_object_movement.h"
#include "constants/flags.h"
#include "constants/items.h"
#include "constants/maps.h"
#include "constants/vars.h"

#define COOP_PARTNER_SLOT   (OBJECT_EVENTS_COUNT - 1)
#define COOP_FOLLOWER_SLOT  (OBJECT_EVENTS_COUNT - 2)
#define COOP_LOCAL_PARTNER  (OBJ_EVENT_ID_DYNAMIC_BASE + 1)
#define COOP_LOCAL_FOLLOWER (OBJ_EVENT_ID_DYNAMIC_BASE + 2)
#define COOP_FOLLOWER_VISIBLE (1u << 31)
#define COOP_QUEUE 128
#define COOP_PARTNER_HIDE_MS 6000u
#define COOP_BATTLE_SILENCE_MS 6000u
#define COOP_BACKGROUND_HOLD_MS 90000u

struct CoopWord
{
    u16 type;
    u16 id;
    u16 value;
};

static EWRAM_DATA struct CoopWord sOutQ[COOP_QUEUE];
static EWRAM_DATA u8 sOutR;
static EWRAM_DATA u8 sOutW;
static EWRAM_DATA u8 sOutN;
static EWRAM_DATA bool8 sApplying;
static EWRAM_DATA bool8 sLinkWasUp;
static EWRAM_DATA bool8 sScanDone;
static EWRAM_DATA u8 sScanPhase;
static EWRAM_DATA u16 sScanCursor;
static EWRAM_DATA u8 sQuietFrames;
static EWRAM_DATA u8 sReadyBurst;
static EWRAM_DATA bool8 sRemoteReady;
static EWRAM_DATA u8 sPreferSlot = OBJECT_EVENTS_COUNT;
static EWRAM_DATA u8 sClaimSeq;
static EWRAM_DATA bool8 sInBattle;
static EWRAM_DATA bool8 sBattleHadLink;
static EWRAM_DATA bool8 sAbort;
static EWRAM_DATA bool8 sHadPartner;
static EWRAM_DATA bool8 sAbortSent;
static EWRAM_DATA bool8 sPartySaved;
static EWRAM_DATA bool8 sPartnerLeadValid;
static EWRAM_DATA u16 sPartnerSpecies;
static EWRAM_DATA u8 sPartnerLevel;
static EWRAM_DATA u16 sPartnerItem;
static EWRAM_DATA u16 sPartnerMoves[4];
static EWRAM_DATA struct Pokemon sPartnerParty[MULTI_PARTY_SIZE];
static EWRAM_DATA bool8 sPartnerPartyValid;
static EWRAM_DATA bool8 sNewPeerSession;
static EWRAM_DATA u16 sPartnerHp;
static EWRAM_DATA u16 sBattleEpoch;
static EWRAM_DATA u16 sBattleTurn;
static EWRAM_DATA bool8 sLocalBattleActionSent;
static EWRAM_DATA bool8 sRemoteBattleActionReady;
static EWRAM_DATA struct CoopBattleAction sRemoteBattleAction;
static EWRAM_DATA u16 sRemoteBattleActionEpoch;
static EWRAM_DATA u16 sRemoteBattleActionTurn;
static EWRAM_DATA bool8 sBattleSeedSent;
static EWRAM_DATA bool8 sRemoteBattleSeedReady;
static EWRAM_DATA rng_value_t sRemoteBattleRng;
static EWRAM_DATA rng_value_t sRemoteBattleRng2;
static EWRAM_DATA u16 sRemoteBattleSeedEpoch;
static EWRAM_DATA u16 sRemoteBattleSeedTurn;
static EWRAM_DATA u32 sRemoteBattleChecksum;
static EWRAM_DATA bool8 sBattleSetupApplied;
static EWRAM_DATA bool8 sRemoteBattleSetupReady;
static EWRAM_DATA u16 sRemoteBattleSetupEpoch;
static EWRAM_DATA u16 sRemoteBattleSetupLen;
static EWRAM_DATA u8 sRemoteBattleSetup[1024];
static EWRAM_DATA u16 sPartnerMaxHp;
static EWRAM_DATA bool8 sSessionLive;
static EWRAM_DATA struct Pokemon sPartyBackup[PARTY_SIZE];
static EWRAM_DATA u8 sPartyCountBackup;
static EWRAM_DATA u32 sMoneyBackup;
static EWRAM_DATA s16 sSpotX;
static EWRAM_DATA s16 sSpotY;
static EWRAM_DATA u8 sSpotFacing;
static EWRAM_DATA u8 sSpotElevation;
static EWRAM_DATA u16 sTrainerA;
static EWRAM_DATA u16 sTrainerB;
static EWRAM_DATA bool8 sTrainerABeaten;
static EWRAM_DATA bool8 sTrainerBBeaten;
static EWRAM_DATA u32 sConfigSeq;
static EWRAM_DATA u8 sConfigAckWait;
static EWRAM_DATA bool8 sLockedForPartner;
static EWRAM_DATA bool8 sPartnerBackground;
static EWRAM_DATA bool8 sAnnouncedBackground;

#define COOP_SIO_FIELD  1u
#define COOP_SIO_SCRIPT 2u

static void MaintainScriptLock(void);

static const u16 sSharedVars[] =
{
    VAR_AZALEA_TOWN_STATE,
    VAR_BAOBA_QUEST_STATE,
    VAR_BILLS_GRANDPA,
    VAR_BLACKTHORN_CITY_STATE,
    VAR_BLACKTHORN_GYM_STATE,
    VAR_CHERRYGROVE_CITY_STATE,
    VAR_CIANWOOD_CITY_STATE,
    VAR_CLEFAIRY_DOLL_STATE,
    VAR_DRAGONS_DEN_QUIZ,
    VAR_ECRUTEAK_CITY_STATE,
    VAR_ECRUTEAK_CITY_THEATER,
    VAR_ELECTRODES_FAINTED,
    VAR_ELEVATOR,
    VAR_FAN_CLUB_CLEFAIRY,
    VAR_FAN_CLUB_GUY,
    VAR_GOLDENROD_CITY_STATE,
    VAR_ICE_PATH_STATE,
    VAR_ILEX_FOREST_FARFETCHD,
    VAR_KANTO_ROCKET_STORY_STATE,
    VAR_KENYA,
    VAR_LAKE_OF_RAGE_FISHERMAN,
    VAR_LEAGUE_STATE,
    VAR_MAHOGANY_TOWN_STATE,
    VAR_MT_SILVER_STATE,
    VAR_NEWBARKTOWN_LABSTATE,
    VAR_NEWBARK_TOWN_STATE,
    VAR_NUM_BADGES,
    VAR_OLIVINE_CITY_STATE,
    VAR_ROCKET_HIDEOUT_STATUE_1,
    VAR_ROCKET_HIDEOUT_STATUE_2,
    VAR_ROCKET_HIDEOUT_STATUE_3,
    VAR_ROCKET_HIDEOUT_STATUE_4,
    VAR_ROCKET_HIDEOUT_STATUE_5,
    VAR_ROCKET_PASSWORD,
    VAR_ROUTE27_BAOBA_CALL,
    VAR_ROUTE27_STATE,
    VAR_ROUTE39_BARN,
    VAR_RUINSOFALPH_STATE,
    VAR_SLOWPOKE_TAIL,
    VAR_SPROUT_TOWER,
    VAR_SSAQUA_STATE,
    VAR_TOHJO_FALLS_GIOVANNI_STATE,
    VAR_TRAIN,
    VAR_TRIGGER_ELM_ROCKET_CALL,
    VAR_VIOLET_CITY_KIMONO_GIRL,
    VAR_VIOLET_CITY_STATE,
    VAR_CELADON_CITY_STATE,
    VAR_CERULEAN_CITY_STATE,
    VAR_CINNABAR_ISLAND_STATE,
    VAR_FUCHSIA_CITY_STATE,
    VAR_GETFLY,
    VAR_KANTO_SAFARI_ZONE_PROGRESS,
    VAR_LAVENDER_TOWN_STATE,
    VAR_PALLET_TOWN_STATE,
    VAR_PALLETTOWN_LABSTATE,
    VAR_PEWTER_CITY_STATE,
    VAR_ROUTE22_GIOVANNI_STATE,
    VAR_ROUTE28_SCIENTIST,
    VAR_OAK_MT_SILVER_CALL,
    VAR_SAFFRON_CITY_STATE,
    VAR_SAFARI_ZONE_GATE_STATE,
    VAR_VERMILION_CITY_STATE,
    VAR_VERMILION_CITY_SAMSON,
    VAR_VERMILION_CITY_STEVEN,
    VAR_VIRIDIAN_CITY_STATE,
    VAR_VIRIDIAN_FOREST_PICHU,
    VAR_ALOLA_STATE,
    VAR_ALOLA_TRAVEL,
    VAR_PLATECOUNTER,
    VAR_SINJOH_STORYLINE,
};

static const u16 sExtraFlags[] =
{
    FLAG_ITEM_ICEPATH1_HM_WATERFALL,
    FLAG_HIDE_ICEPATH_B1F_BOULDER1,
    FLAG_HIDE_ICEPATH_B1F_BOULDER2,
    FLAG_HIDE_ICEPATH_B1F_BOULDER3,
    FLAG_HIDE_ICEPATH_B1F_BOULDER4,
    FLAG_HIDE_ICEPATH_B2F_BOULDER1,
    FLAG_HIDE_ICEPATH_B2F_BOULDER2,
    FLAG_HIDE_ICEPATH_B2F_BOULDER3,
    FLAG_HIDE_ICEPATH_B2F_BOULDER4,
    FLAG_HIDE_ROCKETHIDEOUT_ELECTRODE1,
    FLAG_HIDE_ROCKETHIDEOUT_ELECTRODE2,
    FLAG_HIDE_ROCKETHIDEOUT_ELECTRODE3,
    FLAG_SAFARI_ZONE_EAST_EXPANSION,
    FLAG_SAFARI_ZONE_WEST_EXPANSION,
    FLAG_SYS_GAME_CLEAR,
    FLAG_SYS_NATIONAL_DEX,
    FLAG_SYS_POKENAV_GET,
    FLAG_HIDE_SLOWPOKE_WELL_TAILLESS_SLOWPOKES,
    FLAG_HIDE_AZALEA_TOWN_TAILED_SLOWPOKES,
    FLAG_HIDE_ROUTE13_BOAT,
    FLAG_HIDE_FLYOVER,
    FLAG_HIDE_PHANT_OUTSIDE,
    FLAG_HIDE_PHANT_INSIDE,
    FLAG_VISITED_NEW_SINJOH,
    FLAG_VISITED_ALOLA,
    FLAG_HIDE_SINJOHRUINS_STEVEN,
    FLAG_HIDE_SINJOHRUINS_LAB_STEVEN,
    FLAG_HIDE_SINJOHRUINS_ARCEUSROOM_STEVEN,
    FLAG_HIDE_NEWSINJOH_STEVEN,
    FLAG_HIDE_VERMILION_CITY_SAMSON,
    FLAG_HIDE_ROUTE28_SCIENTIST,
    FLAG_HIDE_SAMSONOAK_INTRO,
    FLAG_ROUTE_49_KIMONOGIRL,
    FLAG_TRIGGERED_PONI_TOTEM,
    FLAG_TRIGGERED_ULAULA_TOTEM,
    FLAG_TRIGGERED_AKALA_TOTEM,
    FLAG_TRIGGERED_MELEMELE_TOTEM,
    FLAG_DEFEATED_ULAULA_TOTEM,
    FLAG_DEFEATED_PONI_TOTEM,
    FLAG_DEFEATED_AKALA_TOTEM,
    FLAG_DEFEATED_MELEMELE_TOTEM,
};

static bool8 IsSharedPokemonHide(u16 flagId)
{
    switch (flagId)
    {
    case FLAG_HIDE_AZALEA_TOWN_FARFETCHD:
    case FLAG_HIDE_AZALEA_TOWN_SLOWPOKE:
    case FLAG_HIDE_AZALEA_ARIADOS_LEFT:
    case FLAG_HIDE_AZALEA_ARIADOS_MID:
    case FLAG_HIDE_AZALEA_ARIADOS_RIGHT:
    case FLAG_HIDE_ILEX_FOREST_FARFETCHD_1:
    case FLAG_HIDE_ILEX_FOREST_FARFETCHD_2:
    case FLAG_HIDE_ILEX_FOREST_FARFETCHD_3:
    case FLAG_HIDE_ILEX_FOREST_FARFETCHD_4:
    case FLAG_HIDE_LAKE_OF_RAGE_GYARADOS:
    case FLAG_HIDE_WHIRL_ISLANDS_TENTACRUEL:
    case FLAG_HIDE_SUDOWOODO:
    case FLAG_HIDE_CERULEAN_GYM_POKEMON:
    case FLAG_HIDE_VERMILION_SNORLAX:
    case FLAG_HIDE_BATTLE_FRONTIER_SUDOWOODO:
    case FLAG_HIDE_ROCKETHIDEOUT_ELECTRODE1:
    case FLAG_HIDE_ROCKETHIDEOUT_ELECTRODE2:
    case FLAG_HIDE_ROCKETHIDEOUT_ELECTRODE3:
        return TRUE;
    default:
        return FALSE;
    }
}

static bool8 IsExtendedHide(u16 flagId)
{
    switch (flagId)
    {
    case FLAG_HIDE_SLOWPOKE_WELL_TAILLESS_SLOWPOKES:
    case FLAG_HIDE_AZALEA_TOWN_TAILED_SLOWPOKES:
    case FLAG_HIDE_ROUTE13_BOAT:
    case FLAG_HIDE_FLYOVER:
    case FLAG_HIDE_PHANT_OUTSIDE:
    case FLAG_HIDE_PHANT_INSIDE:
    case FLAG_HIDE_SINJOHRUINS_STEVEN:
    case FLAG_HIDE_SINJOHRUINS_LAB_STEVEN:
    case FLAG_HIDE_SINJOHRUINS_ARCEUSROOM_STEVEN:
    case FLAG_HIDE_NEWSINJOH_STEVEN:
    case FLAG_HIDE_VERMILION_CITY_SAMSON:
    case FLAG_HIDE_ROUTE28_SCIENTIST:
    case FLAG_HIDE_SAMSONOAK_INTRO:
    case FLAG_ROUTE_49_KIMONOGIRL:
        return TRUE;
    default:
        return FALSE;
    }
}

static bool8 IsIceBoulder(u16 flagId)
{
    return flagId >= FLAG_HIDE_ICEPATH_B1F_BOULDER1
        && flagId <= FLAG_HIDE_ICEPATH_B2F_BOULDER4;
}

static bool8 IsBidirectional(u16 flagId)
{
    if (flagId >= HNS_NPC_HIDE_START
        && flagId < HNS_NPC_HIDE_START + HNS_NPC_HIDE_COUNT
        && flagId != FLAG_HIDE_ILEX_FOREST_SECOND_CELEBI)
        return TRUE;
    return IsSharedPokemonHide(flagId) || IsIceBoulder(flagId) || IsExtendedHide(flagId);
}

static bool8 IsSharedFlag(u16 flagId)
{
    if (flagId >= FLAG_ADVENTURE_STARTED && flagId <= FLAG_UNLOCKED_GOLDENROD_UNDERGROUND_DOOR
        && flagId != FLAG_ROUTE25_GOT_NUGGET)
        return TRUE;
    if (flagId >= FLAG_DEFEATED_VIOLET_GYM && flagId <= FLAG_DEFEATED_VIRIDIAN_GYM)
        return TRUE;
    if (flagId >= FLAG_BADGE01_GET && flagId <= FLAG_BADGE16_GET)
        return TRUE;
    if (flagId >= TRAINER_FLAGS_START && flagId <= TRAINER_FLAGS_END)
        return TRUE;
    if (flagId >= HNS_NPC_HIDE_START
        && flagId < HNS_NPC_HIDE_START + HNS_NPC_HIDE_COUNT
        && flagId != FLAG_HIDE_ILEX_FOREST_SECOND_CELEBI)
        return TRUE;
    if (IsSharedPokemonHide(flagId) || IsIceBoulder(flagId))
        return TRUE;
    switch (flagId)
    {
    case FLAG_RECEIVED_BIKE:
    case FLAG_HAS_MATCH_CALL:
    case FLAG_RECEIVED_HM_CUT:
    case FLAG_RECEIVED_HM_FLASH:
    case FLAG_RECEIVED_HM_ROCK_SMASH:
    case FLAG_RECEIVED_HM_STRENGTH:
    case FLAG_RECEIVED_HM_SURF:
    case FLAG_RECEIVED_POKENAV:
    case FLAG_RECEIVED_RUNNING_SHOES:
    case FLAG_RECEIVED_SQUIRTBOTTLE:
    case FLAG_GOT_SUPER_ROD:
    case FLAG_GET_HEADBUTT:
    case FLAG_ITEM_ICEPATH1_HM_WATERFALL:
    case FLAG_ADDED_MATCH_CALL_TO_POKENAV:
    case FLAG_ENABLE_BAOBA_MATCH_CALL:
    case FLAG_REGISTER_RIVAL_POKENAV:
    case FLAG_SAFARI_ZONE_EAST_EXPANSION:
    case FLAG_SAFARI_ZONE_WEST_EXPANSION:
    case FLAG_SYS_GAME_CLEAR:
    case FLAG_SYS_NATIONAL_DEX:
    case FLAG_SYS_POKENAV_GET:
    case FLAG_VISITED_NEW_SINJOH:
    case FLAG_VISITED_ALOLA:
    case FLAG_TRIGGERED_PONI_TOTEM:
    case FLAG_TRIGGERED_ULAULA_TOTEM:
    case FLAG_TRIGGERED_AKALA_TOTEM:
    case FLAG_TRIGGERED_MELEMELE_TOTEM:
    case FLAG_DEFEATED_ULAULA_TOTEM:
    case FLAG_DEFEATED_PONI_TOTEM:
    case FLAG_DEFEATED_AKALA_TOTEM:
    case FLAG_DEFEATED_MELEMELE_TOTEM:
    case FLAG_GOT_PASSWORD_FROM_ETO:
    case FLAG_GOT_PASSWORD_FROM_GRUNTF:
    case FLAG_DEFEATED_SUDOWOODO:
    case FLAG_DEFEATED_RED:
        return TRUE;
    default:
        break;
    }
    if (IsExtendedHide(flagId))
        return TRUE;
    if (flagId >= TRAINER_REGISTERED_FLAGS_START && flagId <= FLAG_REGISTERED_REENA_HNS)
        return TRUE;
    return FALSE;
}

static bool8 IsSharedVar(u16 varId)
{
    u32 i;
    for (i = 0; i < ARRAY_COUNT(sSharedVars); i++)
    {
        if (sSharedVars[i] == varId)
            return TRUE;
    }
    return FALSE;
}

static bool8 Enqueue(u16 type, u16 id, u16 value)
{
    if (sOutN >= COOP_QUEUE)
        return FALSE;
    sOutQ[sOutW].type = type;
    sOutQ[sOutW].id = id;
    sOutQ[sOutW].value = value;
    sOutW = (sOutW + 1) % COOP_QUEUE;
    sOutN++;
    return TRUE;
}

static void QueueFlag(u16 flagId, bool8 set)
{
    if (sApplying || !IsSharedFlag(flagId) || !sSessionLive)
        return;
    Enqueue(COOP_MSG_WORLD_FLAG, flagId, set ? 1 : 0);
}

static void QueueVar(u16 varId, u16 value)
{
    if (sApplying || !value || !IsSharedVar(varId) || !sSessionLive)
        return;
    if (value <= VarGet(varId))
        return;
    Enqueue(COOP_MSG_WORLD_VAR, varId, value);
}

void Coop_OnFlagChanged(u16 flagId, bool8 set)
{
    QueueFlag(flagId, set);
}

void Coop_OnVarChanged(u16 varId, u16 value)
{
    QueueVar(varId, value);
}

static void GrantProgressItem(u16 flagId)
{
    u16 itemId = ITEM_NONE;

    switch (flagId)
    {
    case FLAG_RECEIVED_BIKE: itemId = ITEM_BICYCLE; break;
    case FLAG_RECEIVED_SQUIRTBOTTLE: itemId = ITEM_SQUIRT_BOTTLE; break;
    case FLAG_RECEIVED_HM_CUT: itemId = ITEM_HM_CUT; break;
    case FLAG_RECEIVED_HM_FLASH: itemId = ITEM_HM_FLASH; break;
    case FLAG_RECEIVED_HM_ROCK_SMASH: itemId = ITEM_HM_ROCK_SMASH; break;
    case FLAG_RECEIVED_HM_STRENGTH: itemId = ITEM_HM_STRENGTH; break;
    case FLAG_RECEIVED_HM_SURF: itemId = ITEM_HM_SURF; break;
    case FLAG_ITEM_ICEPATH1_HM_WATERFALL: itemId = ITEM_HM_WATERFALL; break;
    case FLAG_GOT_SUPER_ROD: itemId = ITEM_SUPER_ROD; break;
    default:
        return;
    }
    if (!CheckBagHasItem(itemId, 1))
        AddBagItem(itemId, 1);
}

static void ApplyFlag(u16 flagId, bool8 set)
{
    if (!IsSharedFlag(flagId))
        return;
    if (!set && !IsBidirectional(flagId))
        return;
    sApplying = TRUE;
    if (set)
    {
        if (!FlagGet(flagId))
            FlagSet(flagId);
        GrantProgressItem(flagId);
    }
    else if (FlagGet(flagId))
    {
        FlagClear(flagId);
    }
    sApplying = FALSE;
}

static void ApplyVar(u16 varId, u16 value)
{
    if (!IsSharedVar(varId) || value <= VarGet(varId))
        return;
    sApplying = TRUE;
    VarSet(varId, value);
    sApplying = FALSE;
}

static void ResetScan(void)
{
    sScanPhase = 0;
    sScanCursor = HNS_HIDE_POKEMON_START;
    sScanDone = FALSE;
    sQuietFrames = 0;
    sReadyBurst = 0;
    sRemoteReady = FALSE;
    sOutR = sOutW = sOutN = 0;
}

static bool8 NextScanFlag(u16 *flagId)
{
    switch (sScanPhase)
    {
    case 0:
        if (sScanCursor <= FLAG_DEFEATED_RED)
        {
            *flagId = sScanCursor++;
            return TRUE;
        }
        sScanPhase = 1;
        sScanCursor = FLAG_BADGE01_GET;
        return FALSE;
    case 1:
        if (sScanCursor <= FLAG_BADGE16_GET)
        {
            *flagId = sScanCursor++;
            return TRUE;
        }
        sScanPhase = 2;
        sScanCursor = TRAINER_FLAGS_START;
        return FALSE;
    case 2:
        if (sScanCursor <= TRAINER_FLAGS_END)
        {
            *flagId = sScanCursor++;
            return TRUE;
        }
        sScanPhase = 3;
        sScanCursor = TRAINER_REGISTERED_FLAGS_START;
        return FALSE;
    case 3:
        if (sScanCursor <= FLAG_REGISTERED_REENA_HNS)
        {
            *flagId = sScanCursor++;
            return TRUE;
        }
        sScanPhase = 4;
        sScanCursor = 0;
        return FALSE;
    case 4:
        if (sScanCursor < ARRAY_COUNT(sExtraFlags))
        {
            *flagId = sExtraFlags[sScanCursor++];
            return TRUE;
        }
        sScanPhase = 5;
        sScanCursor = 0;
        return FALSE;
    default:
        return FALSE;
    }
}

static void ScanStory(void)
{
    u8 steps = 0;

    if (sScanDone || !sSessionLive)
        return;
    while (steps < 24 && sOutN + 4 < COOP_QUEUE && sScanPhase < 5)
    {
        u16 flagId;
        if (!NextScanFlag(&flagId))
            continue;
        steps++;
        if (!IsSharedFlag(flagId))
            continue;
        if (FlagGet(flagId))
            Enqueue(COOP_MSG_WORLD_FLAG, flagId, 1);
        else if (IsBidirectional(flagId))
            Enqueue(COOP_MSG_WORLD_FLAG, flagId, 0);
    }
    while (steps < 32 && sOutN + 2 < COOP_QUEUE && sScanPhase == 5
        && sScanCursor < ARRAY_COUNT(sSharedVars))
    {
        u16 varId = sSharedVars[sScanCursor++];
        u16 value = VarGet(varId);
        steps++;
        if (value != 0)
            Enqueue(COOP_MSG_WORLD_VAR, varId, value);
    }
    if (sScanPhase == 5 && sScanCursor >= ARRAY_COUNT(sSharedVars))
        sScanDone = TRUE;
}

static void FlushOut(void)
{
    u8 sent = 0;
    while (sOutN > 0 && sent < 6)
    {
        struct CoopWord word = sOutQ[sOutR];
        u32 packed = word.id | ((u32)word.value << 16);
        if (!CoopNet_SendWord(word.type, packed))
            break;
        sOutR = (sOutR + 1) % COOP_QUEUE;
        sOutN--;
        sent++;
    }
}

static void AdvertiseReady(void)
{
    if (!sScanDone || sOutN != 0)
    {
        sQuietFrames = 0;
        return;
    }
    if (sQuietFrames < 30)
    {
        sQuietFrames++;
        return;
    }
    if (sReadyBurst < 8 || (sReadyBurst % 60) == 0)
        CoopNet_SendWord(COOP_MSG_WORLD_READY, COOP_READY_STORY);
    if (sReadyBurst < 255)
        sReadyBurst++;
}

static u32 ReadBe32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static u16 ReadBe16(const u8 *p)
{
    return (u16)((p[0] << 8) | p[1]);
}

static void HandleInbound(void)
{
    u16 type;
    u8 payload[512];
    u32 len;
    u8 guard;

    for (guard = 0; guard < 8; guard++)
    {
        if (!CoopNet_Poll(&type, payload, &len, sizeof(payload)))
            break;
        if (type == COOP_MSG_WORLD_FLAG && len == 4)
        {
            u32 packed;
            memcpy(&packed, payload, 4);
            ApplyFlag((u16)packed, (packed >> 16) != 0);
        }
        else if (type == COOP_MSG_WORLD_VAR && len == 4)
        {
            u32 packed;
            memcpy(&packed, payload, 4);
            ApplyVar((u16)packed, (u16)(packed >> 16));
        }
        else if (type == COOP_MSG_WORLD_READY && len == 4)
        {
            u32 value;
            memcpy(&value, payload, 4);
            if (value == COOP_READY_STORY)
                sRemoteReady = TRUE;
            else if (value == COOP_READY_BATTLE_ABORT && sInBattle)
            {
                sAbort = TRUE;
                if (gBattleOutcome == 0)
                    gBattleOutcome = B_OUTCOME_PLAYER_TELEPORTED;
            }
            else if (value == COOP_READY_BACKGROUND)
                sPartnerBackground = TRUE;
            else if (value == COOP_READY_FOREGROUND)
                sPartnerBackground = FALSE;
        }
        else if (type == COOP_MSG_WORLD_CONFIG && len >= 20)
        {
            sPartnerSpecies = (u16)(payload[0] | (payload[1] << 8));
            sPartnerLevel = payload[2];
            sPartnerItem = (u16)(payload[4] | (payload[5] << 8));
            sPartnerMoves[0] = (u16)(payload[6] | (payload[7] << 8));
            sPartnerMoves[1] = (u16)(payload[8] | (payload[9] << 8));
            sPartnerMoves[2] = (u16)(payload[10] | (payload[11] << 8));
            sPartnerMoves[3] = (u16)(payload[12] | (payload[13] << 8));
            sPartnerHp = (u16)(payload[14] | (payload[15] << 8));
            sPartnerMaxHp = (u16)(payload[16] | (payload[17] << 8));
            sPartnerLeadValid = sPartnerSpecies != SPECIES_NONE && sPartnerLevel != 0;
        }
        else if (type == COOP_MSG_SESSION_RESET && len == 0)
        {
            sRemoteReady = FALSE;
            sPartnerLeadValid = FALSE;
            sPartnerPartyValid = FALSE;
            sReadyBurst = 0;
            sConfigAckWait = 0;
            ResetScan();
            sNewPeerSession = TRUE;
            if (sInBattle)
            {
                sAbort = TRUE;
                if (gBattleOutcome == 0)
                    gBattleOutcome = B_OUTCOME_PLAYER_TELEPORTED;
            }
            else
            {
                sBattleEpoch = 0;
                sBattleTurn = 0;
                sLocalBattleActionSent = FALSE;
                sRemoteBattleActionReady = FALSE;
                sBattleSeedSent = FALSE;
                sRemoteBattleSeedReady = FALSE;
                sBattleSetupApplied = FALSE;
                sRemoteBattleSetupReady = FALSE;
                sNewPeerSession = FALSE;
            }
        }
        else if (type == COOP_MSG_BATTLE_SETUP && len <= sizeof(sRemoteBattleSetup) && len >= 12)
        {
            u16 epoch = ReadBe16(payload);
            if ((sInBattle && epoch == sBattleEpoch)
                || (!sInBattle && epoch == (u16)(sBattleEpoch + 1)))
            {
                memcpy(sRemoteBattleSetup, payload, len);
                sRemoteBattleSetupLen = (u16)len;
                sRemoteBattleSetupEpoch = epoch;
                sRemoteBattleSetupReady = TRUE;
            }
        }
        else if (type == COOP_MSG_BATTLE_SEED && len == 40)
        {
            u16 epoch = ReadBe16(payload);
            u16 turn = ReadBe16(payload + 2);
            if ((sInBattle && epoch == sBattleEpoch && (turn == sBattleTurn || turn == (u16)(sBattleTurn + 1)))
                || (!sInBattle && epoch == (u16)(sBattleEpoch + 1) && turn == 0))
            {
                sRemoteBattleSeedEpoch = epoch;
                sRemoteBattleSeedTurn = turn;
                sRemoteBattleRng.a = ReadBe32(payload + 4);
                sRemoteBattleRng.b = ReadBe32(payload + 8);
                sRemoteBattleRng.c = ReadBe32(payload + 12);
                sRemoteBattleRng.ctr = ReadBe32(payload + 16);
                sRemoteBattleRng2.a = ReadBe32(payload + 20);
                sRemoteBattleRng2.b = ReadBe32(payload + 24);
                sRemoteBattleRng2.c = ReadBe32(payload + 28);
                sRemoteBattleRng2.ctr = ReadBe32(payload + 32);
                sRemoteBattleChecksum = ReadBe32(payload + 36);
                sRemoteBattleSeedReady = TRUE;
            }
        }
        else if (type == COOP_MSG_BATTLE_ACTION && len == 12)
        {
            u16 epoch = ReadBe16(payload);
            u16 turn = ReadBe16(payload + 2);
            if ((sInBattle && epoch == sBattleEpoch && (turn == sBattleTurn || turn == (u16)(sBattleTurn + 1)))
                || (!sInBattle && epoch == (u16)(sBattleEpoch + 1) && turn == 0))
            {
                sRemoteBattleActionEpoch = epoch;
                sRemoteBattleActionTurn = turn;
                sRemoteBattleAction.action = payload[4];
                sRemoteBattleAction.data1 = payload[5];
                sRemoteBattleAction.data2 = payload[6];
                sRemoteBattleAction.data3 = payload[7];
                sRemoteBattleAction.switchIndex = payload[8];
                sRemoteBattleAction.movePosition = payload[9];
                sRemoteBattleAction.moveTarget = ReadBe16(payload + 10);
                sRemoteBattleActionReady = TRUE;
            }
        }
        else if (type == COOP_MSG_WORLD_PARTY_BLOB && len == sizeof(sPartnerParty))
        {
            memcpy(sPartnerParty, payload, sizeof(sPartnerParty));
            sPartnerPartyValid = TRUE;
        }
        else if (type == COOP_MSG_WORLD_RESYNC || type == COOP_MSG_POSITION_RESYNC)
        {
            ResetScan();
            sClaimSeq++;
        }
        else if (type == COOP_MSG_WORLD_CONFIG_BLOB && CoopNet_Role() != 0 && len >= 8)
        {
            u32 seq;
            u32 i;
            u32 words = len / 4;
            if (words > 12)
                words = 12;
            seq = ReadBe32(payload);
            /* First word is the sequence; the rest is the rules prefix,
             * each word big-endian on the wire. */
            if (!FlagGet(FLAG_ADVENTURE_STARTED) && words > 1 && gSaveBlock3Ptr != NULL)
            {
                u8 raw[44];
                u32 n = words - 1;
                if (n > 11)
                    n = 11;
                memset(raw, 0, sizeof(raw));
                for (i = 0; i < n; i++)
                {
                    u32 word = ReadBe32(payload + 4 + i * 4);
                    memcpy(raw + i * 4, &word, 4);
                }
                if (sizeof(gSaveBlock3Ptr->challengeSettings) >= n * 4)
                    memcpy(&gSaveBlock3Ptr->challengeSettings, raw, n * 4);
            }
            CoopNet_SendWord(COOP_MSG_WORLD_CONFIG_ACK, seq);
        }
    }
}

static void PublishConfig(void)
{
    u8 raw[44];
    u8 wire[48];
    u32 i;
    u32 n = sizeof(gSaveBlock3Ptr->challengeSettings);
    static u8 sLast[44];
    static bool8 sHaveLast;

    if (CoopNet_Role() != 0 || gSaveBlock3Ptr == NULL)
        return;
    if (n > sizeof(raw))
        n = sizeof(raw);
    memcpy(raw, &gSaveBlock3Ptr->challengeSettings, n);
    if (n < sizeof(raw))
        memset(raw + n, 0, sizeof(raw) - n);
    if (!sHaveLast || memcmp(sLast, raw, sizeof(raw)) != 0)
    {
        memcpy(sLast, raw, sizeof(raw));
        sHaveLast = TRUE;
        sConfigSeq++;
        if (sConfigSeq == 0)
            sConfigSeq = 1;
        sConfigAckWait = 0;
    }
    if ((sConfigAckWait % 90) != 0)
    {
        sConfigAckWait++;
        return;
    }
    sConfigAckWait++;
    memcpy(wire, &sConfigSeq, 4);
    for (i = 0; i < 11; i++)
    {
        u32 word;
        memcpy(&word, raw + i * 4, 4);
        wire[4 + i * 4 + 0] = (u8)(word >> 24);
        wire[4 + i * 4 + 1] = (u8)(word >> 16);
        wire[4 + i * 4 + 2] = (u8)(word >> 8);
        wire[4 + i * 4 + 3] = (u8)word;
    }
    /* Sequence word is host-order in the first slot; send it big-endian too. */
    wire[0] = (u8)(sConfigSeq >> 24);
    wire[1] = (u8)(sConfigSeq >> 16);
    wire[2] = (u8)(sConfigSeq >> 8);
    wire[3] = (u8)sConfigSeq;
    CoopNet_SendBytes(COOP_MSG_WORLD_CONFIG_BLOB, wire, sizeof(wire));
}

static u32 PackFollower(void)
{
    struct ObjectEvent *follower;
    if (!IsFollowerVisible())
        return 0;
    follower = GetFollowerObject();
    if (follower == NULL || follower->invisible)
        return 0;
    if (!(follower->graphicsId & OBJ_EVENT_MON))
        return 0;
    return COOP_FOLLOWER_VISIBLE | (follower->graphicsId & 0xFFFF);
}

static u8 AvatarClass(void)
{
    if (TestPlayerAvatarFlags(PLAYER_AVATAR_FLAG_MACH_BIKE))
        return PLAYER_AVATAR_STATE_MACH_BIKE;
    if (TestPlayerAvatarFlags(PLAYER_AVATAR_FLAG_ACRO_BIKE))
        return PLAYER_AVATAR_STATE_ACRO_BIKE;
    if (TestPlayerAvatarFlags(PLAYER_AVATAR_FLAG_SURFING))
        return PLAYER_AVATAR_STATE_SURFING;
    if (TestPlayerAvatarFlags(PLAYER_AVATAR_FLAG_UNDERWATER))
        return PLAYER_AVATAR_STATE_UNDERWATER;
    return PLAYER_AVATAR_STATE_NORMAL;
}

static u8 DoorPhase(struct ObjectEvent *obj, u8 facing)
{
    u8 behavior;
    if (obj == NULL || !obj->active)
        return 0;
    behavior = MapGridGetMetatileBehaviorAt(obj->currentCoords.x, obj->currentCoords.y);
    if (MetatileBehavior_IsDoor(behavior) || MetatileBehavior_IsNonAnimDoor(behavior))
    {
        if (facing == DIR_NORTH)
            return 1;
        if (facing == DIR_SOUTH)
            return 2;
    }
    return 0;
}

static void PublishPosition(void)
{
    struct ObjectEvent *obj;
    u8 facing;
    u32 avatar;
    u32 words[COOP_POSITION_WORDS];
    u32 lower;

    if (gPlayerAvatar.objectEventId >= OBJECT_EVENTS_COUNT)
        return;
    obj = &gObjectEvents[gPlayerAvatar.objectEventId];
    if (!obj->active)
        return;
    facing = GetPlayerFacingDirection();
    lower = (u32)obj->movementActionId
        | ((u32)AvatarClass() << 8)
        | ((u32)DoorPhase(obj, facing) << 16);
    avatar = lower | ((u32)sClaimSeq << 24);
    words[0] = (u32)gSaveBlock1Ptr->location.mapGroup
        | ((u32)gSaveBlock1Ptr->location.mapNum << 16);
    words[1] = 0;
    if (!sInBattle)
        words[1] |= COOP_SIO_FIELD;
    /* Bit 1 tells the partner this phone is inside a script or cutscene.
     * A lock we applied because THEY are in a script must not echo back. */
    if (!sInBattle && gMain.callback2 == CB2_Overworld
        && ScriptContext_IsEnabled() && !sLockedForPartner)
        words[1] |= COOP_SIO_SCRIPT;
    words[2] = (u32)(u16)obj->currentCoords.x;
    words[3] = (u32)(u16)obj->currentCoords.y;
    words[4] = gSaveBlock2Ptr->playerGender;
    words[5] = facing;
    words[6] = PackFollower();
    words[7] = avatar;
    {
        static u32 sLastLower;
        static u32 sLastPos[7];
        bool8 changed = FALSE;
        u32 i;
        if (lower != sLastLower)
            changed = TRUE;
        for (i = 0; i < 7; i++)
        {
            if (words[i] != sLastPos[i])
                changed = TRUE;
        }
        if (changed)
        {
            sClaimSeq++;
            words[7] = lower | ((u32)sClaimSeq << 24);
            sLastLower = lower;
            for (i = 0; i < 7; i++)
                sLastPos[i] = words[i];
        }
    }
    CoopNet_SetPosition(words);
}

static void ClearSlot(u8 slot)
{
    struct ObjectEvent *obj;
    if (slot >= OBJECT_EVENTS_COUNT)
        return;
    if (slot == gPlayerAvatar.objectEventId)
        return;
    obj = &gObjectEvents[slot];
    if (obj->active && obj->spriteId < MAX_SPRITES && gSprites[obj->spriteId].inUse)
        DestroySprite(&gSprites[obj->spriteId]);
    memset(obj, 0, sizeof(*obj));
}

static u8 SpawnAt(u8 slot, u8 localId, u16 graphicsId, s16 x, s16 y, u8 elevation)
{
    u8 id;
    if (graphicsId == 0)
        return OBJECT_EVENTS_COUNT;
    ClearSlot(slot);
    sPreferSlot = slot;
    id = SpawnSpecialObjectEventParameterized(graphicsId, MOVEMENT_TYPE_FACE_DOWN, localId, x, y, elevation);
    if (id != slot)
    {
        /* The prefer slot was not taken. Do not leave a second copy. */
        if (id < OBJECT_EVENTS_COUNT)
            RemoveObjectEvent(&gObjectEvents[id]);
        return OBJECT_EVENTS_COUNT;
    }
    return id;
}

static void TileBehind(s16 x, s16 y, u8 facing, s16 *outX, s16 *outY)
{
    *outX = x;
    *outY = y;
    if (facing == DIR_NORTH)
        (*outY)++;
    else if (facing == DIR_SOUTH)
        (*outY)--;
    else if (facing == DIR_WEST)
        (*outX)++;
    else if (facing == DIR_EAST)
        (*outX)--;
}

static void StepToward(struct ObjectEvent *obj, s16 x, s16 y, u8 avatarClass)
{
    s16 dx = x - obj->currentCoords.x;
    s16 dy = y - obj->currentCoords.y;
    s16 adx = dx < 0 ? -dx : dx;
    s16 ady = dy < 0 ? -dy : dy;
    u8 dir;
    u8 action;

    if (adx + ady == 0)
        return;
    if (adx + ady > 6)
    {
        MoveObjectEventToMapCoords(obj, x, y);
        return;
    }
    if (ObjectEventIsMovementOverridden(obj) && !ObjectEventClearHeldMovementIfFinished(obj))
        return;
    ObjectEventClearHeldMovementIfActive(obj);
    if (adx >= ady && dx != 0)
        dir = dx > 0 ? DIR_EAST : DIR_WEST;
    else
        dir = dy > 0 ? DIR_SOUTH : DIR_NORTH;
    if (avatarClass == PLAYER_AVATAR_STATE_MACH_BIKE || avatarClass == PLAYER_AVATAR_STATE_ACRO_BIKE)
        action = GetWalkFastMovementAction(dir);
    else if (adx + ady > 2)
        action = GetPlayerRunMovementAction(dir);
    else
        action = GetWalkNormalMovementAction(dir);
    if (!ObjectEventSetHeldMovement(obj, action))
        MoveObjectEventToMapCoords(obj, x, y);
}

static void UpdatePartner(const struct CoopRemoteView *remote)
{
    struct ObjectEvent *player;
    struct ObjectEvent *obj;
    u16 graphicsId;
    u8 avatarClass;
    u8 door;
    u8 elevation;
    bool8 sameMap;
    u32 localMap;

    ClearSlot(COOP_PARTNER_SLOT);
    ClearSlot(COOP_FOLLOWER_SLOT);
    if (!remote->valid || remote->ageMs > (sPartnerBackground ? COOP_BACKGROUND_HOLD_MS : COOP_PARTNER_HIDE_MS))
        return;
    if (gPlayerAvatar.objectEventId >= OBJECT_EVENTS_COUNT)
        return;
    player = &gObjectEvents[gPlayerAvatar.objectEventId];
    if (!player->active || gSaveBlock1Ptr == NULL)
        return;
    localMap = (u32)gSaveBlock1Ptr->location.mapGroup
        | ((u32)gSaveBlock1Ptr->location.mapNum << 16);
    sameMap = remote->mapId == localMap;
    door = (u8)((remote->avatar >> 16) & 0xFF);
    if (!sameMap || door == 1 || remote->x == 0)
    {
        /* Hidden while they are on another map or walking through a door.
         * The next frame that places them on this map spawns them again. */
        return;
    }
    avatarClass = (u8)((remote->avatar >> 8) & 0xFF);
    graphicsId = GetPlayerAvatarGraphicsIdByStateIdAndGender(avatarClass, (u8)remote->gender);
    elevation = player->currentElevation;
    if (SpawnAt(COOP_PARTNER_SLOT, COOP_LOCAL_PARTNER, graphicsId, (s16)remote->x, (s16)remote->y, elevation)
        == OBJECT_EVENTS_COUNT)
        return;
    obj = &gObjectEvents[COOP_PARTNER_SLOT];
    SetObjectEventDirection(obj, (u8)remote->facing);
    StepToward(obj, (s16)remote->x, (s16)remote->y, avatarClass);
    if (remote->follower & COOP_FOLLOWER_VISIBLE)
    {
        u16 followGfx = (u16)(remote->follower & 0xFFFF);
        s16 fx = obj->currentCoords.x;
        s16 fy = obj->currentCoords.y;
        if (remote->facing == DIR_NORTH)
            fy++;
        else if (remote->facing == DIR_SOUTH)
            fy--;
        else if (remote->facing == DIR_WEST)
            fx++;
        else if (remote->facing == DIR_EAST)
            fx--;
        if (SpawnAt(COOP_FOLLOWER_SLOT, COOP_LOCAL_FOLLOWER, followGfx, fx, fy, elevation) < OBJECT_EVENTS_COUNT)
            SetObjectEventDirection(&gObjectEvents[COOP_FOLLOWER_SLOT], (u8)remote->facing);
    }
}

/* The partner is rebuilt every overworld frame into the reserved slots so a
 * battle, menu, door, map edge or cutscene cannot leave them gone. */
static void MaintainPartner(void)
{
    struct CoopRemoteView remote;
    bool8 onField = (gMain.callback2 == CB2_Overworld);

    if (!onField)
        return;
    CoopNet_CopyRemote(&remote);
    /* ClearSlot inside UpdatePartner wipes a sprite we just asked to walk.
     * Only rebuild when the slot is missing, the graphics changed, or the
     * partner is on a new map. While they are present, just step them. */
    if (!remote.valid || remote.ageMs > (sPartnerBackground ? COOP_BACKGROUND_HOLD_MS : COOP_PARTNER_HIDE_MS))
    {
        ClearSlot(COOP_PARTNER_SLOT);
        ClearSlot(COOP_FOLLOWER_SLOT);
        return;
    }
    {
        struct ObjectEvent *obj = &gObjectEvents[COOP_PARTNER_SLOT];
        u32 localMap = 0;
        u8 door = (u8)((remote.avatar >> 16) & 0xFF);
        u8 avatarClass = (u8)((remote.avatar >> 8) & 0xFF);
        u16 graphicsId;
        bool8 sameMap;

        if (gSaveBlock1Ptr != NULL)
            localMap = (u32)gSaveBlock1Ptr->location.mapGroup
                | ((u32)gSaveBlock1Ptr->location.mapNum << 16);
        sameMap = remote.mapId == localMap && remote.x != 0 && door != 1;
        graphicsId = GetPlayerAvatarGraphicsIdByStateIdAndGender(avatarClass, (u8)remote.gender);
        if (!sameMap)
        {
            ClearSlot(COOP_PARTNER_SLOT);
            ClearSlot(COOP_FOLLOWER_SLOT);
            return;
        }
        if (!obj->active || obj->localId != COOP_LOCAL_PARTNER || obj->graphicsId != graphicsId
            || obj->spriteId >= MAX_SPRITES || !gSprites[obj->spriteId].inUse)
        {
            UpdatePartner(&remote);
            return;
        }
        /* Hold still through a cutscene so the sprite does not walk through it. */
        if (Coop_BlockFieldEvent())
            return;
        StepToward(obj, (s16)remote.x, (s16)remote.y, avatarClass);
        if ((u8)remote.facing != 0 && obj->currentCoords.x == (s16)remote.x && obj->currentCoords.y == (s16)remote.y)
            SetObjectEventDirection(obj, (u8)remote.facing);
        if (remote.follower & COOP_FOLLOWER_VISIBLE)
        {
            struct ObjectEvent *follow = &gObjectEvents[COOP_FOLLOWER_SLOT];
            u16 followGfx = (u16)(remote.follower & 0xFFFF);
            if (!follow->active || follow->localId != COOP_LOCAL_FOLLOWER || follow->graphicsId != followGfx)
                UpdatePartner(&remote);
            else
            {
                s16 fx, fy;
                TileBehind(obj->currentCoords.x, obj->currentCoords.y, obj->facingDirection, &fx, &fy);
                StepToward(follow, fx, fy, PLAYER_AVATAR_STATE_NORMAL);
            }
        }
        else
        {
            ClearSlot(COOP_FOLLOWER_SLOT);
        }
    }
}

static void WatchBattle(void)
{
    u32 age;

    if (!sInBattle || !sBattleHadLink)
        return;
    if (!CoopNet_IsUp())
        sAbort = TRUE;
    age = CoopNet_PartnerAgeMs();
    if (sHadPartner && age > (sPartnerBackground ? COOP_BACKGROUND_HOLD_MS : COOP_BATTLE_SILENCE_MS))
        sAbort = TRUE;
    if (!sAbort)
        return;
    /* Latched for this battle. A later resume must not continue it. */
    if (gBattleOutcome == 0)
        gBattleOutcome = B_OUTCOME_PLAYER_TELEPORTED;
    if (!sAbortSent && CoopNet_IsUp())
    {
        if (CoopNet_SendWord(COOP_MSG_WORLD_READY, COOP_READY_BATTLE_ABORT))
            sAbortSent = TRUE;
    }
}

static void PublishLead(void)
{
    struct Pokemon *mon = &gPlayerParty[0];
    u8 raw[20];
    u32 i;
    u16 species;
    static u16 sLastSpecies;
    static u8 sLastLevel;
    static u8 sTicks;

    if (gPlayerPartyCount == 0)
        return;
    for (i = 0; i < gPlayerPartyCount; i++)
    {
        if (GetMonData(&gPlayerParty[i], MON_DATA_HP) != 0
            && GetMonData(&gPlayerParty[i], MON_DATA_SPECIES) != SPECIES_NONE)
        {
            mon = &gPlayerParty[i];
            break;
        }
    }
    species = GetMonData(mon, MON_DATA_SPECIES);
    if (species == sLastSpecies && GetMonData(mon, MON_DATA_LEVEL) == sLastLevel && (sTicks++ % 90) != 0)
        return;
    sLastSpecies = species;
    sLastLevel = GetMonData(mon, MON_DATA_LEVEL);
    memset(raw, 0, sizeof(raw));
    raw[0] = (u8)species;
    raw[1] = (u8)(species >> 8);
    raw[2] = sLastLevel;
    raw[4] = (u8)GetMonData(mon, MON_DATA_HELD_ITEM);
    raw[5] = (u8)(GetMonData(mon, MON_DATA_HELD_ITEM) >> 8);
    for (i = 0; i < 4; i++)
    {
        u16 move = GetMonData(mon, MON_DATA_MOVE1 + i);
        raw[6 + i * 2] = (u8)move;
        raw[7 + i * 2] = (u8)(move >> 8);
    }
    raw[14] = (u8)GetMonData(mon, MON_DATA_HP);
    raw[15] = (u8)(GetMonData(mon, MON_DATA_HP) >> 8);
    raw[16] = (u8)GetMonData(mon, MON_DATA_MAX_HP);
    raw[17] = (u8)(GetMonData(mon, MON_DATA_MAX_HP) >> 8);
    CoopNet_SendBytes(COOP_MSG_WORLD_CONFIG, raw, sizeof(raw));
}

/* Share the complete three-Pokemon battle roster. struct Pokemon contains
 * only fixed-width save data (no pointers), and both phones run this same
 * Heart & Soul build/ABI. The legacy lead summary remains for the presence
 * and readiness checks; the blob supplies all battle stats, PP, status,
 * held items, and switch candidates. */
STATIC_ASSERT(sizeof(struct Pokemon) * MULTI_PARTY_SIZE <= 512, CoopPartyBlobTooLarge);

static void PublishParty(void)
{
    u8 raw[sizeof(struct Pokemon) * MULTI_PARTY_SIZE];
    static u8 last[sizeof(struct Pokemon) * MULTI_PARTY_SIZE];
    static bool8 haveLast;
    static u8 ticks;
    u32 i;
    bool8 changed;

    memset(raw, 0, sizeof(raw));
    for (i = 0; i < MULTI_PARTY_SIZE && i < gPlayerPartyCount; i++)
    {
        if (GetMonData(&gPlayerParty[i], MON_DATA_SANITY_HAS_SPECIES, NULL))
            memcpy(raw + i * sizeof(struct Pokemon), &gPlayerParty[i], sizeof(struct Pokemon));
    }
    changed = !haveLast || memcmp(last, raw, sizeof(raw)) != 0;
    if (!changed && ++ticks < 90)
        return;
    if (CoopNet_SendBytes(COOP_MSG_WORLD_PARTY_BLOB, raw, sizeof(raw)))
    {
        memcpy(last, raw, sizeof(raw));
        haveLast = TRUE;
        ticks = 0;
    }
    else
    {
        ticks = 89;
    }
}

bool8 Coop_IsStorySyncReady(void)
{
    return sSessionLive && sScanDone && sRemoteReady && sOutN == 0;
}

static bool8 PartnerInRange(void)
{
    struct CoopRemoteView remote;
    struct ObjectEvent *player;
    s16 dx, dy;

    CoopNet_CopyRemote(&remote);
    if (!remote.valid || remote.ageMs > 3000 || remote.sio == 0 || remote.x == 0)
        return FALSE;
    if (gPlayerAvatar.objectEventId >= OBJECT_EVENTS_COUNT || gSaveBlock1Ptr == NULL)
        return FALSE;
    player = &gObjectEvents[gPlayerAvatar.objectEventId];
    if (!player->active)
        return FALSE;
    if (remote.mapId != ((u32)gSaveBlock1Ptr->location.mapGroup
        | ((u32)gSaveBlock1Ptr->location.mapNum << 16)))
        return FALSE;
    dx = player->currentCoords.x - (s16)remote.x;
    dy = player->currentCoords.y - (s16)remote.y;
    if (dx < 0)
        dx = -dx;
    if (dy < 0)
        dy = -dy;
    return (dx + dy) <= 12;
}

bool8 Coop_PartnersNearby(void)
{
    return Coop_IsStorySyncReady() && PartnerInRange()
        && sPartnerLeadValid && sPartnerPartyValid;
}

bool8 Coop_BlockFieldEvent(void)
{
    struct CoopRemoteView remote;

    if (!sSessionLive || !CoopNet_IsUp())
        return FALSE;
    /* Our own script. The lock we took for the partner is not one of those. */
    if (ScriptContext_IsEnabled() && !sLockedForPartner)
        return TRUE;
    CoopNet_CopyRemote(&remote);
    if (!remote.valid || remote.ageMs > 3000)
        return FALSE;
    return (remote.sio & COOP_SIO_SCRIPT) != 0;
}

bool8 Coop_BlockTrainerApproach(void)
{
    if (Coop_BlockFieldEvent())
        return TRUE;
    if (!sSessionLive || !CoopNet_IsUp())
        return FALSE;
    if (!Coop_IsStorySyncReady())
        return TRUE;
    return PartnerInRange() && (!sPartnerLeadValid || !sPartnerPartyValid);
}

u8 Coop_ChooseTrainerApproach(u8 localDistance, u8 partnerDistance)
{
    if (!sSessionLive || !CoopNet_IsUp())
        return localDistance;
    if (!Coop_IsStorySyncReady())
        return 0;
    if (PartnerInRange() && (!sPartnerLeadValid || !sPartnerPartyValid))
        return 0;
    if (!Coop_PartnersNearby())
        return localDistance;
    /* Both phones run the same trainer approach. If either player is in
     * range, the remote tile supplies the same trigger on the other phone. */
    if (localDistance != 0)
        return localDistance;
    return partnerDistance;
}

bool8 Coop_CopyPartnerTile(s16 *x, s16 *y)
{
    struct CoopRemoteView remote;

    if (x == NULL || y == NULL)
        return FALSE;
    CoopNet_CopyRemote(&remote);
    if (!remote.valid || remote.ageMs > 3000 || remote.x == 0)
        return FALSE;
    *x = (s16)remote.x;
    *y = (s16)remote.y;
    return TRUE;
}

void Coop_PrepareSharedTrainerBattle(void)
{
    u32 i;

    if (!CoopNet_IsUp() || !Coop_PartnersNearby())
        return;
    if (!sInBattle)
        Coop_OnBattleStarted();
    if (!sInBattle)
        return;
    for (i = 0; i < MULTI_PARTY_SIZE; i++)
    {
        struct Pokemon *dst = &gPlayerParty[MULTI_PARTY_SIZE + i];
        if (GetMonData(&sPartnerParty[i], MON_DATA_SANITY_HAS_SPECIES, NULL))
            *dst = sPartnerParty[i];
        else
            ZeroMonData(dst);
    }
    CalculatePlayerPartyCount();
    gBattleTypeFlags |= BATTLE_TYPE_DOUBLE | BATTLE_TYPE_MULTI | BATTLE_TYPE_INGAME_PARTNER | BATTLE_TYPE_COOP;
    sPartySaved = TRUE;
}

void Coop_RestoreSharedBattleParty(bool8 fullRestore)
{
    u32 i;

    if (!sPartySaved)
        return;
    if (fullRestore)
    {
        gPlayerPartyCount = sPartyCountBackup;
        for (i = 0; i < PARTY_SIZE; i++)
            gPlayerParty[i] = sPartyBackup[i];
    }
    else
    {
        for (i = MULTI_PARTY_SIZE; i < PARTY_SIZE; i++)
            gPlayerParty[i] = sPartyBackup[i];
    }
    CalculatePlayerPartyCount();
    sPartySaved = FALSE;
}

bool8 Coop_PartnerVisible(void)
{
    struct CoopRemoteView remote;
    u32 localMap;

    CoopNet_CopyRemote(&remote);
    if (!remote.valid || remote.ageMs > (sPartnerBackground ? COOP_BACKGROUND_HOLD_MS : COOP_PARTNER_HIDE_MS)
        || gSaveBlock1Ptr == NULL)
        return FALSE;
    localMap = (u32)gSaveBlock1Ptr->location.mapGroup
        | ((u32)gSaveBlock1Ptr->location.mapNum << 16);
    return remote.mapId == localMap && remote.x != 0;
}

void Coop_Boot(void)
{
    CoopNet_Boot();
}

void Coop_Frame(void)
{
    bool8 up = CoopNet_IsUp();

    if (up && !sLinkWasUp)
    {
        sSessionLive = TRUE;
        ResetScan();
        sAbortSent = FALSE;
    }
    if (!up && sLinkWasUp)
    {
        sRemoteReady = FALSE;
        sPartnerPartyValid = FALSE;
        if (!sInBattle)
        {
            sBattleEpoch = 0;
            sBattleTurn = 0;
            sLocalBattleActionSent = FALSE;
            sRemoteBattleActionReady = FALSE;
            sBattleSeedSent = FALSE;
            sRemoteBattleSeedReady = FALSE;
            sBattleSetupApplied = FALSE;
            sRemoteBattleSetupReady = FALSE;
        }
        sReadyBurst = 0;
    }
    sLinkWasUp = up;
    if (up)
        sSessionLive = TRUE;
    if (!sSessionLive)
        return;
    HandleInbound();
    if (up)
    {
        bool8 background = CoopNet_IsBackgrounded();
        ScanStory();
        FlushOut();
        AdvertiseReady();
        PublishConfig();
        PublishPosition();
        PublishLead();
        PublishParty();
        if (background != sAnnouncedBackground)
        {
            if (CoopNet_SendWord(COOP_MSG_WORLD_READY,
                    background ? COOP_READY_BACKGROUND : COOP_READY_FOREGROUND))
                sAnnouncedBackground = background;
        }
    }
    if (CoopNet_PartnerAgeMs() < 2000)
        sHadPartner = TRUE;
    WatchBattle();
    MaintainScriptLock();
    MaintainPartner();
    if (sInBattle && !sAbort && !gMain.inBattle)
        sInBattle = FALSE;
}

static void ReleasePartnerLock(void)
{
    if (!sLockedForPartner)
        return;
    sLockedForPartner = FALSE;
    if (!ScriptContext_IsEnabled())
        UnlockPlayerFieldControls();
}

static void MaintainScriptLock(void)
{
    struct CoopRemoteView remote;
    bool8 remoteScript;

    if (!sSessionLive || !CoopNet_IsUp())
    {
        ReleasePartnerLock();
        return;
    }
    /* A menu or battle hides the field. Leave the latch set so coming back
     * does not unlock a cutscene that is still running. */
    if (gMain.callback2 != CB2_Overworld)
        return;
    CoopNet_CopyRemote(&remote);
    remoteScript = remote.valid && remote.ageMs <= 3000 && (remote.sio & COOP_SIO_SCRIPT) != 0;
    if (remoteScript && !sLockedForPartner && !ArePlayerFieldControlsLocked())
    {
        LockPlayerFieldControls();
        sLockedForPartner = TRUE;
    }
    else if (!remoteScript)
    {
        ReleasePartnerLock();
    }
}

static bool8 SameMonIdentity(struct Pokemon *a, struct Pokemon *b)
{
    if (!GetMonData(a, MON_DATA_SANITY_HAS_SPECIES, NULL))
        return FALSE;
    if (!GetMonData(b, MON_DATA_SANITY_HAS_SPECIES, NULL))
        return FALSE;
    return GetMonData(a, MON_DATA_PERSONALITY, NULL) == GetMonData(b, MON_DATA_PERSONALITY, NULL)
        && GetMonData(a, MON_DATA_OT_ID, NULL) == GetMonData(b, MON_DATA_OT_ID, NULL);
}

bool8 Coop_OwnsBattleMon(struct Pokemon *mon)
{
    u32 i;

    if (!(gBattleTypeFlags & BATTLE_TYPE_COOP) || !sPartySaved || mon == NULL)
        return TRUE;
    for (i = 0; i < PARTY_SIZE; i++)
    {
        if (SameMonIdentity(mon, &sPartyBackup[i]))
            return TRUE;
    }
    return FALSE;
}

void Coop_DropSavedMon(struct Pokemon *mon)
{
    u32 i, j;

    if (!sPartySaved || mon == NULL)
        return;
    for (i = 0; i < PARTY_SIZE; i++)
    {
        if (!SameMonIdentity(mon, &sPartyBackup[i]))
            continue;
        for (j = i; j + 1 < PARTY_SIZE; j++)
            sPartyBackup[j] = sPartyBackup[j + 1];
        ZeroMonData(&sPartyBackup[PARTY_SIZE - 1]);
        if (sPartyCountBackup > 0)
            sPartyCountBackup--;
        return;
    }
}

void Coop_FinishSharedBattle(void)
{
    struct Pokemon next[PARTY_SIZE];
    u32 n = 0;
    u32 i, j;

    if (!sPartySaved || !(gBattleTypeFlags & BATTLE_TYPE_COOP))
        return;
    memset(next, 0, sizeof(next));
    for (i = 0; i < PARTY_SIZE; i++)
    {
        bool8 found = FALSE;
        if (!GetMonData(&sPartyBackup[i], MON_DATA_SANITY_HAS_SPECIES, NULL))
            continue;
        for (j = 0; j < PARTY_SIZE; j++)
        {
            if (!SameMonIdentity(&gPlayerParty[j], &sPartyBackup[i]))
                continue;
            next[n++] = gPlayerParty[j];
            found = TRUE;
            break;
        }
        if (!found && n < PARTY_SIZE)
            next[n++] = sPartyBackup[i];
    }
    for (i = 0; i < PARTY_SIZE; i++)
        gPlayerParty[i] = next[i];
    CalculatePlayerPartyCount();
    sPartySaved = FALSE;
}

void Coop_OnBattleStarted(void)
{
    struct ObjectEvent *obj;
    u32 i;
    bool8 keepSnapshot;

    sBattleHadLink = CoopNet_IsUp();
    if (!sBattleHadLink)
        return;
    keepSnapshot = sInBattle && sPartySaved;
    sInBattle = TRUE;
    sAbort = FALSE;
    sAbortSent = FALSE;
    /* PrepareSharedTrainerBattle already snapped the real party, then
     * injected the partner lead. A second call from battle init must not
     * photograph that temporary lead. */
    if (keepSnapshot)
        return;
    sBattleEpoch++;
    if (sBattleEpoch == 0)
        sBattleEpoch = 1;
    sBattleTurn = 0;
    sLocalBattleActionSent = FALSE;
    if (sRemoteBattleActionEpoch != sBattleEpoch || sRemoteBattleActionTurn != 0)
        sRemoteBattleActionReady = FALSE;
    sBattleSeedSent = FALSE;
    if (sRemoteBattleSeedEpoch != sBattleEpoch || sRemoteBattleSeedTurn != 0)
        sRemoteBattleSeedReady = FALSE;
    sBattleSetupApplied = FALSE;
    if (sRemoteBattleSetupEpoch != sBattleEpoch)
        sRemoteBattleSetupReady = FALSE;
    memset(&sRemoteBattleAction, 0, sizeof(sRemoteBattleAction));
    sPartyCountBackup = gPlayerPartyCount;
    for (i = 0; i < PARTY_SIZE; i++)
        sPartyBackup[i] = gPlayerParty[i];
    sMoneyBackup = GetMoney(&gSaveBlock1Ptr->money);
    sSpotX = 0;
    sSpotY = 0;
    sSpotFacing = DIR_SOUTH;
    sSpotElevation = 3;
    if (gPlayerAvatar.objectEventId < OBJECT_EVENTS_COUNT)
    {
        obj = &gObjectEvents[gPlayerAvatar.objectEventId];
        if (obj->active)
        {
            sSpotX = obj->currentCoords.x;
            sSpotY = obj->currentCoords.y;
            sSpotFacing = obj->facingDirection;
            sSpotElevation = obj->currentElevation;
        }
    }
    sTrainerA = 0;
    sTrainerB = 0;
    sTrainerABeaten = FALSE;
    sTrainerBBeaten = FALSE;
    if (gBattleTypeFlags & BATTLE_TYPE_TRAINER)
    {
        sTrainerA = TRAINER_BATTLE_PARAM.opponentA;
        sTrainerB = TRAINER_BATTLE_PARAM.opponentB;
        if (sTrainerA != 0)
            sTrainerABeaten = HasTrainerBeenFought(sTrainerA);
        if (sTrainerB != 0)
            sTrainerBBeaten = HasTrainerBeenFought(sTrainerB);
    }
}

bool8 Coop_BattleTakeRemoteAction(struct CoopBattleAction *out)
{
    if (out == NULL || !sInBattle || !sRemoteBattleActionReady)
        return FALSE;
    if (sRemoteBattleActionEpoch != sBattleEpoch || sRemoteBattleActionTurn != sBattleTurn)
    {
        if (sRemoteBattleActionEpoch != sBattleEpoch || sRemoteBattleActionTurn < sBattleTurn)
            sRemoteBattleActionReady = FALSE;
        return FALSE;
    }
    *out = sRemoteBattleAction;
    sRemoteBattleActionReady = FALSE;
    return TRUE;
}

bool8 Coop_BattleSendLocalAction(const struct CoopBattleAction *action)
{
    u8 wire[12];

    if (action == NULL || !sInBattle || !CoopNet_IsUp())
        return FALSE;
    if (sLocalBattleActionSent)
        return TRUE;
    wire[0] = (u8)(sBattleEpoch >> 8);
    wire[1] = (u8)sBattleEpoch;
    wire[2] = (u8)(sBattleTurn >> 8);
    wire[3] = (u8)sBattleTurn;
    wire[4] = action->action;
    wire[5] = action->data1;
    wire[6] = action->data2;
    wire[7] = action->data3;
    wire[8] = action->switchIndex;
    wire[9] = action->movePosition;
    wire[10] = (u8)(action->moveTarget >> 8);
    wire[11] = (u8)action->moveTarget;
    if (!CoopNet_SendBytes(COOP_MSG_BATTLE_ACTION, wire, sizeof(wire)))
        return FALSE;
    sLocalBattleActionSent = TRUE;
    return TRUE;
}

static u32 HashBattleBytes(u32 hash, const void *data, u32 size)
{
    const u8 *bytes = data;
    u32 i;
    for (i = 0; i < size; i++)
    {
        hash ^= bytes[i];
        hash *= 16777619;
    }
    return hash;
}

static u32 BattleStateChecksum(void)
{
    u32 hash = 2166136261u;
    u32 i;
    u8 hostBattler = CoopNet_Role() == 0 ? 0 : 1;
    u8 guestBattler = CoopNet_Role() == 0 ? 1 : 0;

    hash = HashBattleBytes(hash, &gBattleMons[hostBattler], sizeof(gBattleMons[hostBattler]));
    hash = HashBattleBytes(hash, &gBattleMons[guestBattler], sizeof(gBattleMons[guestBattler]));
    for (i = 2; i < gBattlersCount; i++)
        hash = HashBattleBytes(hash, &gBattleMons[i], sizeof(gBattleMons[i]));
    if (CoopNet_Role() == 0)
    {
        hash = HashBattleBytes(hash, &gPlayerParty[0], sizeof(struct Pokemon) * MULTI_PARTY_SIZE);
        hash = HashBattleBytes(hash, &gPlayerParty[MULTI_PARTY_SIZE], sizeof(struct Pokemon) * MULTI_PARTY_SIZE);
    }
    else
    {
        hash = HashBattleBytes(hash, &gPlayerParty[MULTI_PARTY_SIZE], sizeof(struct Pokemon) * MULTI_PARTY_SIZE);
        hash = HashBattleBytes(hash, &gPlayerParty[0], sizeof(struct Pokemon) * MULTI_PARTY_SIZE);
    }
    hash = HashBattleBytes(hash, gEnemyParty, sizeof(gEnemyParty));
    hash = HashBattleBytes(hash, &gBattleWeather, sizeof(gBattleWeather));
    hash = HashBattleBytes(hash, &gBattleTypeFlags, sizeof(gBattleTypeFlags));
    hash = HashBattleBytes(hash, &gBattleOutcome, sizeof(gBattleOutcome));
    hash = HashBattleBytes(hash, &gBattlersCount, sizeof(gBattlersCount));
    return hash;
}

bool8 Coop_BattleSyncTurnStart(void)
{
    u8 wire[40];

    if (!sInBattle || !CoopNet_IsUp())
        return FALSE;
    if (CoopNet_Role() == 0)
    {
        if (!sBattleSetupApplied)
        {
            u8 setup[1024];
            u32 offset = 12;
            u32 setupLen = 12 + sizeof(gEnemyParty) + 2 * sizeof(gBattleMons[0]);
            if (setupLen > sizeof(setup))
                return FALSE;
            setup[0] = (u8)(sBattleEpoch >> 8);
            setup[1] = (u8)sBattleEpoch;
            setup[2] = gBattlersCount;
            setup[3] = 0;
            setup[4] = (u8)(gBattleTypeFlags >> 24);
            setup[5] = (u8)(gBattleTypeFlags >> 16);
            setup[6] = (u8)(gBattleTypeFlags >> 8);
            setup[7] = (u8)gBattleTypeFlags;
            setup[8] = (u8)(gBattleWeather >> 24);
            setup[9] = (u8)(gBattleWeather >> 16);
            setup[10] = (u8)(gBattleWeather >> 8);
            setup[11] = (u8)gBattleWeather;
            memcpy(setup + offset, gEnemyParty, sizeof(gEnemyParty));
            offset += sizeof(gEnemyParty);
            memcpy(setup + offset, &gBattleMons[2], sizeof(gBattleMons[0]));
            offset += sizeof(gBattleMons[0]);
            memcpy(setup + offset, &gBattleMons[3], sizeof(gBattleMons[0]));
            if (!CoopNet_SendBytes(COOP_MSG_BATTLE_SETUP, setup, setupLen))
                return FALSE;
            sBattleSetupApplied = TRUE;
        }
        if (sBattleSeedSent)
            return TRUE;
        wire[0] = (u8)(sBattleEpoch >> 8);
        wire[1] = (u8)sBattleEpoch;
        wire[2] = (u8)(sBattleTurn >> 8);
        wire[3] = (u8)sBattleTurn;
        wire[4] = (u8)(gRngValue.a >> 24);
        wire[5] = (u8)(gRngValue.a >> 16);
        wire[6] = (u8)(gRngValue.a >> 8);
        wire[7] = (u8)gRngValue.a;
        wire[8] = (u8)(gRngValue.b >> 24);
        wire[9] = (u8)(gRngValue.b >> 16);
        wire[10] = (u8)(gRngValue.b >> 8);
        wire[11] = (u8)gRngValue.b;
        wire[12] = (u8)(gRngValue.c >> 24);
        wire[13] = (u8)(gRngValue.c >> 16);
        wire[14] = (u8)(gRngValue.c >> 8);
        wire[15] = (u8)gRngValue.c;
        wire[16] = (u8)(gRngValue.ctr >> 24);
        wire[17] = (u8)(gRngValue.ctr >> 16);
        wire[18] = (u8)(gRngValue.ctr >> 8);
        wire[19] = (u8)gRngValue.ctr;
        wire[20] = (u8)(gRng2Value.a >> 24);
        wire[21] = (u8)(gRng2Value.a >> 16);
        wire[22] = (u8)(gRng2Value.a >> 8);
        wire[23] = (u8)gRng2Value.a;
        wire[24] = (u8)(gRng2Value.b >> 24);
        wire[25] = (u8)(gRng2Value.b >> 16);
        wire[26] = (u8)(gRng2Value.b >> 8);
        wire[27] = (u8)gRng2Value.b;
        wire[28] = (u8)(gRng2Value.c >> 24);
        wire[29] = (u8)(gRng2Value.c >> 16);
        wire[30] = (u8)(gRng2Value.c >> 8);
        wire[31] = (u8)gRng2Value.c;
        wire[32] = (u8)(gRng2Value.ctr >> 24);
        wire[33] = (u8)(gRng2Value.ctr >> 16);
        wire[34] = (u8)(gRng2Value.ctr >> 8);
        wire[35] = (u8)gRng2Value.ctr;
        {
            u32 checksum = BattleStateChecksum();
            wire[36] = (u8)(checksum >> 24);
            wire[37] = (u8)(checksum >> 16);
            wire[38] = (u8)(checksum >> 8);
            wire[39] = (u8)checksum;
        }
        if (!CoopNet_SendBytes(COOP_MSG_BATTLE_SEED, wire, sizeof(wire)))
            return FALSE;
        sBattleSeedSent = TRUE;
        return TRUE;
    }
    if (!sBattleSetupApplied)
    {
        u32 offset = 12;
        u32 expectedLen = 12 + sizeof(gEnemyParty) + 2 * sizeof(gBattleMons[0]);
        if (!sRemoteBattleSetupReady || sRemoteBattleSetupEpoch != sBattleEpoch)
            return FALSE;
        if (sRemoteBattleSetupLen != expectedLen)
        {
            sAbort = TRUE;
            if (gBattleOutcome == 0)
                gBattleOutcome = B_OUTCOME_PLAYER_TELEPORTED;
            return FALSE;
        }
        gBattlersCount = sRemoteBattleSetup[2];
        gBattleTypeFlags = ReadBe32(sRemoteBattleSetup + 4);
        gBattleWeather = ReadBe32(sRemoteBattleSetup + 8);
        memcpy(gEnemyParty, sRemoteBattleSetup + offset, sizeof(gEnemyParty));
        offset += sizeof(gEnemyParty);
        memcpy(&gBattleMons[2], sRemoteBattleSetup + offset, sizeof(gBattleMons[0]));
        offset += sizeof(gBattleMons[0]);
        memcpy(&gBattleMons[3], sRemoteBattleSetup + offset, sizeof(gBattleMons[0]));
        sBattleSetupApplied = TRUE;
        sRemoteBattleSetupReady = FALSE;
    }
    if (!sRemoteBattleSeedReady)
        return FALSE;
    if (sRemoteBattleSeedEpoch != sBattleEpoch || sRemoteBattleSeedTurn != sBattleTurn)
    {
        if (sRemoteBattleSeedEpoch != sBattleEpoch || sRemoteBattleSeedTurn < sBattleTurn)
            sRemoteBattleSeedReady = FALSE;
        return FALSE;
    }
    if (BattleStateChecksum() != sRemoteBattleChecksum)
    {
        sAbort = TRUE;
        if (gBattleOutcome == 0)
            gBattleOutcome = B_OUTCOME_PLAYER_TELEPORTED;
        return FALSE;
    }
    gRngValue = sRemoteBattleRng;
    gRng2Value = sRemoteBattleRng2;
    sRemoteBattleSeedReady = FALSE;
    return TRUE;
}

void Coop_BattleTurnComplete(void)
{
    if (!sInBattle)
        return;
    sBattleTurn++;
    sLocalBattleActionSent = FALSE;
    if (sRemoteBattleActionEpoch != sBattleEpoch || sRemoteBattleActionTurn != sBattleTurn)
        sRemoteBattleActionReady = FALSE;
    sBattleSeedSent = FALSE;
    if (sRemoteBattleSeedEpoch != sBattleEpoch || sRemoteBattleSeedTurn != sBattleTurn)
        sRemoteBattleSeedReady = FALSE;
}

bool8 Coop_BattleAbortPending(void)
{
    return sAbort;
}

void Coop_LeaveAbortedBattle(void)
{
    struct ObjectEvent *obj;
    u32 i;
    u32 money;

    sAbort = FALSE;
    sInBattle = FALSE;
    sBattleHadLink = FALSE;
    if (sNewPeerSession)
    {
        sBattleEpoch = 0;
        sBattleTurn = 0;
        sLocalBattleActionSent = FALSE;
        sRemoteBattleActionReady = FALSE;
        sBattleSeedSent = FALSE;
        sRemoteBattleSeedReady = FALSE;
        sNewPeerSession = FALSE;
    }
    Coop_RestoreSharedBattleParty(TRUE);
    gPlayerPartyCount = sPartyCountBackup;
    for (i = 0; i < PARTY_SIZE; i++)
        gPlayerParty[i] = sPartyBackup[i];
    CalculatePlayerPartyCount();
    money = GetMoney(&gSaveBlock1Ptr->money);
    if (money > sMoneyBackup)
        RemoveMoney(&gSaveBlock1Ptr->money, money - sMoneyBackup);
    else if (money < sMoneyBackup)
        AddMoney(&gSaveBlock1Ptr->money, sMoneyBackup - money);
    if (sTrainerA != 0 && !sTrainerABeaten)
        ClearTrainerFlag(sTrainerA);
    if (sTrainerB != 0 && !sTrainerBBeaten)
        ClearTrainerFlag(sTrainerB);
    if (gPlayerAvatar.objectEventId < OBJECT_EVENTS_COUNT)
    {
        obj = &gObjectEvents[gPlayerAvatar.objectEventId];
        obj->currentCoords.x = sSpotX;
        obj->currentCoords.y = sSpotY;
        obj->previousCoords.x = sSpotX;
        obj->previousCoords.y = sSpotY;
        obj->facingDirection = sSpotFacing;
        obj->currentElevation = sSpotElevation;
        obj->previousElevation = sSpotElevation;
        if (obj->active && obj->spriteId < MAX_SPRITES && gSprites[obj->spriteId].inUse)
        {
            MoveObjectEventToMapCoords(obj, sSpotX, sSpotY);
            SetObjectEventDirection(obj, sSpotFacing);
        }
    }
    gNoOfApproachingTrainers = 0;
    gApproachingTrainerId = 0;
    gTrainerApproachedPlayer = FALSE;
    ScriptContext_Init();
    gMain.inBattle = FALSE;
    gMain.callback1 = gPreBattleCallback1;
    SetMainCallback2(CB2_ReturnToField);
    gFieldCallback = FieldCB_ReturnToFieldNoScriptCheckMusic;
}

bool8 Coop_IsReservedPresenceObjectEventId(u8 objectEventId)
{
    return objectEventId == COOP_PARTNER_SLOT || objectEventId == COOP_FOLLOWER_SLOT;
}

bool8 Coop_IsCoopLocalId(u8 localId)
{
    return localId == COOP_LOCAL_PARTNER || localId == COOP_LOCAL_FOLLOWER;
}

u8 Coop_TakePreferredObjectEventId(void)
{
    u8 id = sPreferSlot;
    sPreferSlot = OBJECT_EVENTS_COUNT;
    if (id < OBJECT_EVENTS_COUNT && !gObjectEvents[id].active)
        return id;
    return OBJECT_EVENTS_COUNT;
}

#else
typedef int CoopUnusedOnGba;
#endif
