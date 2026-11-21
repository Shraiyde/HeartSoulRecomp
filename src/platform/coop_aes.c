#ifdef PORTABLE
#include "platform/coop_aes.h"

#include <stdlib.h>
#include <string.h>

static const uint8_t sSbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const uint8_t sRcon[11] = {
    0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36
};

static uint32_t LoadBe32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void StoreBe32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t SubWord(uint32_t w)
{
    return ((uint32_t)sSbox[(w >> 24) & 0xff] << 24)
         | ((uint32_t)sSbox[(w >> 16) & 0xff] << 16)
         | ((uint32_t)sSbox[(w >> 8) & 0xff] << 8)
         | (uint32_t)sSbox[w & 0xff];
}

static uint32_t RotWord(uint32_t w)
{
    return (w << 8) | (w >> 24);
}

static void Aes256Expand(const uint8_t key[32], uint32_t rk[60])
{
    int i;

    for (i = 0; i < 8; i++)
        rk[i] = LoadBe32(key + (size_t)i * 4);
    for (i = 8; i < 60; i++)
    {
        uint32_t t = rk[i - 1];
        if ((i % 8) == 0)
            t = SubWord(RotWord(t)) ^ ((uint32_t)sRcon[i / 8] << 24);
        else if ((i % 8) == 4)
            t = SubWord(t);
        rk[i] = rk[i - 8] ^ t;
    }
}

static uint8_t Xtime(uint8_t x)
{
    return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
}

static void Aes256EncryptBlock(const uint32_t rk[60], const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[4][4];
    int round, r, c;

    for (c = 0; c < 4; c++)
    {
        for (r = 0; r < 4; r++)
            s[r][c] = in[c * 4 + r];
    }

    for (round = 0; round <= 14; round++)
    {
        if (round != 0)
        {
            for (r = 0; r < 4; r++)
            {
                for (c = 0; c < 4; c++)
                    s[r][c] = sSbox[s[r][c]];
            }
            for (r = 1; r < 4; r++)
            {
                uint8_t tmp[4];
                for (c = 0; c < 4; c++)
                    tmp[c] = s[r][(c + r) & 3];
                for (c = 0; c < 4; c++)
                    s[r][c] = tmp[c];
            }
            if (round != 14)
            {
                for (c = 0; c < 4; c++)
                {
                    uint8_t a = s[0][c];
                    uint8_t b = s[1][c];
                    uint8_t d = s[2][c];
                    uint8_t e = s[3][c];
                    s[0][c] = (uint8_t)(Xtime(a ^ b) ^ b ^ d ^ e);
                    s[1][c] = (uint8_t)(a ^ Xtime(b ^ d) ^ d ^ e);
                    s[2][c] = (uint8_t)(a ^ b ^ Xtime(d ^ e) ^ e);
                    s[3][c] = (uint8_t)(Xtime(e ^ a) ^ a ^ b ^ d);
                }
            }
        }
        for (c = 0; c < 4; c++)
        {
            uint32_t k = rk[round * 4 + c];
            s[0][c] ^= (uint8_t)(k >> 24);
            s[1][c] ^= (uint8_t)(k >> 16);
            s[2][c] ^= (uint8_t)(k >> 8);
            s[3][c] ^= (uint8_t)k;
        }
    }

    for (c = 0; c < 4; c++)
    {
        for (r = 0; r < 4; r++)
            out[c * 4 + r] = s[r][c];
    }
}

static void XorBlock(uint8_t *d, const uint8_t *a, const uint8_t *b)
{
    int i;
    for (i = 0; i < 16; i++)
        d[i] = (uint8_t)(a[i] ^ b[i]);
}

/* GHASH multiply in the GCM bit ordering (NIST right-shift). */
static void GhashMul(uint8_t x[16], const uint8_t h[16])
{
    uint8_t z[16];
    uint8_t v[16];
    int i, bit;

    memset(z, 0, sizeof(z));
    memcpy(v, h, 16);
    for (i = 0; i < 16; i++)
    {
        for (bit = 7; bit >= 0; bit--)
        {
            uint8_t lsb;
            int k;
            if ((x[i] >> bit) & 1)
            {
                for (k = 0; k < 16; k++)
                    z[k] ^= v[k];
            }
            lsb = (uint8_t)(v[15] & 1);
            for (k = 15; k > 0; k--)
                v[k] = (uint8_t)((v[k] >> 1) | (v[k - 1] << 7));
            v[0] >>= 1;
            if (lsb)
                v[0] ^= 0xe1;
        }
    }
    memcpy(x, z, 16);
}

