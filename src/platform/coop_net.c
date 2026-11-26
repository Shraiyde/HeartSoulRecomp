#ifdef PORTABLE
#if defined(__linux__)

#define _GNU_SOURCE
#include "platform/coop_net.h"
#include "platform/coop_aes.h"
#include "platform/coop_secret.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define COOP_PEER_MAGIC 0x4D504C4Bu
#define COOP_PEER_VER   1
#define COOP_PHONE_PROTOCOL 4
#define COOP_MAX_FRAME  8192
#define COOP_QUEUE      48
#define COOP_RELIABLE_OUTBOX 2048
#define COOP_PAY_MAX    96
#define COOP_MSG_ACK     0xFFFFu

struct CoopQueued
{
    uint16_t type;
    uint16_t len;
    uint32_t seq;
    uint8_t bytes[COOP_PAY_MAX];
};

static pthread_t sThread;
static pthread_mutex_t sMu = PTHREAD_MUTEX_INITIALIZER;
static bool sThreadStarted;
static bool sWant = true;
static bool sLinkUp;
static bool sEverUp;
static bool sResumeSpent;
static int sRole = -1;
static uint64_t sSendCounter = 1;
static uint64_t sRecvCounter;
static uint32_t sPosition[COOP_POSITION_WORDS];
static bool sHavePosition;
static struct CoopRemoteView sRemote;
static uint64_t sRemoteAtMs;
static struct CoopQueued sOut[COOP_QUEUE];
static struct CoopQueued sPending[COOP_RELIABLE_OUTBOX];
static int sPendingRead, sPendingWrite, sPendingCount;
static uint32_t sReliableTxSeq, sReliableRxSeq;
static bool sAckDirty;
static int sOutRead;
static int sOutWrite;
static int sOutCount;
static struct CoopQueued sIn[COOP_QUEUE];
static int sInRead;
static int sInWrite;
static int sInCount;
static uint8_t sSessionKey[32];
static bool sPcSession;
static bool sBackground;
static bool sForceFlush;
static bool sHaveKey;
static char sClientId[20];
static char sPeerClientId[20];
static bool sHavePeerClientId;
static char sSessionId[20];
static char sPeerSessionId[20];
static bool sHavePeerSessionId;

static uint64_t CoopNowMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void StoreBe32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t LoadBe32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void StoreBe64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 7; i >= 0; i--)
    {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

static uint64_t LoadBe64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

static bool QueuePush(struct CoopQueued *q, int *read, int *write, int *count, const struct CoopQueued *item)
{
    if (*count >= COOP_QUEUE)
        return false;
    q[*write] = *item;
    *write = (*write + 1) % COOP_QUEUE;
    (*count)++;
    (void)read;
    return true;
}

static bool QueuePop(struct CoopQueued *q, int *read, int *write, int *count, struct CoopQueued *item)
{
    if (*count <= 0)
        return false;
    *item = q[*read];
    *read = (*read + 1) % COOP_QUEUE;
    (*count)--;
    (void)write;
    return true;
}

static bool SendAll(int fd, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len)
    {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                struct pollfd pfd;
                pfd.fd = fd;
                pfd.events = POLLOUT;
                pfd.revents = 0;
                if (poll(&pfd, 1, 2000) <= 0)
                    return false;
                continue;
            }
            return false;
        }
        if (n == 0)
            return false;
        off += (size_t)n;
    }
    return true;
}

static bool RecvSome(int fd, uint8_t *buf, size_t cap, size_t *have, int waitMs)
{
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, waitMs) < 0)
    {
        if (errno == EINTR)
            return true;
        return false;
    }
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
        return false;
    if (!(pfd.revents & POLLIN))
        return true;
    if (*have >= cap)
        return false;
    {
        ssize_t n = recv(fd, buf + *have, cap - *have, 0);
        if (n < 0)
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                return true;
            return false;
        }
        if (n == 0)
            return false;
        *have += (size_t)n;
    }
    return true;
}

static bool EncryptAndSend(int fd, int sender, const uint8_t *plain, size_t plainLen)
{
    uint8_t nonce[12];
    uint8_t cipher[COOP_MAX_FRAME];
    uint8_t tag[16];
    uint8_t packet[12 + COOP_MAX_FRAME];
    uint8_t header[4];
    uint64_t counter;
    size_t wire;

    if (plainLen > COOP_MAX_FRAME - 16 || !sHaveKey)
        return false;
    pthread_mutex_lock(&sMu);
    counter = sSendCounter++;
    pthread_mutex_unlock(&sMu);
    StoreBe32(nonce, (uint32_t)sender);
    StoreBe64(nonce + 4, counter);
    if (CoopAesGcmEncrypt(sSessionKey, nonce, plain, plainLen, cipher, tag) != 0)
        return false;
    memcpy(packet, nonce, 12);
    memcpy(packet + 12, cipher, plainLen);
    memcpy(packet + 12 + plainLen, tag, 16);
    wire = 12 + plainLen + 16;
    StoreBe32(header, (uint32_t)wire);
    if (!SendAll(fd, header, 4))
        return false;
    return SendAll(fd, packet, wire);
}

