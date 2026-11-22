#ifndef COOP_SECRET_H
#define COOP_SECRET_H

/* Shared co-op authentication secret. Never log the decoded secret. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const uint8_t sCoopKeyPad[24] = {
    0xA7, 0x3C, 0x91, 0x5E, 0x28, 0xF4, 0x6B, 0xD0, 0x19, 0xC2, 0x7F, 0x44,
    0xE8, 0x33, 0xB5, 0x6A, 0x0D, 0x9F, 0x52, 0xC8, 0x17, 0xE1, 0x4A, 0x76
};

static inline void CoopSecretDecode(const uint8_t *enc, size_t len, uint8_t *out)
{
    size_t i;
    for (i = 0; i < len; i++)
        out[i] = (uint8_t)(enc[i] ^ sCoopKeyPad[i % sizeof(sCoopKeyPad)]);
}

static inline void CoopSecretShared(uint8_t out[32])
{
    CoopSecretDecode(sCoopSecretEnc, sizeof(sCoopSecretEnc), out);
}

#endif
