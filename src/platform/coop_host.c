#ifdef PORTABLE
#if defined(__linux__)

#define _GNU_SOURCE
#include "platform/coop_host.h"
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

#define HOST_PORT 27770
#define HOST_SLOTS 2
#define HOST_FRAME 8192
#define HOST_MAGIC 0x4D504C4Bu
#define HOST_STAMP 2

struct HostSlot
{
    int fd;
    int role;
    char id[24];
    uint64_t recvCounter;
    uint64_t sendCounter;
    uint64_t closedAtMs;
    uint8_t acc[HOST_FRAME + 8];
    size_t have;
    bool used;
};

static pthread_t sThread;
static pthread_mutex_t sMu = PTHREAD_MUTEX_INITIALIZER;
static bool sStarted;
static volatile bool sListening;
static uint8_t sKey[32];
static struct HostSlot sSlot[HOST_SLOTS];

static uint64_t NowMs(void)
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

static int B64Value(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

static int Base64Decode(const char *in, uint8_t *out, size_t cap)
{
    size_t n = 0;
    int val = 0;
    int bits = 0;
    for (; *in != '\0'; in++)
    {
        int d;
        if (*in == '=' || *in == '\r' || *in == '\n')
            break;
        d = B64Value((unsigned char)*in);
        if (d < 0)
            return -1;
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            if (n >= cap)
                return -1;
            out[n++] = (uint8_t)((val >> bits) & 0xff);
        }
    }
    return (int)n;
}

