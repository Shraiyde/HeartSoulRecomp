#include "global.h"
#include "coop_net.h"
#include "coop_player.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "constants/event_object_movement.h"
#include "constants/event_objects.h"

#define COOP_REMOTE_LOCAL_ID 0xFE
#define COOP_POSITION_SEND_INTERVAL 4

static u8 sSendFrameCounter;
static bool8 sRemoteSpawned;
static u8 sRemoteMapNum;
static u8 sRemoteMapGroup;

static void RemoveRemoteAvatar(void)
{
    if (sRemoteSpawned)
        RemoveObjectEventByLocalIdAndMap(COOP_REMOTE_LOCAL_ID, sRemoteMapNum, sRemoteMapGroup);
    sRemoteSpawned = FALSE;
}

void CoopPlayer_Disconnect(void)
{
    RemoveRemoteAvatar();
    sSendFrameCounter = 0;
}

void CoopPlayer_Update(void)
{
    struct CoopNetPosition remote;
    s16 x, y;
    u8 objectEventId;
    u8 mapNum = gSaveBlock1Ptr->location.mapNum;
    u8 mapGroup = gSaveBlock1Ptr->location.mapGroup;

    if (!CoopNet_IsConnected())
    {
        RemoveRemoteAvatar();
        return;
    }

    /* Position is a replaceable snapshot, so it is intentionally sent less
     * often than the reliable gameplay stream. */
    if (++sSendFrameCounter >= COOP_POSITION_SEND_INTERVAL)
    {
        sSendFrameCounter = 0;
        PlayerGetDestCoords(&x, &y);
        CoopNet_SendPosition(mapGroup, mapNum, x, y,
                             GetPlayerFacingDirection(), PlayerGetElevation(),
                             GetPlayerAvatarGraphicsIdByCurrentState());
    }

    if (!CoopNet_GetRemotePosition(&remote))
        return;

    /* A partner on another map is not rendered as an NPC in this map. Keep
     * their latest snapshot; they reappear when both players share a map. */
    if (remote.mapGroup != mapGroup || remote.mapNum != mapNum)
    {
        RemoveRemoteAvatar();
        return;
    }

    if (sRemoteSpawned
        && (sRemoteMapNum != remote.mapNum || sRemoteMapGroup != remote.mapGroup))
        RemoveRemoteAvatar();

    /* This engine's TryGet helper returns TRUE when the object is absent. */
    if (TryGetObjectEventIdByLocalIdAndMap(COOP_REMOTE_LOCAL_ID,
                                            remote.mapNum, remote.mapGroup,
                                            &objectEventId))
    {
        objectEventId = SpawnSpecialObjectEventParameterized(
            remote.graphicsId, MOVEMENT_TYPE_NONE, COOP_REMOTE_LOCAL_ID,
            remote.x, remote.y, remote.elevation);
        if (objectEventId == OBJECT_EVENTS_COUNT)
            return;
        sRemoteSpawned = TRUE;
        sRemoteMapNum = remote.mapNum;
        sRemoteMapGroup = remote.mapGroup;
    }
    else
    {
        TryMoveObjectEventToMapCoords(COOP_REMOTE_LOCAL_ID, remote.mapNum,
                                      remote.mapGroup, remote.x, remote.y);
    }

    if (remote.facing >= DIR_SOUTH && remote.facing <= DIR_EAST)
        ObjectEventTurn(&gObjectEvents[objectEventId], remote.facing);
}