static void Inc32(uint8_t block[16])
{
    uint32_t n = LoadBe32(block + 12);
    StoreBe32(block + 12, n + 1);
}

static void GhashData(uint8_t y[16], const uint8_t h[16], const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off + 16 <= len)
    {
        int i;
        for (i = 0; i < 16; i++)
            y[i] ^= data[off + i];
        GhashMul(y, h);
        off += 16;
    }
    if (off < len)
    {
        uint8_t block[16];
        memset(block, 0, sizeof(block));
        memcpy(block, data + off, len - off);
        XorBlock(y, y, block);
        GhashMul(y, h);
    }
}

static void GhashLengths(uint8_t y[16], const uint8_t h[16], size_t cipherLen)
{
    uint8_t block[16];
    memset(block, 0, sizeof(block));
    /* AAD length is 0. Cipher length in bits. */
    StoreBe32(block + 8, (uint32_t)((cipherLen >> 29) & 0xffffffffu));
    StoreBe32(block + 12, (uint32_t)((cipherLen << 3) & 0xffffffffu));
    XorBlock(y, y, block);
    GhashMul(y, h);
}

static void CryptCtr(const uint32_t rk[60], const uint8_t nonce[12],
                     const uint8_t *in, size_t len, uint8_t *out)
{
    uint8_t counter[16];
    uint8_t stream[16];
    size_t off = 0;

    memset(counter, 0, sizeof(counter));
    memcpy(counter, nonce, 12);
    StoreBe32(counter + 12, 2); /* J0 is nonce||1; first keystream is inc32(J0) */

    while (off < len)
    {
        size_t n = len - off;
        size_t i;
        if (n > 16)
            n = 16;
        Aes256EncryptBlock(rk, counter, stream);
        for (i = 0; i < n; i++)
            out[off + i] = (uint8_t)(in[off + i] ^ stream[i]);
        Inc32(counter);
        off += n;
    }
}

static void ComputeTag(const uint32_t rk[60], const uint8_t h[16], const uint8_t nonce[12],
                       const uint8_t *cipher, size_t cipherLen, uint8_t tag[16])
{
    uint8_t y[16];
    uint8_t j0[16];
    uint8_t ek[16];

    memset(y, 0, sizeof(y));
    GhashData(y, h, cipher, cipherLen);
    GhashLengths(y, h, cipherLen);
    memset(j0, 0, sizeof(j0));
    memcpy(j0, nonce, 12);
    StoreBe32(j0 + 12, 1);
    Aes256EncryptBlock(rk, j0, ek);
    XorBlock(tag, y, ek);
}

int CoopAesGcmEncrypt(const uint8_t key[32], const uint8_t nonce[12],
                      const uint8_t *plain, size_t plainLen,
                      uint8_t *out, uint8_t tag[16])
{
    uint32_t rk[60];
    uint8_t h[16];
    uint8_t zero[16];

    if (key == NULL || nonce == NULL || (plain == NULL && plainLen != 0) || out == NULL || tag == NULL)
        return -1;
    Aes256Expand(key, rk);
    memset(zero, 0, sizeof(zero));
    Aes256EncryptBlock(rk, zero, h);
    CryptCtr(rk, nonce, plain, plainLen, out);
    ComputeTag(rk, h, nonce, out, plainLen, tag);
    return 0;
}

int CoopAesGcmDecrypt(const uint8_t key[32], const uint8_t nonce[12],
                      const uint8_t *cipher, size_t cipherLen, const uint8_t tag[16],
                      uint8_t *out)
{
    uint32_t rk[60];
    uint8_t h[16];
    uint8_t zero[16];
    uint8_t expect[16];
    uint8_t diff;
    size_t i;

    if (key == NULL || nonce == NULL || tag == NULL || (cipher == NULL && cipherLen != 0) || out == NULL)
        return -1;
    Aes256Expand(key, rk);
    memset(zero, 0, sizeof(zero));
    Aes256EncryptBlock(rk, zero, h);
    ComputeTag(rk, h, nonce, cipher, cipherLen, expect);
    diff = 0;
    for (i = 0; i < 16; i++)
        diff |= (uint8_t)(expect[i] ^ tag[i]);
    if (diff != 0)
        return -1;
    CryptCtr(rk, nonce, cipher, cipherLen, out);
    return 0;
}

