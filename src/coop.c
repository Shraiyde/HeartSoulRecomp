#include "global.h"

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
    case FLAG_RECEIVED_HM_WHIRLPOOL: itemId = ITEM_HM_WHIRLPOOL; break;
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

static void HandleInbound(void)
{
    u16 type;
    u8 payload[96];
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
    words[1] = sInBattle ? 0 : 1;
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
    if (!remote->valid || remote->ageMs > COOP_PARTNER_HIDE_MS)
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
    if (!remote.valid || remote.ageMs > COOP_PARTNER_HIDE_MS)
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
                StepToward(follow, obj->currentCoords.x, obj->currentCoords.y, PLAYER_AVATAR_STATE_NORMAL);
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
    if (sHadPartner && age > COOP_BATTLE_SILENCE_MS)
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
        ScanStory();
        FlushOut();
        AdvertiseReady();
        PublishConfig();
        PublishPosition();
    }
    if (CoopNet_PartnerAgeMs() < 2000)
        sHadPartner = TRUE;
    WatchBattle();
    MaintainPartner();
    if (sInBattle && !sAbort && !gMain.inBattle)
        sInBattle = FALSE;
}

void Coop_OnBattleStarted(void)
{
    struct ObjectEvent *obj;
    u32 i;

    sBattleHadLink = CoopNet_IsUp();
    if (!sBattleHadLink)
        return;
    sInBattle = TRUE;
    sAbort = FALSE;
    sAbortSent = FALSE;
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