static bool BuildAndSend(int fd, int sender, uint16_t type, const uint8_t *payload, uint32_t len, uint32_t seq)
{
    uint8_t plain[16 + COOP_PAY_MAX];
    if (len > COOP_PAY_MAX)
        return false;
    StoreBe32(plain, COOP_PEER_MAGIC);
    plain[4] = 0;
    plain[5] = COOP_PEER_VER;
    plain[6] = (uint8_t)(type >> 8);
    plain[7] = (uint8_t)type;
    StoreBe32(plain + 8, len);
    StoreBe32(plain + 12, seq);
    if (len > 0)
        memcpy(plain + 16, payload, len);
    return EncryptAndSend(fd, sender, plain, 16 + len);
}

static void NoteLink(bool up)
{
    pthread_mutex_lock(&sMu);
    sLinkUp = up;
    if (!up)
    {
        sRemote.valid = false;
        sRemoteAtMs = 0;
    }
    pthread_mutex_unlock(&sMu);
}

static void HandlePlain(const uint8_t *plain, size_t len, uint32_t seq)
{
    uint32_t magic, payLen;
    uint16_t ver, type;
    const uint8_t *payload;

    if (len < 16)
        return;
    magic = LoadBe32(plain);
    ver = (uint16_t)((plain[4] << 8) | plain[5]);
    type = (uint16_t)((plain[6] << 8) | plain[7]);
    payLen = LoadBe32(plain + 8);
    if (magic != COOP_PEER_MAGIC || ver != COOP_PEER_VER)
        return;
    if (payLen > 4096 || len != 16 + payLen)
        return;
    payload = plain + 16;
    if (!sPcSession && type == COOP_MSG_ACK)
    {
        uint32_t ack;
        if (payLen != 4) return;
        ack = LoadBe32(payload);
        pthread_mutex_lock(&sMu);
        if (ack > sReliableTxSeq)
        {
            pthread_mutex_unlock(&sMu);
            return;
        }
        while (sPendingCount > 0 && sPending[sPendingRead].seq <= ack)
        {
            sPendingRead = (sPendingRead + 1) % COOP_RELIABLE_OUTBOX;
            sPendingCount--;
        }
        pthread_mutex_unlock(&sMu);
        return;
    }
    if (type == COOP_MSG_POSITION && payLen >= 16 && (payLen % 4) == 0)
    {
        uint32_t words[COOP_POSITION_WORDS];
        uint32_t n = payLen / 4;
        uint32_t i;
        if (n > COOP_POSITION_WORDS)
            n = COOP_POSITION_WORDS;
        memset(words, 0, sizeof(words));
        for (i = 0; i < n; i++)
            words[i] = LoadBe32(payload + i * 4);
        pthread_mutex_lock(&sMu);
        sRemote.valid = true;
        sRemote.mapId = words[0];
        sRemote.sio = words[1];
        sRemote.x = words[2];
        sRemote.y = words[3];
        if (n >= 6)
        {
            sRemote.gender = words[4];
            sRemote.facing = words[5];
        }
        if (n >= 7)
            sRemote.follower = words[6];
        if (n >= 8)
            sRemote.avatar = words[7];
        sRemoteAtMs = CoopNowMs();
        pthread_mutex_unlock(&sMu);
        return;
    }
    if (payLen > COOP_PAY_MAX)
        return;
    {
        struct CoopQueued item;
        if (!sPcSession)
        {
            if (seq == 0) return;
            pthread_mutex_lock(&sMu);
            if (seq <= sReliableRxSeq)
            {
                sAckDirty = true;
                pthread_mutex_unlock(&sMu);
                return;
            }
            if (seq != sReliableRxSeq + 1)
            {
                pthread_mutex_unlock(&sMu);
                return;
            }
            pthread_mutex_unlock(&sMu);
        }
        memset(&item, 0, sizeof(item));
        item.type = type;
        item.len = (uint16_t)payLen;
        if (payLen == 4)
        {
            uint32_t word = LoadBe32(payload);
            StoreBe32(item.bytes, word); /* kept big-endian; Poll converts */
            memcpy(item.bytes, payload, 4);
        }
        else if (payLen > 0)
        {
            memcpy(item.bytes, payload, payLen);
        }
        pthread_mutex_lock(&sMu);
        if (QueuePush(sIn, &sInRead, &sInWrite, &sInCount, &item))
        {
            if (!sPcSession)
            {
                sReliableRxSeq = seq;
                sAckDirty = true;
            }
        }
        else if (sPcSession)
        {
            struct CoopQueued junk;
            QueuePop(sIn, &sInRead, &sInWrite, &sInCount, &junk);
            QueuePush(sIn, &sInRead, &sInWrite, &sInCount, &item);
        }
        pthread_mutex_unlock(&sMu);
    }
}

