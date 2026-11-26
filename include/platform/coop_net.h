#ifndef COOP_NET_H
#define COOP_NET_H

#include <stdbool.h>
#include <stdint.h>

#define COOP_MSG_POSITION          16
#define COOP_MSG_WORLD_FLAG        17
#define COOP_MSG_SIO_MULTI         23
#define COOP_MSG_WORLD_VAR         24
#define COOP_MSG_WORLD_READY       25
#define COOP_MSG_WORLD_CONFIG      26
#define COOP_MSG_WORLD_CONFIG_ACK  27
#define COOP_MSG_SIO_RESYNC        28
#define COOP_MSG_WORLD_CONFIG_BLOB 29
#define COOP_MSG_WORLD_RESYNC      30
#define COOP_MSG_POSITION_RESYNC   31
#define COOP_MSG_WORLD_PARTY_BLOB  32
#define COOP_MSG_BATTLE_ACTION     33
#define COOP_MSG_BATTLE_SEED       34
#define COOP_MSG_SESSION_RESET     35
#define COOP_MSG_BATTLE_SETUP      37
#define COOP_MSG_SAVE_ID            38

#define COOP_READY_STORY           1u
#define COOP_READY_BATTLE_ABORT    2u
#define COOP_READY_BACKGROUND      3u
#define COOP_READY_FOREGROUND      4u

#define COOP_POSITION_WORDS        8

struct CoopRemoteView
{
    bool valid;
    uint32_t mapId;
    uint32_t sio;
    uint32_t x;
    uint32_t y;
    uint32_t gender;
    uint32_t facing;
    uint32_t follower;
    uint32_t avatar;
    uint32_t ageMs;
};

/* HNS_COOP_MODE host/join is the phone session. HNS_COOP_JOIN=1, with no
 * phone mode, joins the Windows PC host instead. */
void CoopNet_Boot(void);
void CoopNet_SetBackgrounded(bool background);
bool CoopNet_IsBackgrounded(void);

bool CoopNet_IsUp(void);
int CoopNet_Role(void);
uint32_t CoopNet_PartnerAgeMs(void);
void CoopNet_CopyRemote(struct CoopRemoteView *out);

/* Host-order words. The link thread sends them in the World Host layout. */
void CoopNet_SetPosition(const uint32_t words[COOP_POSITION_WORDS]);

/* Queue one host-order word (flag, var, ready, ack). Returns false if the
 * outbound queue is full; the caller keeps the word and tries again. */
bool CoopNet_SendWord(uint16_t type, uint32_t word);

/* Queue an already-packed payload (config, party, or battle-state snapshot). */
bool CoopNet_SendBytes(uint16_t type, const uint8_t *bytes, uint32_t len);

/* Pop one inbound non-position message. Payload is host-order for a single
 * word when *len == 4. Returns false when the queue is empty. */
bool CoopNet_Poll(uint16_t *type, uint8_t *payload, uint32_t *len, uint32_t cap);

#endif