static uint32_t Rotr(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32 - n));
}

static void Sha256Block(uint32_t st[8], const uint8_t block[64])
{
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = LoadBe32(block + (size_t)i * 4);
    for (i = 16; i < 64; i++)
    {
        uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = st[0]; b = st[1]; c = st[2]; d = st[3];
    e = st[4]; f = st[5]; g = st[6]; h = st[7];
    for (i = 0; i < 64; i++)
    {
        uint32_t S1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
    st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

void CoopSha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    uint32_t st[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    uint8_t block[64];
    size_t off = 0;
    uint64_t bits = (uint64_t)len * 8;
    int i;

    while (off + 64 <= len)
    {
        Sha256Block(st, data + off);
        off += 64;
    }
    memset(block, 0, sizeof(block));
    if (len > off)
        memcpy(block, data + off, len - off);
    block[len - off] = 0x80;
    if (len - off >= 56)
    {
        Sha256Block(st, block);
        memset(block, 0, sizeof(block));
    }
    for (i = 0; i < 8; i++)
        block[63 - i] = (uint8_t)(bits >> (8 * i));
    Sha256Block(st, block);
    for (i = 0; i < 8; i++)
        StoreBe32(out + (size_t)i * 4, st[i]);
}

void CoopHmacSha256(const uint8_t *key, size_t keyLen, const uint8_t *data, size_t dataLen, uint8_t out[32])
{
    uint8_t k[64];
    uint8_t ipad[64];
    uint8_t opad[64];
    uint8_t inner[32];
    uint8_t *buf;
    size_t i;
    /* data is hashed as key-pad || data. Bound the stack copy. */
    uint8_t stack[64 + 256];
    uint8_t *heap = NULL;

    memset(k, 0, sizeof(k));
    if (keyLen > 64)
        CoopSha256(key, keyLen, k);
    else if (key != NULL && keyLen > 0)
        memcpy(k, key, keyLen);
    for (i = 0; i < 64; i++)
    {
        ipad[i] = (uint8_t)(k[i] ^ 0x36);
        opad[i] = (uint8_t)(k[i] ^ 0x5c);
    }
    if (dataLen <= 256)
    {
        buf = stack;
    }
    else
    {
        /* The game build has malloc. This path is only the HELLO proof,
         * whose input is a short string, so the stack buffer is enough.
         * Keep a fallback for a long HMAC used by tests. */
        heap = (uint8_t *)malloc(64 + dataLen);
        buf = heap;
        if (buf == NULL)
        {
            memset(out, 0, 32);
            return;
        }
    }
    memcpy(buf, ipad, 64);
    if (dataLen > 0 && data != NULL)
        memcpy(buf + 64, data, dataLen);
    CoopSha256(buf, 64 + dataLen, inner);
    memcpy(buf, opad, 64);
    memcpy(buf + 64, inner, 32);
    CoopSha256(buf, 64 + 32, out);
    if (heap != NULL)
        free(heap);
    memset(inner, 0, sizeof(inner));
    memset(k, 0, sizeof(k));
}

int CoopBase64Encode(const uint8_t *in, size_t len, char *out, size_t outCap)
{
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = 4 * ((len + 2) / 3);
    size_t i, o = 0;

    if (out == NULL || outCap < need + 1)
        return -1;
    for (i = 0; i + 3 <= len; i += 3)
    {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = tab[(v >> 18) & 63];
        out[o++] = tab[(v >> 12) & 63];
        out[o++] = tab[(v >> 6) & 63];
        out[o++] = tab[v & 63];
    }
    if (len - i == 1)
    {
        uint32_t v = ((uint32_t)in[i] << 16);
        out[o++] = tab[(v >> 18) & 63];
        out[o++] = tab[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    }
    else if (len - i == 2)
    {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = tab[(v >> 18) & 63];
        out[o++] = tab[(v >> 12) & 63];
        out[o++] = tab[(v >> 6) & 63];
        out[o++] = '=';
    }
    out[o] = '\0';
    return (int)o;
}
#else
typedef int CoopAesUnusedOnGba;
#endif