static bool DecodeEncrypted(const uint8_t *wire, size_t wireLen, int expectedSender)
{
    uint8_t plain[COOP_MAX_FRAME];
    uint32_t sender;
    uint64_t counter;
    size_t cipherLen;
    const uint8_t *tag;

    if (wireLen < 12 + 16)
        return true;
    sender = LoadBe32(wire);
    counter = LoadBe64(wire + 4);
    if (sender != (uint32_t)expectedSender)
        return true;
    pthread_mutex_lock(&sMu);
    if (counter <= sRecvCounter)
    {
        pthread_mutex_unlock(&sMu);
        return true;
    }
    pthread_mutex_unlock(&sMu);
    cipherLen = wireLen - 12 - 16;
    tag = wire + 12 + cipherLen;
    if (CoopAesGcmDecrypt(sSessionKey, wire, wire + 12, cipherLen, tag, plain) != 0)
        return true;
    pthread_mutex_lock(&sMu);
    if (counter > sRecvCounter)
        sRecvCounter = counter;
    pthread_mutex_unlock(&sMu);
    HandlePlain(plain, cipherLen, LoadBe32(plain + 12));
    return true;
}

static void FlushOutbound(int fd, int sender, uint32_t *seq)
{
    uint32_t position[COOP_POSITION_WORDS];
    bool have = false;
    int guard;

    pthread_mutex_lock(&sMu);
    if (sHavePosition)
    {
        memcpy(position, sPosition, sizeof(position));
        have = true;
    }
    pthread_mutex_unlock(&sMu);
    if (have)
    {
        uint8_t raw[COOP_POSITION_WORDS * 4];
        int i;
        for (i = 0; i < COOP_POSITION_WORDS; i++)
            StoreBe32(raw + i * 4, position[i]);
        if (!BuildAndSend(fd, sender, COOP_MSG_POSITION, raw, sizeof(raw), ++(*seq)))
            return;
    }
    for (guard = 0; guard < 8; guard++)
    {
        struct CoopQueued item;
        bool got;
        pthread_mutex_lock(&sMu);
        if (!sPcSession && sPendingCount >= COOP_RELIABLE_OUTBOX)
        {
            pthread_mutex_unlock(&sMu);
            break;
        }
        got = QueuePop(sOut, &sOutRead, &sOutWrite, &sOutCount, &item);
        pthread_mutex_unlock(&sMu);
        if (!got)
            break;
        if (sPcSession)
        {
            if (!BuildAndSend(fd, sender, item.type, item.bytes, item.len, ++(*seq)))
            {
                pthread_mutex_lock(&sMu);
                QueuePush(sOut, &sOutRead, &sOutWrite, &sOutCount, &item);
                pthread_mutex_unlock(&sMu);
                break;
            }
            continue;
        }
        pthread_mutex_lock(&sMu);
        item.seq = ++sReliableTxSeq;
        pthread_mutex_unlock(&sMu);
        if (!BuildAndSend(fd, sender, item.type, item.bytes, item.len, item.seq))
        {
            pthread_mutex_lock(&sMu);
            sReliableTxSeq--;
            item.seq = 0;
            QueuePush(sOut, &sOutRead, &sOutWrite, &sOutCount, &item);
            pthread_mutex_unlock(&sMu);
            break;
        }
        pthread_mutex_lock(&sMu);
        sPending[sPendingWrite] = item;
        sPendingWrite = (sPendingWrite + 1) % COOP_RELIABLE_OUTBOX;
        sPendingCount++;
        pthread_mutex_unlock(&sMu);
    }
}

static bool ReadLine(int fd, char *out, size_t cap)
{
    size_t n = 0;
    uint64_t start = CoopNowMs();
    while (n + 1 < cap)
    {
        uint8_t b;
        ssize_t r;
        if (CoopNowMs() - start > 8000)
            return false;
        r = recv(fd, &b, 1, 0);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                struct pollfd pfd;
                pfd.fd = fd;
                pfd.events = POLLIN;
                pfd.revents = 0;
                if (poll(&pfd, 1, 200) < 0 && errno != EINTR)
                    return false;
                continue;
            }
            return false;
        }
        if (r == 0)
            return false;
        if (b == '\n')
        {
            out[n] = '\0';
            return true;
        }
        if (b != '\r')
            out[n++] = (char)b;
    }
    return false;
}

static int TcpConnect(const char *host, int port)
{
    int fd;
    struct sockaddr_in addr;
    int flags;
    int on = 1;
    int rc;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
        return -1;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
#ifdef TCP_USER_TIMEOUT
    {
        int ms = 20000;
        setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &ms, sizeof(ms));
    }
#endif
    flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS)
    {
        close(fd);
        return -1;
    }
    if (rc < 0)
    {
        struct pollfd pfd;
        int err = 0;
        socklen_t elen = sizeof(err);
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        if (poll(&pfd, 1, 8000) <= 0)
        {
            close(fd);
            return -1;
        }
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0)
        {
            close(fd);
            return -1;
        }
    }
    return fd;
}

