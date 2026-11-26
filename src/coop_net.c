#include "global.h"
#include "coop_net.h"

#ifdef PORTABLE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define COOP_MAGIC 0x48534350u /* "HSCP" */
#define COOP_PROTOCOL 1
#define COOP_HEADER_SIZE 16
#define COOP_PACKET_MAX 512
#define COOP_TYPE_HELLO 1
#define COOP_TYPE_WELCOME 2
#define COOP_TYPE_PING 3
#define COOP_TYPE_PONG 4
#define COOP_TYPE_POSITION 5
#define COOP_TYPE_RELIABLE 6
#define COOP_TYPE_ACK 7
#define COOP_RELIABLE_QUEUE_CAPACITY 32
#define COOP_RETRANSMIT_MS 250
#define COOP_HELLO_INTERVAL_MS 500
#define COOP_PING_INTERVAL_MS 1000
#define COOP_TIMEOUT_MS 6000
#define COOP_CONNECT_TIMEOUT_MS 12000

static int sSocket = -1;
static enum CoopNetState sState = COOP_NET_DISCONNECTED;
static struct sockaddr_in sPeer;
static bool8 sHavePeer;
static u32 sSequence;
static u32 sLastReceive;
static u32 sLastSend;
static u32 sConnectStarted;
static struct CoopNetPosition sRemotePosition;

struct CoopReliableOut
{
    u32 sequence;
    u8 messageType;
    u16 payloadSize;
    u8 payload[COOP_NET_MESSAGE_MAX];
    u32 lastSent;
};

struct CoopReliableIn
{
    u8 messageType;
    u16 payloadSize;
    u8 payload[COOP_NET_MESSAGE_MAX];
};

static struct CoopReliableOut sOutbox[COOP_RELIABLE_QUEUE_CAPACITY];
static struct CoopReliableIn sInbox[COOP_RELIABLE_QUEUE_CAPACITY];
static u8 sOutboxCount;
static u8 sInboxHead;
static u8 sInboxCount;
static u32 sReliableSendSequence;
static u32 sReliableReceiveSequence;

static void AcknowledgeReliable(u32 acknowledgement)
{
    u8 removed = 0;
    while (removed < sOutboxCount
        && (s32)(acknowledgement - sOutbox[removed].sequence) >= 0)
        removed++;
    if (removed != 0)
    {
        u8 i;
        for (i = removed; i < sOutboxCount; i++)
            sOutbox[i - removed] = sOutbox[i];
        sOutboxCount -= removed;
    }
}

static u32 NowMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u32)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static void WriteU16(u8 *p, u16 value)
{
    p[0] = (u8)(value >> 8);
    p[1] = (u8)value;
}

static void WriteU32(u8 *p, u32 value)
{
    p[0] = (u8)(value >> 24);
    p[1] = (u8)(value >> 16);
    p[2] = (u8)(value >> 8);
    p[3] = (u8)value;
}

static u16 ReadU16(const u8 *p)
{
    return (u16)((p[0] << 8) | p[1]);
}

static u32 ReadU32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static bool8 SendPacket(u8 type, const u8 *payload, u16 payloadSize)
{
    u8 packet[COOP_PACKET_MAX];
    u32 sequence;
    ssize_t sent;

    if (sSocket < 0 || !sHavePeer || payloadSize > COOP_PACKET_MAX - COOP_HEADER_SIZE)
        return FALSE;

    sequence = ++sSequence;
    WriteU32(packet, COOP_MAGIC);
    packet[4] = COOP_PROTOCOL;
    packet[5] = type;
    WriteU16(packet + 6, payloadSize);
    WriteU32(packet + 8, sequence);
    WriteU32(packet + 12, sReliableReceiveSequence);
    if (payloadSize != 0)
        memcpy(packet + COOP_HEADER_SIZE, payload, payloadSize);

    sent = sendto(sSocket, packet, COOP_HEADER_SIZE + payloadSize, 0,
                  (const struct sockaddr *)&sPeer, sizeof(sPeer));
    if (sent != COOP_HEADER_SIZE + payloadSize)
        return FALSE;
    sLastSend = NowMs();
    return TRUE;
}

