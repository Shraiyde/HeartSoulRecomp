#ifndef COOP_SECRET_H
#define COOP_SECRET_H

/* Baked ZeroTier World Host endpoint and shared auth secret.
 * The bytes are XOR-obfuscated the same way as the existing World Host.
 * Never log the decoded secret. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const uint8_t sCoopKeyPad[24] = {
    0xA7, 0x3C, 0x91, 0x5E, 0x28, 0xF4, 0x6B, 0xD0, 0x19, 0xC2, 0x7F, 0x44,
    0xE8, 0x33, 0xB5, 0x6A, 0x0D, 0x9F, 0x52, 0xC8, 0x17, 0xE1, 0x4A, 0x76
};

/* 10.137.178.161 */
static const uint8_t sCoopHostEnc[14] = {
    0x96, 0x0C, 0xBF, 0x6F, 0x1B, 0xC3, 0x45, 0xE1, 0x2E, 0xFA, 0x51, 0x75, 0xDE, 0x02
};

/* 27770 */
static const uint8_t sCoopPortEnc[5] = { 0x95, 0x0B, 0xA6, 0x69, 0x18 };

static const uint8_t sCoopSecretEnc[32] = {
    0x76, 0x2B, 0xFF, 0x47, 0x3C, 0x11, 0xD9, 0xA9, 0x39, 0x80, 0xEA, 0x1B, 0xCB, 0x35, 0x64, 0x27,
    0xFC, 0x5C, 0x8E, 0x49, 0x81, 0xB1, 0x11, 0x14, 0x04, 0x85, 0x74, 0x67, 0xD1, 0x02, 0x7D, 0x20
};

static inline void CoopSecretDecode(const uint8_t *enc, size_t len, uint8_t *out)
{
    size_t i;
    for (i = 0; i < len; i++)
        out[i] = (uint8_t)(enc[i] ^ sCoopKeyPad[i % sizeof(sCoopKeyPad)]);
}

static inline void CoopSecretHost(char out[16])
{
    uint8_t raw[14];
    CoopSecretDecode(sCoopHostEnc, sizeof(sCoopHostEnc), raw);
    memcpy(out, raw, sizeof(raw));
    out[sizeof(raw)] = '\0';
    memset(raw, 0, sizeof(raw));
}

static inline int CoopSecretPort(void)
{
    uint8_t raw[6];
    int port = 0;
    size_t i;
    CoopSecretDecode(sCoopPortEnc, sizeof(sCoopPortEnc), raw);
    raw[sizeof(sCoopPortEnc)] = '\0';
    for (i = 0; raw[i] >= '0' && raw[i] <= '9'; i++)
        port = port * 10 + (raw[i] - '0');
    memset(raw, 0, sizeof(raw));
    return port;
}

static inline void CoopSecretShared(uint8_t out[32])
{
    CoopSecretDecode(sCoopSecretEnc, sizeof(sCoopSecretEnc), out);
}

#endif