static int TcpListen(int port)
{
    int fd;
    int on = 1;
    struct sockaddr_in addr;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    /* The embedded ZeroTier bridge forwards peers into this loopback socket. */
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 1) < 0)
    {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}

static void LoadOrCreateClientId(void)
{
    FILE *f = fopen("coop.cid", "r");
    if (f != NULL)
    {
        if (fgets(sClientId, sizeof(sClientId), f) != NULL)
        {
            size_t n = strcspn(sClientId, "\r\n");
            sClientId[n] = '\0';
        }
        fclose(f);
    }
    if (strlen(sClientId) < 8)
    {
        uint8_t rnd[8];
        int i;
        FILE *u = fopen("/dev/urandom", "rb");
        memset(rnd, 0, sizeof(rnd));
        if (u != NULL)
        {
            if (fread(rnd, 1, sizeof(rnd), u) != sizeof(rnd))
                memset(rnd, 0x5a, sizeof(rnd));
            fclose(u);
        }
        for (i = 0; i < 8; i++)
            sprintf(sClientId + i * 2, "%02x", rnd[i]);
        f = fopen("coop.cid", "w");
        if (f != NULL)
        {
            fputs(sClientId, f);
            fputc('\n', f);
            fclose(f);
        }
    }
}

static bool PrepareKey(void)
{
    uint8_t secret[32];
    CoopSecretShared(secret);
    if (sPcSession)
        CoopHmacSha256(secret, sizeof(secret), (const uint8_t *)"HNS-ZT-SESSION-V1", 16, sSessionKey);
    else
        CoopHmacSha256(secret, sizeof(secret), (const uint8_t *)"HNS-DIRECT-SESSION-V1", 21, sSessionKey);
    memset(secret, 0, sizeof(secret));
    sHaveKey = true;
    return true;
}

static bool HelloProofForId(const char *clientId, char *out, size_t cap)
{
    uint8_t secret[32];
    uint8_t mac[32];
    char msg[64];
    int n;
    CoopSecretShared(secret);
    n = snprintf(msg, sizeof(msg), "hello|%s", clientId);
    if (n <= 0 || (size_t)n >= sizeof(msg))
    {
        memset(secret, 0, sizeof(secret));
        return false;
    }
    CoopHmacSha256(secret, sizeof(secret), (const uint8_t *)msg, (size_t)n, mac);
    memset(secret, 0, sizeof(secret));
    memset(msg, 0, sizeof(msg));
    return CoopBase64Encode(mac, 32, out, cap) > 0;
}

/* Both devices run the same encrypted, framed peer protocol. The phone that
 * opened the listening socket is role 0; the joining phone is role 1. */
static uint64_t RandomCounter(void)
{
    uint64_t value = 0;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f != NULL)
    {
        if (fread(&value, 1, sizeof(value), f) != sizeof(value))
            value = 0;
        fclose(f);
    }
    if (value == 0)
        value = (CoopNowMs() << 16) ^ (uint64_t)getpid();
    return value | 1ull;
}

static bool HelloProofForSession(const char *kind, const char *clientId,
                                  const char *sessionId, const char *peerSessionId,
                                  uint32_t receivedSeq, char *out, size_t cap)
{
    uint8_t secret[32];
    uint8_t mac[32];
    char msg[128];
    int n;
    CoopSecretShared(secret);
    n = snprintf(msg, sizeof(msg), "%s|%s|%s|%s|%u",
                 kind, clientId, sessionId, peerSessionId, receivedSeq);
    if (n <= 0 || (size_t)n >= sizeof(msg))
    {
        memset(secret, 0, sizeof(secret));
        return false;
    }
    CoopHmacSha256(secret, sizeof(secret), (const uint8_t *)msg, (size_t)n, mac);
    memset(secret, 0, sizeof(secret));
    memset(msg, 0, sizeof(msg));
    return CoopBase64Encode(mac, 32, out, cap) > 0;
}

static bool VerifyHello(const char *line, char *clientIdOut, size_t clientIdCap,
                        char *sessionIdOut, size_t sessionIdCap,
                        char *peerSessionIdOut, size_t peerSessionIdCap,
                        uint32_t *receivedSeqOut)
{
    int version = 0;
    unsigned int receivedSeq = 0;
    char clientId[20], sessionId[20], peerSessionId[20], proof[80], expected[80];
    if (sscanf(line, "HELLO %d %19s %19s %19s %u %79s",
               &version, clientId, sessionId, peerSessionId, &receivedSeq, proof) != 6
        || version != COOP_PHONE_PROTOCOL)
        return false;
    if (strlen(clientId) < 8 || strlen(sessionId) < 8
        || (strcmp(peerSessionId, "-") != 0 && strlen(peerSessionId) < 8)
        || clientIdCap <= strlen(clientId)
        || sessionIdCap <= strlen(sessionId)
        || peerSessionIdCap <= strlen(peerSessionId)
        || !HelloProofForSession("hello", clientId, sessionId, peerSessionId,
                                 (uint32_t)receivedSeq, expected, sizeof(expected))
        || strcmp(proof, expected) != 0)
        return false;
    memcpy(clientIdOut, clientId, strlen(clientId) + 1);
    memcpy(sessionIdOut, sessionId, strlen(sessionId) + 1);
    memcpy(peerSessionIdOut, peerSessionId, strlen(peerSessionId) + 1);
    *receivedSeqOut = (uint32_t)receivedSeq;
    return true;
}

