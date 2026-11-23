#ifdef PORTABLE
#if defined(__linux__)

#define _GNU_SOURCE
#include "platform/coop_net.h"
#include "platform/coop_aes.h"
#include "platform/coop_host.h"
#include "platform/coop_secret.h"

#ifdef __ANDROID__
#include <jni.h>
#endif

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
#define COOP_MAX_FRAME  8192
#define COOP_QUEUE      48
#define COOP_PAY_MAX    96

struct CoopQueued
{
    uint16_t type;
    uint16_t len;
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
static int sOutRead;
static int sOutWrite;
static int sOutCount;
static struct CoopQueued sIn[COOP_QUEUE];
static int sInRead;
static int sInWrite;
static int sInCount;
static uint8_t sSessionKey[32];
static bool sHaveKey;
static char sClientId[20];
static int sPhoneLink;
static bool sBackground;
static volatile bool sForceFlush;
static int sSessionFd = -1;

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

static void HandlePlain(const uint8_t *plain, size_t len)
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
        if (!QueuePush(sIn, &sInRead, &sInWrite, &sInCount, &item))
        {
            /* Drop the oldest inbound word so a burst cannot stall the link. */
            struct CoopQueued junk;
            QueuePop(sIn, &sInRead, &sInWrite, &sInCount, &junk);
            QueuePush(sIn, &sInRead, &sInWrite, &sInCount, &item);
        }
        pthread_mutex_unlock(&sMu);
    }
}

static bool DecodeEncrypted(const uint8_t *wire, size_t wireLen)
{
    uint8_t nonce[12];
    uint8_t plain[COOP_MAX_FRAME];
    uint32_t sender;
    uint64_t counter;
    size_t cipherLen;
    const uint8_t *tag;

    if (wireLen < 12 + 16)
        return true;
    sender = LoadBe32(wire);
    counter = LoadBe64(wire + 4);
    /* Phones only apply frames the World Host stamped. */
    if (sender != 2)
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
    memcpy(nonce, wire, 12);
    (void)nonce;
    pthread_mutex_lock(&sMu);
    if (counter > sRecvCounter)
        sRecvCounter = counter;
    pthread_mutex_unlock(&sMu);
    HandlePlain(plain, cipherLen);
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
        got = QueuePop(sOut, &sOutRead, &sOutWrite, &sOutCount, &item);
        pthread_mutex_unlock(&sMu);
        if (!got)
            break;
        if (!BuildAndSend(fd, sender, item.type, item.bytes, item.len, ++(*seq)))
        {
            pthread_mutex_lock(&sMu);
            /* Put it back at the read side by sliding... simplest: push again. */
            if (!QueuePush(sOut, &sOutRead, &sOutWrite, &sOutCount, &item))
            {
                /* Queue was full of newer words; this one is dropped. */
            }
            pthread_mutex_unlock(&sMu);
            break;
        }
    }
}

