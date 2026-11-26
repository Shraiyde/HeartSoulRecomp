#ifndef GUARD_COOP_NET_H
#define GUARD_COOP_NET_H

#include "global.h"

enum CoopNetState
{
    COOP_NET_DISCONNECTED,
    COOP_NET_LISTENING,
    COOP_NET_CONNECTING,
    COOP_NET_CONNECTED,
};

struct CoopNetPosition
{
    bool8 valid;
    u8 mapGroup;
    u8 mapNum;
    s16 x;
    s16 y;
    u8 facing;
    u32 receivedAtMs;
};

/*
 * Android/portable peer transport. Host and guest connect directly over LAN
 * or a VPN interface (for example ZeroTier); there is no relay/server.
 * Position snapshots are deliberately best-effort. Story and battle actions
 * will use the separate reliable message channel added above this transport.
 */
bool8 CoopNet_Host(u16 port);
bool8 CoopNet_Join(const char *ipv4, u16 port);
void CoopNet_Stop(void);
void CoopNet_Poll(void);
enum CoopNetState CoopNet_GetState(void);
bool8 CoopNet_IsConnected(void);
bool8 CoopNet_SendPosition(u8 mapGroup, u8 mapNum, s16 x, s16 y, u8 facing);
bool8 CoopNet_GetRemotePosition(struct CoopNetPosition *position);

/* Reliable, ordered gameplay messages. The transport owns a bounded outbox
 * until the peer acknowledges each message. The caller supplies an application
 * message type and an opaque payload; PopMessage copies one received message. */
#define COOP_NET_MESSAGE_MAX 240
bool8 CoopNet_SendReliable(u8 messageType, const void *payload, u16 payloadSize);
bool8 CoopNet_PopMessage(u8 *messageType, void *payload, u16 *payloadSize);

#endif // GUARD_COOP_NET_H