static void AckPending(uint32_t receivedSeq)
{
    pthread_mutex_lock(&sMu);
    if (receivedSeq <= sReliableTxSeq)
    {
        while (sPendingCount > 0 && sPending[sPendingRead].seq <= receivedSeq)
        {
            sPendingRead = (sPendingRead + 1) % COOP_RELIABLE_OUTBOX;
            sPendingCount--;
        }
    }
    pthread_mutex_unlock(&sMu);
}

static void ResetReliableSession(void)
{
    pthread_mutex_lock(&sMu);
    sReliableTxSeq = 0;
    sReliableRxSeq = 0;
    sPendingRead = sPendingWrite = sPendingCount = 0;
    sOutRead = sOutWrite = sOutCount = 0;
    sInRead = sInWrite = sInCount = 0;
    sAckDirty = false;
    pthread_mutex_unlock(&sMu);
}

static void CreateSessionId(void)
{
    uint64_t value = RandomCounter();
    snprintf(sSessionId, sizeof(sSessionId), "%016llx", (unsigned long long)value);
    sPeerSessionId[0] = '\0';
    sHavePeerSessionId = false;
}

static void RunSession(int fd, int localRole, bool isHost)
{
    char line[160];
    char proof[80];
    char hello[128];
    char peerId[20];
    uint8_t acc[COOP_MAX_FRAME + 8];
    size_t have = 0;
    uint32_t seq = 1;
    int remoteRole = sPcSession ? 2 : (localRole == 0 ? 1 : 0);
    uint64_t lastPos = 0;
    uint64_t lastAck = 0;
    uint64_t lastReplay = 0;

    if (isHost)
    {
        if (!ReadLine(fd, line, sizeof(line))
            || !VerifyHello(line, peerId, sizeof(peerId))
            || (sHavePeerClientId && strcmp(sPeerClientId, peerId) != 0))
        {
            fprintf(stderr, "co-op: rejected a different or incompatible phone peer\n");
            close(fd);
            return;
        }
        if (!sHavePeerClientId)
        {
            memcpy(sPeerClientId, peerId, strlen(peerId) + 1);
            sHavePeerClientId = true;
        }
        if (!HelloProofForId(sClientId, proof, sizeof(proof)))
        {
            close(fd);
            return;
        }
        snprintf(hello, sizeof(hello), "READY %d %s %s\n",
                 COOP_PHONE_PROTOCOL, sClientId, proof);
        memset(proof, 0, sizeof(proof));
        if (!SendAll(fd, (const uint8_t *)hello, strlen(hello)))
        {
            memset(hello, 0, sizeof(hello));
            close(fd);
            return;
        }
        memset(hello, 0, sizeof(hello));
    }
    else
    {
        if (!HelloProofForId(sClientId, proof, sizeof(proof)))
        {
            close(fd);
            return;
        }
        if (sPcSession)
            snprintf(hello, sizeof(hello), "HELLO %s %s\n", sClientId, proof);
        else
            snprintf(hello, sizeof(hello), "HELLO %d %s %s\n",
                     COOP_PHONE_PROTOCOL, sClientId, proof);
        memset(proof, 0, sizeof(proof));
        if (!SendAll(fd, (const uint8_t *)hello, strlen(hello)))
        {
            memset(hello, 0, sizeof(hello));
            close(fd);
            return;
        }
        memset(hello, 0, sizeof(hello));
        if (!ReadLine(fd, line, sizeof(line)))
        {
            fprintf(stderr, "co-op: host rejected the join\n");
            close(fd);
            return;
        }
        if (sPcSession)
        {
            if (strncmp(line, "FULL", 4) == 0)
            {
                fprintf(stderr, "co-op: PC host is full\n");
                sWant = false;
                close(fd);
                return;
            }
            if (strncmp(line, "READY ", 6) != 0)
            {
                fprintf(stderr, "co-op: PC host rejected the join\n");
                close(fd);
                return;
            }
            localRole = atoi(line + 6);
            if (localRole != 0 && localRole != 1)
                localRole = 0;
        }
        else
        {
            int version = 0;
            char hostId[20];
            char hostProof[80];
            char expected[80];
            if (sscanf(line, "READY %d %19s %79s", &version, hostId, hostProof) != 3
                || version != COOP_PHONE_PROTOCOL
                || strlen(hostId) < 8
                || !HelloProofForId(hostId, expected, sizeof(expected))
                || strcmp(hostProof, expected) != 0
                || (sHavePeerClientId && strcmp(sPeerClientId, hostId) != 0))
            {
                fprintf(stderr, "co-op: incompatible or changed phone host\n");
                close(fd);
                return;
            }
            if (!sHavePeerClientId)
            {
                memcpy(sPeerClientId, hostId, strlen(hostId) + 1);
                sHavePeerClientId = true;
            }
        }
    }

    pthread_mutex_lock(&sMu);
    sRole = localRole;
    sLinkUp = true;
    sEverUp = true;
    sSendCounter = RandomCounter();
    sRecvCounter = 0;
    pthread_mutex_unlock(&sMu);
    fprintf(stderr, "co-op: direct phone link up (role %d)\n", localRole);

    /* Replay unacknowledged events after reconnect; the receiver suppresses duplicates. */
    if (!sPcSession)
    {
        int i;
        pthread_mutex_lock(&sMu);
        for (i = 0; i < sPendingCount; i++)
        {
            int at = (sPendingRead + i) % COOP_RELIABLE_OUTBOX;
            struct CoopQueued item = sPending[at];
            pthread_mutex_unlock(&sMu);
            if (!BuildAndSend(fd, localRole, item.type, item.bytes, item.len, item.seq))
                goto down;
            pthread_mutex_lock(&sMu);
        }
        pthread_mutex_unlock(&sMu);
    }

    while (sWant)
    {
        uint64_t now;
        if (!RecvSome(fd, acc, sizeof(acc), &have, 200))
            break;
        while (have >= 4)
        {
            uint32_t len = LoadBe32(acc);
            if (len < 28 || len > COOP_MAX_FRAME)
            {
                have = 0;
                goto down;
            }
            if (have < 4 + len)
                break;
            DecodeEncrypted(acc + 4, len, remoteRole);
            memmove(acc, acc + 4 + len, have - 4 - len);
            have -= 4 + len;
        }
        now = CoopNowMs();
        if (!sPcSession && now - lastReplay >= 1000)
        {
            int i;
            pthread_mutex_lock(&sMu);
            for (i = 0; i < sPendingCount; i++)
            {
                int at = (sPendingRead + i) % COOP_RELIABLE_OUTBOX;
                struct CoopQueued item = sPending[at];
                pthread_mutex_unlock(&sMu);
                if (!BuildAndSend(fd, localRole, item.type, item.bytes, item.len, item.seq))
                    goto down;
                pthread_mutex_lock(&sMu);
            }
            pthread_mutex_unlock(&sMu);
            lastReplay = now;
        }
        if (!sPcSession)
        {
            uint8_t ackBytes[4];
            uint32_t ack = 0;
            bool sendAck = false;
            pthread_mutex_lock(&sMu);
            if (sAckDirty && now - lastAck >= 100)
            {
                ack = sReliableRxSeq;
                sAckDirty = false;
                sendAck = true;
            }
            pthread_mutex_unlock(&sMu);
            if (sendAck)
            {
                StoreBe32(ackBytes, ack);
                if (!BuildAndSend(fd, localRole, COOP_MSG_ACK, ackBytes, sizeof(ackBytes), 0))
                    goto down;
                lastAck = now;
            }
        }
        {
            bool force;
            bool background;
            pthread_mutex_lock(&sMu);
            force = sForceFlush;
            background = sBackground;
            if (force)
                sForceFlush = false;
            pthread_mutex_unlock(&sMu);
            if (force)
            {
#ifdef TCP_USER_TIMEOUT
                int userMs = background ? 120000 : 20000;
                setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &userMs, sizeof(userMs));
#endif
                FlushOutbound(fd, localRole, &seq);
                lastPos = now;
                continue;
            }
        }
        if (lastPos == 0 || now - lastPos >= 400)
        {
            FlushOutbound(fd, localRole, &seq);
            lastPos = now;
        }
        else
        {
            int guard;
            for (guard = 0; guard < 4; guard++)
            {
                struct CoopQueued item;
                bool got;
                pthread_mutex_lock(&sMu);
                if (!sPcSession && sPendingCount >= COOP_QUEUE)
                {
                    pthread_mutex_unlock(&sMu);
                    break;
                }
                got = QueuePop(sOut, &sOutRead, &sOutWrite, &sOutCount, &item);
                pthread_mutex_unlock(&sMu);
                if (!got)
                    break;
                if (sPcSession)
                {
                    if (!BuildAndSend(fd, localRole, item.type, item.bytes, item.len, ++seq))
                    {
                        pthread_mutex_lock(&sMu);
                        QueuePush(sOut, &sOutRead, &sOutWrite, &sOutCount, &item);
                        pthread_mutex_unlock(&sMu);
                        goto down;
                    }
                    continue;
                }
                pthread_mutex_lock(&sMu);
                item.seq = ++sReliableTxSeq;
                pthread_mutex_unlock(&sMu);
                if (!BuildAndSend(fd, localRole, item.type, item.bytes, item.len, item.seq))
                {
                    pthread_mutex_lock(&sMu);
                    sReliableTxSeq--;
                    item.seq = 0;
                    QueuePush(sOut, &sOutRead, &sOutWrite, &sOutCount, &item);
                    pthread_mutex_unlock(&sMu);
                    goto down;
                }
                pthread_mutex_lock(&sMu);
                sPending[sPendingWrite] = item;
                sPendingWrite = (sPendingWrite + 1) % COOP_RELIABLE_OUTBOX;
                sPendingCount++;
                pthread_mutex_unlock(&sMu);
            }
        }
    }