static bool ReadLine(int fd, char *out, size_t cap, uint64_t limitMs)
{
    size_t n = 0;
    uint64_t start = CoopNowMs();
    while (n + 1 < cap)
    {
        uint8_t b;
        ssize_t r;
        if (CoopNowMs() - start > limitMs)
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

static void TuneSocket(int fd, bool phone)
{
    int on = 1;
    int idle = phone ? 15 : 30;
    int interval = 5;
    int count = phone ? 9 : 3;
    int userMs = phone ? 90000 : 20000;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
#ifdef TCP_KEEPIDLE
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
#ifdef TCP_KEEPINTVL
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
#endif
#ifdef TCP_KEEPCNT
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
#endif
#ifdef TCP_USER_TIMEOUT
    setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &userMs, sizeof(userMs));
#endif
    (void)idle;
    (void)interval;
    (void)count;
    (void)userMs;
}

static int TcpConnect(const char *host, int port, int waitMs)
{
    int fd;
    struct sockaddr_in addr;
    int flags;
    int rc;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
        return -1;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    TuneSocket(fd, sPhoneLink != 0);
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
        if (poll(&pfd, 1, waitMs) <= 0)
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
    CoopHmacSha256(secret, sizeof(secret), (const uint8_t *)"HNS-ZT-SESSION-V1", 16, sSessionKey);
    memset(secret, 0, sizeof(secret));
    sHaveKey = true;
    return true;
}

static bool HelloProof(char *out, size_t cap)
{
    uint8_t secret[32];
    uint8_t mac[32];
    char msg[64];
    int n;
    CoopSecretShared(secret);
    n = snprintf(msg, sizeof(msg), "hello|%s", sClientId);
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

static void RunSession(int fd)
{
    char line[160];
    char proof[80];
    char hello[128];
    uint8_t acc[COOP_MAX_FRAME + 8];
    size_t have = 0;
    uint32_t seq = 1;
    int sender;
    uint64_t lastPos = 0;

    if (!HelloProof(proof, sizeof(proof)))
    {
        close(fd);
        return;
    }
    snprintf(hello, sizeof(hello), "HELLO %s %s\n", sClientId, proof);
    memset(proof, 0, sizeof(proof));
    if (!SendAll(fd, (const uint8_t *)hello, strlen(hello)))
    {
        memset(hello, 0, sizeof(hello));
        close(fd);
        return;
    }
    memset(hello, 0, sizeof(hello));
    if (!ReadLine(fd, line, sizeof(line), sPhoneLink ? 20000 : 8000))
    {
        close(fd);
        return;
    }
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
    sender = atoi(line + 6);
    if (sender != 0 && sender != 1)
        sender = 0;
    pthread_mutex_lock(&sMu);
    sRole = sender;
    sLinkUp = true;
    sEverUp = true;
    sSessionFd = fd;
    pthread_mutex_unlock(&sMu);
    fprintf(stderr, "co-op: joined host\n");

    while (sWant)
    {
        uint64_t now;
        if (!RecvSome(fd, acc, sizeof(acc), &have, 200))
            break;
        while (have >= 4)
        {
            uint32_t len = LoadBe32(acc);
            if (len < 16 || len > COOP_MAX_FRAME)
            {
                have = 0;
                goto down;
            }
            if (have < 4 + len)
                break;
            DecodeEncrypted(acc + 4, len);
            memmove(acc, acc + 4 + len, have - 4 - len);
            have -= 4 + len;
        }
        if (sForceFlush)
        {
            sForceFlush = false;
#ifdef TCP_USER_TIMEOUT
            if (sBackground)
            {
                int userMs = 120000;
                setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &userMs, sizeof(userMs));
            }
#endif
            FlushOutbound(fd, sender, &seq);
            lastPos = CoopNowMs();
        }
        now = CoopNowMs();
        if (lastPos == 0 || now - lastPos >= 400)
        {
            FlushOutbound(fd, sender, &seq);
            lastPos = now;
        }
        else
        {
            /* Still drain a few story words between position beats. */
            int guard;
            for (guard = 0; guard < 4; guard++)
            {
                struct CoopQueued item;
                bool got;
                pthread_mutex_lock(&sMu);
                got = QueuePop(sOut, &sOutRead, &sOutWrite, &sOutCount, &item);
                pthread_mutex_unlock(&sMu);
                if (!got)
                    break;
                if (!BuildAndSend(fd, sender, item.type, item.bytes, item.len, ++seq))
                    goto down;
            }
        }
    }
down:
    pthread_mutex_lock(&sMu);
    if (sSessionFd == fd)
        sSessionFd = -1;
    pthread_mutex_unlock(&sMu);
    NoteLink(false);
    close(fd);
    fprintf(stderr, "co-op: host link down\n");
}

static void ChooseEndpoint(char *host, size_t hostCap, int *port)
{
    const char *asHost = getenv("HNS_COOP_HOST");
    const char *localPort = getenv("HNS_COOP_LOCAL_PORT");
    sPhoneLink = 0;
    if (asHost != NULL && asHost[0] == '1')
    {
        int spins = 0;
        CoopHost_Start();
        while (!CoopHost_Listening() && spins < 50)
        {
            usleep(100 * 1000);
            spins++;
        }
        snprintf(host, hostCap, "127.0.0.1");
        *port = 27770;
        sPhoneLink = 1;
        return;
    }
    if (localPort != NULL && localPort[0] != '\0')
    {
        snprintf(host, hostCap, "127.0.0.1");
        *port = atoi(localPort);
        sPhoneLink = 1;
        return;
    }
    CoopSecretHost(host);
    *port = CoopSecretPort();
}

static void *ThreadMain(void *unused)
{
    char host[64];
    int port;
    int waitMs;
    (void)unused;
    signal(SIGPIPE, SIG_IGN);
    ChooseEndpoint(host, sizeof(host), &port);
    waitMs = sPhoneLink ? 20000 : 8000;
    PrepareKey();
    LoadOrCreateClientId();

    while (sWant)
    {
        int fd = TcpConnect(host, port, waitMs);
        if (fd < 0)
        {
            if (sEverUp)
            {
                /* One resume of the same session, then stop. */
                sResumeSpent = true;
                break;
            }
            /* First join: a short wait, then one more try, then stop.
             * No popup and no reconnect loop. */
            usleep(1500 * 1000);
            fd = TcpConnect(host, port, waitMs);
            if (fd < 0)
                break;
        }
        RunSession(fd);
        if (!sWant)
            break;
        if (!sEverUp)
            break;
        if (sResumeSpent)
            break;
        sResumeSpent = true;
        fprintf(stderr, "co-op: resuming host session\n");
        usleep(400 * 1000);
    }
    NoteLink(false);
    memset(host, 0, sizeof(host));
    return NULL;
}

void CoopNet_SetBackgrounded(bool background)
{
    pthread_mutex_lock(&sMu);
    sBackground = background;
    sForceFlush = true;
    pthread_mutex_unlock(&sMu);
    CoopNet_SendWord(COOP_MSG_WORLD_READY, background ? COOP_READY_BACKGROUND : COOP_READY_FOREGROUND);
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
    const char *join = getenv("HNS_COOP_JOIN");
    const char *asHost = getenv("HNS_COOP_HOST");
    const char *localPort = getenv("HNS_COOP_LOCAL_PORT");
    bool armed = (join != NULL && join[0] == '1')
        || (asHost != NULL && asHost[0] == '1')
        || (localPort != NULL && localPort[0] != '\0');
    if (!armed)
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