static bool MacEqual(const uint8_t *a, const uint8_t *b)
{
    uint8_t diff = 0;
    int i;
    for (i = 0; i < 32; i++)
        diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
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

static bool ReadLine(int fd, char *out, size_t cap)
{
    size_t n = 0;
    uint64_t start = NowMs();
    while (n + 1 < cap)
    {
        uint8_t b;
        ssize_t r;
        if (NowMs() - start > 8000)
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

static void Tune(int fd)
{
    int on = 1;
    int idle = 15;
    int interval = 5;
    int count = 9;
    int userMs = 90000;
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

static bool ProofOk(const char *id, const char *b64)
{
    uint8_t secret[32];
    uint8_t expect[32];
    uint8_t got[32];
    char msg[64];
    int n;
    int decoded;
    bool ok;

    n = snprintf(msg, sizeof(msg), "hello|%s", id);
    if (n <= 0 || (size_t)n >= sizeof(msg))
        return false;
    decoded = Base64Decode(b64, got, sizeof(got));
    if (decoded != 32)
        return false;
    CoopSecretShared(secret);
    CoopHmacSha256(secret, sizeof(secret), (const uint8_t *)msg, (size_t)n, expect);
    memset(secret, 0, sizeof(secret));
    memset(msg, 0, sizeof(msg));
    ok = MacEqual(expect, got);
    memset(expect, 0, sizeof(expect));
    memset(got, 0, sizeof(got));
    return ok;
}

static bool StampAndSend(struct HostSlot *dest, const uint8_t *plain, size_t plainLen)
{
    uint8_t nonce[12];
    uint8_t cipher[HOST_FRAME];
    uint8_t tag[16];
    uint8_t packet[12 + HOST_FRAME];
    uint8_t header[4];
    uint64_t counter;
    size_t wire;

    if (dest->fd < 0 || plainLen > HOST_FRAME - 16)
        return false;
    counter = dest->sendCounter++;
    if (dest->sendCounter == 0)
        dest->sendCounter = 1;
    StoreBe32(nonce, HOST_STAMP);
    StoreBe64(nonce + 4, counter);
    if (CoopAesGcmEncrypt(sKey, nonce, plain, plainLen, cipher, tag) != 0)
        return false;
    memcpy(packet, nonce, 12);
    memcpy(packet + 12, cipher, plainLen);
    memcpy(packet + 12 + plainLen, tag, 16);
    wire = 12 + plainLen + 16;
    StoreBe32(header, (uint32_t)wire);
    if (!SendAll(dest->fd, header, 4))
        return false;
    return SendAll(dest->fd, packet, wire);
}

static void CloseSlotFd(struct HostSlot *slot)
{
    if (slot->fd >= 0)
    {
        close(slot->fd);
        slot->fd = -1;
        slot->have = 0;
        slot->closedAtMs = NowMs();
    }
}

static int ActiveCount(void)
{
    int i, n = 0;
    for (i = 0; i < HOST_SLOTS; i++)
    {
        if (sSlot[i].used && sSlot[i].fd >= 0)
            n++;
    }
    return n;
}

static void Relay(struct HostSlot *from, const uint8_t *plain, size_t plainLen)
{
    int i;
    for (i = 0; i < HOST_SLOTS; i++)
    {
        if (!sSlot[i].used || sSlot[i].fd < 0 || &sSlot[i] == from)
            continue;
        if (!StampAndSend(&sSlot[i], plain, plainLen))
            CloseSlotFd(&sSlot[i]);
    }
}

static bool TakeFrame(struct HostSlot *slot, const uint8_t *wire, size_t wireLen)
{
    uint8_t plain[HOST_FRAME];
    uint32_t sender;
    uint64_t counter;
    size_t cipherLen;
    const uint8_t *tag;

    if (wireLen < 12 + 16)
        return true;
    sender = LoadBe32(wire);
    counter = LoadBe64(wire + 4);
    if ((int)sender != slot->role)
        return true;
    if (counter <= slot->recvCounter)
        return true;
    cipherLen = wireLen - 12 - 16;
    if (cipherLen > sizeof(plain))
        return false;
    tag = wire + 12 + cipherLen;
    if (CoopAesGcmDecrypt(sKey, wire, wire + 12, cipherLen, tag, plain) != 0)
        return false;
    slot->recvCounter = counter;
    if (cipherLen >= 16 && LoadBe32(plain) == HOST_MAGIC)
        Relay(slot, plain, cipherLen);
    return true;
}

static void Drain(struct HostSlot *slot)
{
    while (slot->have >= 4)
    {
        uint32_t len = LoadBe32(slot->acc);
        if (len < 16 || len > HOST_FRAME)
        {
            CloseSlotFd(slot);
            return;
        }
        if (slot->have < 4 + len)
            return;
        if (!TakeFrame(slot, slot->acc + 4, len))
        {
            CloseSlotFd(slot);
            return;
        }
        memmove(slot->acc, slot->acc + 4 + len, slot->have - 4 - len);
        slot->have -= 4 + len;
    }
}

static void Expire(void)
{
    int i;
    uint64_t now = NowMs();
    for (i = 0; i < HOST_SLOTS; i++)
    {
        if (!sSlot[i].used || sSlot[i].fd >= 0)
            continue;
        if (now - sSlot[i].closedAtMs > 30000)
            memset(&sSlot[i], 0, sizeof(sSlot[i]));
    }
}

static struct HostSlot *Claim(const char *id)
{
    int i;
    struct HostSlot *freeSlot = NULL;
    for (i = 0; i < HOST_SLOTS; i++)
    {
        if (sSlot[i].used && strcmp(sSlot[i].id, id) == 0)
            return &sSlot[i];
    }
    for (i = 0; i < HOST_SLOTS; i++)
    {
        if (!sSlot[i].used)
        {
            freeSlot = &sSlot[i];
            break;
        }
    }
    if (freeSlot == NULL)
        return NULL;
    memset(freeSlot, 0, sizeof(*freeSlot));
    freeSlot->fd = -1;
    freeSlot->role = (int)(freeSlot - sSlot);
    freeSlot->sendCounter = 1;
    snprintf(freeSlot->id, sizeof(freeSlot->id), "%s", id);
    freeSlot->used = true;
    return freeSlot;
}

static void Welcome(int fd)
{
    char line[160];
    char id[24];
    char proof[96];
    char reply[32];
    struct HostSlot *slot;
    int flags;

    Tune(fd);
    if (!ReadLine(fd, line, sizeof(line)))
    {
        close(fd);
        return;
    }
    if (sscanf(line, "HELLO %23s %95s", id, proof) != 2 || !ProofOk(id, proof))
    {
        fprintf(stderr, "co-op: phone host rejected a join\n");
        close(fd);
        return;
    }
    memset(line, 0, sizeof(line));
    memset(proof, 0, sizeof(proof));
    Expire();
    slot = Claim(id);
    memset(id, 0, sizeof(id));
    if (slot == NULL)
    {
        const char *full = "FULL\n";
        SendAll(fd, (const uint8_t *)full, 5);
        close(fd);
        return;
    }
    CloseSlotFd(slot);
    slot->fd = fd;
    slot->have = 0;
    snprintf(reply, sizeof(reply), "READY %d %d\n", slot->role, ActiveCount());
    if (!SendAll(fd, (const uint8_t *)reply, strlen(reply)))
    {
        CloseSlotFd(slot);
        return;
    }
    flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    fprintf(stderr, "co-op: phone host accepted role %d\n", slot->role);
}

static void *Serve(void *unused)
{
    int listenFd;
    struct sockaddr_in addr;
    int on = 1;
    (void)unused;
    signal(SIGPIPE, SIG_IGN);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(HOST_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0)
        return NULL;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (bind(listenFd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(listenFd, 2) < 0)
    {
        fprintf(stderr, "co-op: phone host could not listen\n");
        close(listenFd);
        return NULL;
    }
    {
        int flags = fcntl(listenFd, F_GETFL, 0);
        if (flags >= 0)
            fcntl(listenFd, F_SETFL, flags | O_NONBLOCK);
    }
    pthread_mutex_lock(&sMu);
    sListening = true;
    pthread_mutex_unlock(&sMu);
    fprintf(stderr, "co-op: phone host listening\n");

    for (;;)
    {
        struct pollfd pfds[1 + HOST_SLOTS];
        int nfds = 1;
        int i;
        int rc;
        pfds[0].fd = listenFd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        for (i = 0; i < HOST_SLOTS; i++)
        {
            if (!sSlot[i].used || sSlot[i].fd < 0)
                continue;
            pfds[nfds].fd = sSlot[i].fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }
        rc = poll(pfds, (nfds_t)nfds, 200);
        if (rc < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        Expire();
        if (pfds[0].revents & POLLIN)
        {
            int fd = accept(listenFd, NULL, NULL);
            if (fd >= 0)
                Welcome(fd);
        }
        for (i = 1; i < nfds; i++)
        {
            int s;
            if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            for (s = 0; s < HOST_SLOTS; s++)
            {
                ssize_t n;
                if (!sSlot[s].used || sSlot[s].fd != pfds[i].fd)
                    continue;
                if (sSlot[s].have >= sizeof(sSlot[s].acc))
                {
                    CloseSlotFd(&sSlot[s]);
                    break;
                }
                n = recv(sSlot[s].fd, sSlot[s].acc + sSlot[s].have,
                         sizeof(sSlot[s].acc) - sSlot[s].have, 0);
                if (n <= 0)
                {
                    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                        break;
                    CloseSlotFd(&sSlot[s]);
                    break;
                }
                sSlot[s].have += (size_t)n;
                Drain(&sSlot[s]);
                break;
            }
        }
    }
    close(listenFd);
    pthread_mutex_lock(&sMu);
    sListening = false;
    pthread_mutex_unlock(&sMu);
    return NULL;
}

void CoopHost_Start(void)
{
    uint8_t secret[32];
    pthread_mutex_lock(&sMu);
    if (sStarted)
    {
        pthread_mutex_unlock(&sMu);
        return;
    }
    sStarted = true;
    pthread_mutex_unlock(&sMu);
    CoopSecretShared(secret);
    CoopHmacSha256(secret, sizeof(secret), (const uint8_t *)"HNS-ZT-SESSION-V1", 16, sKey);
    memset(secret, 0, sizeof(secret));
    if (pthread_create(&sThread, NULL, Serve, NULL) == 0)
        pthread_detach(sThread);
}

bool CoopHost_Listening(void)
{
    bool up;
    pthread_mutex_lock(&sMu);
    up = sListening;
    pthread_mutex_unlock(&sMu);
    return up;
}

#else
typedef int CoopHostUnused;
#endif
#else
typedef int CoopHostUnusedOnGba;
#endif