down:
    NoteLink(false);
    close(fd);
    fprintf(stderr, "co-op: direct phone link down\n");
}

static void *ThreadMain(void *unused)
{
    const char *mode = getenv("HNS_COOP_MODE");
    const char *address = getenv("HNS_COOP_ADDRESS");
    const char *portText = getenv("HNS_COOP_PORT");
    const char *pcJoin = getenv("HNS_COOP_JOIN");
    char pcHost[16];
    int port = portText != NULL && portText[0] != '\0' ? atoi(portText) : 54555;
    bool hostMode = mode != NULL && strcmp(mode, "host") == 0;
    bool joinMode = mode != NULL && strcmp(mode, "join") == 0;
    sPcSession = pcJoin != NULL && pcJoin[0] == '1' && !hostMode && !joinMode;
    if (sPcSession)
    {
        CoopSecretHost(pcHost);
        address = pcHost;
        port = CoopSecretPort();
    }
    (void)unused;
    signal(SIGPIPE, SIG_IGN);
    PrepareKey();
    LoadOrCreateClientId();

    if (port < 1 || port > 65535)
    {
        fprintf(stderr, "co-op: invalid TCP port\n");
        return NULL;
    }

    if (hostMode)
    {
        int listener = TcpListen(port);
        if (listener < 0)
        {
            fprintf(stderr, "co-op: could not listen on TCP port %d\n", port);
            return NULL;
        }
        fprintf(stderr, "co-op: hosting direct phone session on TCP port %d\n", port);
        while (sWant)
        {
            struct pollfd pfd;
            int fd;
            pfd.fd = listener;
            pfd.events = POLLIN;
            pfd.revents = 0;
            if (poll(&pfd, 1, 500) <= 0)
                continue;
            fd = accept(listener, NULL, NULL);
            if (fd < 0)
            {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                break;
            }
            {
                int flags = fcntl(fd, F_GETFL, 0);
                fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            }
            RunSession(fd, 0, true);
        }
        close(listener);
    }
    else
    {
        unsigned int attempt = 0;
        uint64_t reconnectStarted = 0;
        if (address == NULL || address[0] == '\0')
        {
            fprintf(stderr, "co-op: join address is missing\n");
            return NULL;
        }
        while (sWant)
        {
            int fd;
            unsigned int delayMs;
            if (sEverUp)
            {
                if (reconnectStarted == 0)
                    reconnectStarted = CoopNowMs();
                if (CoopNowMs() - reconnectStarted >= 60000)
                {
                    fprintf(stderr, "co-op: reconnect window expired after 60 seconds\n");
                    break;
                }
            }
            fd = TcpConnect(address, port);
            if (fd >= 0)
            {
                RunSession(fd, 1, false);
                if (!sWant || !sEverUp)
                    break;
            }
            else if (!sEverUp)
            {
                /* Initial dialing is bounded too; do not spin forever on a
                 * mistyped address or a host that is not running. */
                if (++attempt >= 12)
                {
                    fprintf(stderr, "co-op: host could not be reached after 12 attempts\n");
                    break;
                }
            }
            delayMs = 400u << (attempt > 4 ? 4 : attempt);
            if (delayMs > 8000u)
                delayMs = 8000u;
            usleep(delayMs * 1000u);
            if (attempt < 16)
                attempt++;
        }
    }
    NoteLink(false);
    return NULL;
}

