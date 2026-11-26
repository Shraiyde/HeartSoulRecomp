#include "global.h"
#include "coop_net.h"
#include "coop_sync.h"
#include "event_data.h"
#include "constants/flags.h"

#define COOP_MSG_FLAG_CHANGE 1
#define COOP_MSG_FLAG_SNAPSHOT 2
#define COOP_SHARED_FLAG_FIRST 0x20
#define COOP_FLAG_SNAPSHOT_BITS 32
#define COOP_FLAG_SNAPSHOT_BYTES 6

static bool8 sApplyingRemoteFlag;
static bool8 sWasConnected;
static u16 sSnapshotCursor;

static bool8 IsSharedFlag(u16 flagId)
{
    return flagId >= COOP_SHARED_FLAG_FIRST && flagId < DAILY_FLAGS_START;
}

static void WriteU16BE(u8 *data, u16 value)
{
    data[0] = value >> 8;
    data[1] = value;
}

static u16 ReadU16BE(const u8 *data)
{
    return ((u16)data[0] << 8) | data[1];
}

static void WriteU32BE(u8 *data, u32 value)
{
    data[0] = value >> 24;
    data[1] = value >> 16;
    data[2] = value >> 8;
    data[3] = value;
}

static u32 ReadU32BE(const u8 *data)
{
    return ((u32)data[0] << 24) | ((u32)data[1] << 16)
         | ((u32)data[2] << 8) | data[3];
}

void CoopSync_FlagChanged(u16 flagId, bool8 value)
{
    u8 payload[3];

    if (sApplyingRemoteFlag || !CoopNet_IsConnected() || !IsSharedFlag(flagId))
        return;

    WriteU16BE(payload, flagId);
    payload[2] = value ? 1 : 0;
    CoopNet_SendReliable(COOP_MSG_FLAG_CHANGE, payload, sizeof(payload));
}

static void ApplyRemoteFlag(u16 flagId, bool8 value)
{
    if (!IsSharedFlag(flagId))
        return;

    sApplyingRemoteFlag = TRUE;
    if (value)
        FlagSet(flagId);
    else
        FlagClear(flagId);
    sApplyingRemoteFlag = FALSE;
}

static void SendSnapshotChunk(void)
{
    u8 payload[COOP_FLAG_SNAPSHOT_BYTES];
    u32 mask = 0;
    u16 bit;

    if (sSnapshotCursor >= DAILY_FLAGS_START)
        return;

    for (bit = 0; bit < COOP_FLAG_SNAPSHOT_BITS
         && sSnapshotCursor + bit < DAILY_FLAGS_START; bit++)
    {
        if (FlagGet(sSnapshotCursor + bit))
            mask |= (1u << bit);
    }

    WriteU16BE(payload, sSnapshotCursor);
    WriteU32BE(payload + 2, mask);
    if (CoopNet_SendReliable(COOP_MSG_FLAG_SNAPSHOT, payload, sizeof(payload)))
        sSnapshotCursor += COOP_FLAG_SNAPSHOT_BITS;
}

static void ApplySnapshotChunk(const u8 *payload)
{
    u16 firstFlag = ReadU16BE(payload);
    u32 mask = ReadU32BE(payload + 2);
    u16 bit;

    if (firstFlag < COOP_SHARED_FLAG_FIRST
        || firstFlag >= DAILY_FLAGS_START
        || (firstFlag & (COOP_FLAG_SNAPSHOT_BITS - 1)) != 0)
        return;

    for (bit = 0; bit < COOP_FLAG_SNAPSHOT_BITS; bit++)
    {
        u16 flagId = firstFlag + bit;
        if (flagId >= DAILY_FLAGS_START)
            break;
        /* Initial synchronization merges progress: set flags are shared,
         * while an unset flag never erases progress from the other save. */
        if (mask & (1u << bit))
            ApplyRemoteFlag(flagId, TRUE);
    }
}

void CoopSync_Update(void)
{
    u8 messageType;
    u8 payload[COOP_NET_MESSAGE_MAX];
    u16 payloadSize;
    bool8 connected = CoopNet_IsConnected();

    if (!connected)
    {
        sWasConnected = FALSE;
        sSnapshotCursor = COOP_SHARED_FLAG_FIRST;
        return;
    }

    if (!sWasConnected)
    {
        sWasConnected = TRUE;
        sSnapshotCursor = COOP_SHARED_FLAG_FIRST;
    }

    while (TRUE)
    {
        payloadSize = sizeof(payload);
        if (!CoopNet_PopMessage(&messageType, payload, &payloadSize))
            break;

        if (messageType == COOP_MSG_FLAG_CHANGE && payloadSize == 3)
        {
            u16 flagId = ReadU16BE(payload);
            if (IsSharedFlag(flagId))
                ApplyRemoteFlag(flagId, payload[2] != 0);
        }
        else if (messageType == COOP_MSG_FLAG_SNAPSHOT
                 && payloadSize == COOP_FLAG_SNAPSHOT_BYTES)
        {
            ApplySnapshotChunk(payload);
        }
    }

    /* Both peers send their set flags once on joining. The merge is additive,
     * so neither player's existing progress is lost during initial sync. */
    SendSnapshotChunk();
}
