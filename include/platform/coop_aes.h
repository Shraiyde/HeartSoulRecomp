#ifndef COOP_AES_H
#define COOP_AES_H

#include <stddef.h>
#include <stdint.h>

/* AES-256-GCM, SHA-256 and HMAC-SHA256 used by the World Host link.
 * No secret material is printed by these helpers. */

void CoopSha256(const uint8_t *data, size_t len, uint8_t out[32]);
void CoopHmacSha256(const uint8_t *key, size_t keyLen, const uint8_t *data, size_t dataLen, uint8_t out[32]);

/* out receives ciphertext then the 16-byte tag is written to tag.
 * Returns 0 on success. */
int CoopAesGcmEncrypt(const uint8_t key[32], const uint8_t nonce[12],
                      const uint8_t *plain, size_t plainLen,
                      uint8_t *out, uint8_t tag[16]);

/* cipherLen is the ciphertext length, not including the tag. */
int CoopAesGcmDecrypt(const uint8_t key[32], const uint8_t nonce[12],
                      const uint8_t *cipher, size_t cipherLen, const uint8_t tag[16],
                      uint8_t *out);

/* Standard base64 with padding, no newlines. Returns bytes written, or -1. */
int CoopBase64Encode(const uint8_t *in, size_t len, char *out, size_t outCap);

#endif