void CoopNet_SetBackgrounded(bool background)
{
    pthread_mutex_lock(&sMu);
    sBackground = background;
    sForceFlush = true;
    pthread_mutex_unlock(&sMu);
    CoopNet_SendWord(COOP_MSG_WORLD_READY,
        background ? COOP_READY_BACKGROUND : COOP_READY_FOREGROUND);
}

bool CoopNet_IsBackgrounded(void)
{
    bool background;
    pthread_mutex_lock(&sMu);
    background = sBackground;
    pthread_mutex_unlock(&sMu);
    return background;
}

void CoopNet_Boot(void)
{
    const char *mode = getenv("HNS_COOP_MODE");
    const char *pcJoin = getenv("HNS_COOP_JOIN");
    bool phone = mode != NULL && (strcmp(mode, "host") == 0 || strcmp(mode, "join") == 0);
    bool pc = pcJoin != NULL && pcJoin[0] == '1';
    if (!phone && !pc)
        return;
    if (sThreadStarted)
        return;
    sWant = true;
    if (pthread_create(&sThread, NULL, ThreadMain, NULL) == 0)
    {
        sThreadStarted = true;
        pthread_detach(sThread);
    }
}

bool CoopNet_IsUp(void)
{
    bool up;
    pthread_mutex_lock(&sMu);
    up = sLinkUp;
    pthread_mutex_unlock(&sMu);
    return up;
}