static bool8 OpenSocket(u16 port, bool8 bindSocket)
{
    int flags;
    int reuse = 1;

    sSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sSocket < 0)
        return FALSE;
    setsockopt(sSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    flags = fcntl(sSocket, F_GETFL, 0);
    if (flags < 0 || fcntl(sSocket, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        close(sSocket);
        sSocket = -1;
        return FALSE;
    }

    if (bindSocket)
    {
        struct sockaddr_in local;
        memset(&local, 0, sizeof(local));
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = htons(port);
        if (bind(sSocket, (struct sockaddr *)&local, sizeof(local)) < 0)
        {
            close(sSocket);
            sSocket = -1;
            return FALSE;
        }
    }
    return TRUE;
}

bool8 CoopNet_Host(u16 port)
{
    CoopNet_Stop();
    if (!OpenSocket(port, TRUE))
        return FALSE;
    sHavePeer = FALSE;
    sState = COOP_NET_LISTENING;
    sLastReceive = NowMs();
    sConnectStarted = 0;
    return TRUE;
}

bool8 CoopNet_Join(const char *ipv4, u16 port)
{
    CoopNet_Stop();
    if (!ipv4 || !OpenSocket(0, FALSE))
        return FALSE;

    memset(&sPeer, 0, sizeof(sPeer));
    sPeer.sin_family = AF_INET;
    sPeer.sin_port = htons(port);
    if (inet_pton(AF_INET, ipv4, &sPeer.sin_addr) != 1)
    {
        CoopNet_Stop();
        return FALSE;
    }

    sHavePeer = TRUE;
    sState = COOP_NET_CONNECTING;
    sLastReceive = NowMs();
    sLastSend = 0;
    sConnectStarted = sLastReceive;
    return TRUE;
}

void CoopNet_Stop(void)
{
    if (sSocket >= 0)
        close(sSocket);
    sSocket = -1;
    sState = COOP_NET_DISCONNECTED;
    sHavePeer = FALSE;
    sConnectStarted = 0;
    sSequence = 0;
    sReliableSendSequence = 0;
    sReliableReceiveSequence = 0;
    sOutboxCount = 0;
    sInboxHead = 0;
    sInboxCount = 0;
    memset(sOutbox, 0, sizeof(sOutbox));
    memset(sInbox, 0, sizeof(sInbox));
    memset(&sPeer, 0, sizeof(sPeer));
    memset(&sRemotePosition, 0, sizeof(sRemotePosition));
}

enum CoopNetState CoopNet_GetState(void)
{
    return sState;
}

bool8 CoopNet_IsConnected(void)
{
    return sState == COOP_NET_CONNECTED;
}

bool8 CoopNet_GetRemotePosition(struct CoopNetPosition *position)
{
    if (!position || !sRemotePosition.valid)
        return FALSE;
    *position = sRemotePosition;
    return TRUE;
}

bool8 CoopNet_SendPosition(u8 mapGroup, u8 mapNum, s16 x, s16 y, u8 facing, u8 elevation, u16 graphicsId)
{
    u8 payload[12];
    if (sState != COOP_NET_CONNECTED)
        return FALSE;
    payload[0] = mapGroup;
    payload[1] = mapNum;
    WriteU16(payload + 2, (u16)x);
    WriteU16(payload + 4, (u16)y);
    payload[6] = facing;
    payload[7] = elevation;
    WriteU16(payload + 8, graphicsId);
    payload[10] = 0;
    payload[11] = 0;
    return SendPacket(COOP_TYPE_POSITION, payload, sizeof(payload));
}

static bool8 SendReliableFrame(const struct CoopReliableOut *message)
{
    u8 payload[COOP_NET_MESSAGE_MAX + 1];
    u8 packet[COOP_PACKET_MAX];
    ssize_t sent;
    u16 payloadSize = message->payloadSize + 1;

    if (sSocket < 0 || !sHavePeer || payloadSize > COOP_PACKET_MAX - COOP_HEADER_SIZE)
        return FALSE;

    payload[0] = message->messageType;
    if (message->payloadSize != 0)
        memcpy(payload + 1, message->payload, message->payloadSize);

    WriteU32(packet, COOP_MAGIC);
    packet[4] = COOP_PROTOCOL;
    packet[5] = COOP_TYPE_RELIABLE;
    WriteU16(packet + 6, payloadSize);
    WriteU32(packet + 8, message->sequence);
    WriteU32(packet + 12, sReliableReceiveSequence);
    memcpy(packet + COOP_HEADER_SIZE, payload, payloadSize);

    sent = sendto(sSocket, packet, COOP_HEADER_SIZE + payloadSize, 0,
                  (const struct sockaddr *)&sPeer, sizeof(sPeer));
    return sent == COOP_HEADER_SIZE + payloadSize;
}

bool8 CoopNet_SendReliable(u8 messageType, const void *payload, u16 payloadSize)
{
    struct CoopReliableOut *message;

    if (sState != COOP_NET_CONNECTED || payloadSize > COOP_NET_MESSAGE_MAX
        || (payloadSize != 0 && payload == NULL)
        || sOutboxCount >= COOP_RELIABLE_QUEUE_CAPACITY)
        return FALSE;

    message = &sOutbox[sOutboxCount++];
    memset(message, 0, sizeof(*message));
    message->sequence = ++sReliableSendSequence;
    message->messageType = messageType;
    message->payloadSize = payloadSize;
    if (payloadSize != 0)
        memcpy(message->payload, payload, payloadSize);

    if (SendReliableFrame(message))
        message->lastSent = NowMs();
    return TRUE;
}

bool8 CoopNet_PopMessage(u8 *messageType, void *payload, u16 *payloadSize)
{
    struct CoopReliableIn *message;
    if (sInboxCount == 0 || !messageType || !payloadSize)
        return FALSE;

    message = &sInbox[sInboxHead];
    if (*payloadSize < message->payloadSize || (message->payloadSize != 0 && !payload))
        return FALSE;

    *messageType = message->messageType;
    *payloadSize = message->payloadSize;
    if (message->payloadSize != 0)
        memcpy(payload, message->payload, message->payloadSize);

    sInboxHead = (sInboxHead + 1) % COOP_RELIABLE_QUEUE_CAPACITY;
    sInboxCount--;
    return TRUE;
}

static void FlushReliableOutbox(void)
{
    u8 i;
    u32 now = NowMs();
    for (i = 0; i < sOutboxCount; i++)
    {
        if (sOutbox[i].lastSent == 0 || now - sOutbox[i].lastSent >= COOP_RETRANSMIT_MS)
        {
            if (SendReliableFrame(&sOutbox[i]))
                sOutbox[i].lastSent = now;
        }
    }
}

static bool8 SamePeer(const struct sockaddr_in *from)
{
    return from->sin_family == sPeer.sin_family
        && from->sin_port == sPeer.sin_port
        && from->sin_addr.s_addr == sPeer.sin_addr.s_addr;
}

static void HandlePacket(const u8 *packet, size_t size, const struct sockaddr_in *from)
{
    u16 payloadSize;
    u8 type;
    u32 now = NowMs();

    if (size < COOP_HEADER_SIZE || ReadU32(packet) != COOP_MAGIC
        || packet[4] != COOP_PROTOCOL)
        return;
    payloadSize = ReadU16(packet + 6);
    if (payloadSize != size - COOP_HEADER_SIZE)
        return;
    type = packet[5];

    if (sState == COOP_NET_LISTENING && type == COOP_TYPE_HELLO)
    {
        sPeer = *from;
        sHavePeer = TRUE;
        sState = COOP_NET_CONNECTED;
        sLastReceive = now;
        SendPacket(COOP_TYPE_WELCOME, NULL, 0);
        return;
    }

    if (!sHavePeer || !SamePeer(from))
        return;

    sLastReceive = now;
    AcknowledgeReliable(ReadU32(packet + 12));
    if (type == COOP_TYPE_RELIABLE && payloadSize >= 1)
    {
        u32 sequence = ReadU32(packet + 8);
        if (sequence == sReliableReceiveSequence + 1
            && sInboxCount < COOP_RELIABLE_QUEUE_CAPACITY
            && payloadSize - 1 <= COOP_NET_MESSAGE_MAX)
        {
            u8 tail = (sInboxHead + sInboxCount) % COOP_RELIABLE_QUEUE_CAPACITY;
            struct CoopReliableIn *message = &sInbox[tail];
            message->messageType = packet[COOP_HEADER_SIZE];
            message->payloadSize = payloadSize - 1;
            if (message->payloadSize != 0)
                memcpy(message->payload, packet + COOP_HEADER_SIZE + 1, message->payloadSize);
            sReliableReceiveSequence = sequence;
            sInboxCount++;
        }
        /* ACK duplicates and the last contiguous message. A gap is not
         * acknowledged; the sender will retransmit the missing sequence. */
        SendPacket(COOP_TYPE_ACK, NULL, 0);
    }
    else if (type == COOP_TYPE_ACK)
    {
        /* The cumulative acknowledgement was applied above. */
    }
    else if (type == COOP_TYPE_HELLO && sState == COOP_NET_CONNECTED)
    {
        SendPacket(COOP_TYPE_WELCOME, NULL, 0);
    }
    else if (type == COOP_TYPE_WELCOME && sState == COOP_NET_CONNECTING)
    {
        sState = COOP_NET_CONNECTED;
    }
    else if (type == COOP_TYPE_PING)
    {
        SendPacket(COOP_TYPE_PONG, NULL, 0);
    }
    else if (type == COOP_TYPE_POSITION && payloadSize == 12)
    {
        sRemotePosition.valid = TRUE;
        sRemotePosition.mapGroup = packet[COOP_HEADER_SIZE];
        sRemotePosition.mapNum = packet[COOP_HEADER_SIZE + 1];
        sRemotePosition.x = (s16)ReadU16(packet + COOP_HEADER_SIZE + 2);
        sRemotePosition.y = (s16)ReadU16(packet + COOP_HEADER_SIZE + 4);
        sRemotePosition.facing = packet[COOP_HEADER_SIZE + 6];
        sRemotePosition.elevation = packet[COOP_HEADER_SIZE + 7];
        sRemotePosition.graphicsId = ReadU16(packet + COOP_HEADER_SIZE + 8);
        sRemotePosition.receivedAtMs = now;
    }
}

void CoopNet_Poll(void)
{
    u8 packet[COOP_PACKET_MAX];
    struct sockaddr_in from;
    socklen_t fromSize;
    ssize_t received;
    u32 now;

    if (sSocket < 0)
        return;

    for (;;)
    {
        fromSize = sizeof(from);
        received = recvfrom(sSocket, packet, sizeof(packet), 0,
                            (struct sockaddr *)&from, &fromSize);
        if (received < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                break;
            CoopNet_Stop();
            return;
        }
        HandlePacket(packet, (size_t)received, &from);
    }

    now = NowMs();
    if (sState == COOP_NET_CONNECTING && now - sConnectStarted > COOP_CONNECT_TIMEOUT_MS)
    {
        CoopNet_Stop();
        return;
    }
    if (sState == COOP_NET_CONNECTED)
        FlushReliableOutbox();

    if (sState == COOP_NET_CONNECTING && (now - sLastSend >= COOP_HELLO_INTERVAL_MS))
        SendPacket(COOP_TYPE_HELLO, NULL, 0);
    else if (sState == COOP_NET_CONNECTED)
    {
        if (now - sLastReceive > COOP_TIMEOUT_MS)
        {
            if (sState == COOP_NET_CONNECTED)
                CoopNet_Stop();
            return;
        }
        if (now - sLastSend >= COOP_PING_INTERVAL_MS)
            SendPacket(COOP_TYPE_PING, NULL, 0);
    }
}

#else

bool8 CoopNet_Host(u16 port) { (void)port; return FALSE; }
bool8 CoopNet_Join(const char *ipv4, u16 port) { (void)ipv4; (void)port; return FALSE; }
void CoopNet_Stop(void) {}
void CoopNet_Poll(void) {}
enum CoopNetState CoopNet_GetState(void) { return COOP_NET_DISCONNECTED; }
bool8 CoopNet_IsConnected(void) { return FALSE; }
bool8 CoopNet_SendPosition(u8 mapGroup, u8 mapNum, s16 x, s16 y, u8 facing, u8 elevation, u16 graphicsId)
{ (void)mapGroup; (void)mapNum; (void)x; (void)y; (void)facing; (void)elevation; (void)graphicsId; return FALSE; }
bool8 CoopNet_GetRemotePosition(struct CoopNetPosition *position) { (void)position; return FALSE; }
bool8 CoopNet_SendReliable(u8 messageType, const void *payload, u16 payloadSize)
{ (void)messageType; (void)payload; (void)payloadSize; return FALSE; }
bool8 CoopNet_PopMessage(u8 *messageType, void *payload, u16 *payloadSize)
{ (void)messageType; (void)payload; (void)payloadSize; return FALSE; }

#endif

#if defined(__ANDROID__)
#include <jni.h>

JNIEXPORT jboolean JNICALL
Java_com_heartsoul_recomp_HeartSoulActivity_nativeCoopHost(JNIEnv *env, jclass clazz, jint port)
{
    (void)env;
    (void)clazz;
    return CoopNet_Host((u16)port) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_heartsoul_recomp_HeartSoulActivity_nativeCoopJoin(JNIEnv *env, jclass clazz, jstring address, jint port)
{
    const char *ipv4;
    jboolean result;
    (void)clazz;

    if (address == NULL)
        return JNI_FALSE;
    ipv4 = (*env)->GetStringUTFChars(env, address, NULL);
    if (ipv4 == NULL)
        return JNI_FALSE;
    result = CoopNet_Join(ipv4, (u16)port) ? JNI_TRUE : JNI_FALSE;
    (*env)->ReleaseStringUTFChars(env, address, ipv4);
    return result;
}

JNIEXPORT jint JNICALL
Java_com_heartsoul_recomp_HeartSoulActivity_nativeCoopState(JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;
    return (jint)CoopNet_GetState();
}

JNIEXPORT void JNICALL
Java_com_heartsoul_recomp_HeartSoulActivity_nativeCoopStop(JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;
    CoopNet_Stop();
}
#endif