int CoopNet_Role(void)
{
    int role;
    pthread_mutex_lock(&sMu);
    role = sRole;
    pthread_mutex_unlock(&sMu);
    return role;
}

uint32_t CoopNet_PartnerAgeMs(void)
{
    uint64_t at;
    bool valid;
    pthread_mutex_lock(&sMu);
    at = sRemoteAtMs;
    valid = sRemote.valid;
    pthread_mutex_unlock(&sMu);
    if (!valid || at == 0)
        return 0xffffffffu;
    {
        uint64_t now = CoopNowMs();
        if (now < at)
            return 0;
        if (now - at > 0xffffffffull)
            return 0xffffffffu;
        return (uint32_t)(now - at);
    }
}

void CoopNet_CopyRemote(struct CoopRemoteView *out)
{
    if (out == NULL)
        return;
    pthread_mutex_lock(&sMu);
    *out = sRemote;
    pthread_mutex_unlock(&sMu);
    out->ageMs = CoopNet_PartnerAgeMs();
}

void CoopNet_SetPosition(const uint32_t words[COOP_POSITION_WORDS])
{
    if (words == NULL)
        return;
    pthread_mutex_lock(&sMu);
    memcpy(sPosition, words, sizeof(sPosition));
    sHavePosition = true;
    pthread_mutex_unlock(&sMu);
}

bool CoopNet_SendWord(uint16_t type, uint32_t word)
{
    struct CoopQueued item;
    bool ok;
    memset(&item, 0, sizeof(item));
    item.type = type;
    item.len = 4;
    StoreBe32(item.bytes, word);
    pthread_mutex_lock(&sMu);
    ok = QueuePush(sOut, &sOutRead, &sOutWrite, &sOutCount, &item);
    pthread_mutex_unlock(&sMu);
    return ok;
}

bool CoopNet_SendBytes(uint16_t type, const uint8_t *bytes, uint32_t len)
{
    struct CoopQueued item;
    bool ok;
    if (bytes == NULL || len > COOP_PAY_MAX)
        return false;
    memset(&item, 0, sizeof(item));
    item.type = type;
    item.len = (uint16_t)len;
    memcpy(item.bytes, bytes, len);
    pthread_mutex_lock(&sMu);
    ok = QueuePush(sOut, &sOutRead, &sOutWrite, &sOutCount, &item);
    pthread_mutex_unlock(&sMu);
    return ok;
}

bool CoopNet_Poll(uint16_t *type, uint8_t *payload, uint32_t *len, uint32_t cap)
{
    struct CoopQueued item;
    bool got;
    if (type == NULL || len == NULL)
        return false;
    pthread_mutex_lock(&sMu);
    got = QueuePop(sIn, &sInRead, &sInWrite, &sInCount, &item);
    pthread_mutex_unlock(&sMu);
    if (!got)
        return false;
    *type = item.type;
    *len = item.len;
    if (payload != NULL && item.len > 0 && cap > 0)
    {
        uint32_t n = item.len;
        if (n > cap)
            n = cap;
        if (item.len == 4 && cap >= 4)
        {
            uint32_t word = LoadBe32(item.bytes);
            memcpy(payload, &word, 4);
            *len = 4;
        }
        else
        {
            memcpy(payload, item.bytes, n);
        }
    }
    return true;
}

#ifdef __ANDROID__
#include <jni.h>

JNIEXPORT void JNICALL
Java_com_heartsoul_recomp_HeartSoulActivity_nativeCoopBackground(JNIEnv *env, jobject thiz, jboolean background)
{
    (void)env;
    (void)thiz;
    CoopNet_SetBackgrounded(background == JNI_TRUE);
}
#endif

#else

#include "platform/coop_net.h"

#include <string.h>

void CoopNet_Boot(void) {}
void CoopNet_SetBackgrounded(bool background) { (void)background; }
bool CoopNet_IsBackgrounded(void) { return false; }
bool CoopNet_IsUp(void) { return false; }
int CoopNet_Role(void) { return -1; }
uint32_t CoopNet_PartnerAgeMs(void) { return 0xffffffffu; }
void CoopNet_CopyRemote(struct CoopRemoteView *out)
{
    if (out != NULL)
        memset(out, 0, sizeof(*out));
}
void CoopNet_SetPosition(const uint32_t words[COOP_POSITION_WORDS]) { (void)words; }
bool CoopNet_SendWord(uint16_t type, uint32_t word) { (void)type; (void)word; return false; }
bool CoopNet_SendBytes(uint16_t type, const uint8_t *bytes, uint32_t len)
{
    (void)type; (void)bytes; (void)len;
    return false;
}
bool CoopNet_Poll(uint16_t *type, uint8_t *payload, uint32_t *len, uint32_t cap)
{
    (void)type; (void)payload; (void)len; (void)cap;
    return false;
}

#endif
#else
typedef int CoopNetUnusedOnGba;
#endif
