#include "minicrypto.h"

#include <string.h>

// ---------------------------------------------------------------- AES

static uint8_t g_sbox[256], g_inv[256];
static uint32_t g_te[256], g_td[256];
static int g_ready = 0;

static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }
static uint8_t mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xt(a);
        b >>= 1;
    }
    return r;
}
static uint32_t rotr8(uint32_t v) { return (v >> 8) | (v << 24); }

static void aes_tables(void) {
    if (g_ready) return;
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ (uint8_t)(p << 1) ^ ((p & 0x80) ? 0x1b : 0));
        q ^= (uint8_t)(q << 1);
        q ^= (uint8_t)(q << 2);
        q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;
        uint8_t x = (uint8_t)(q ^ (uint8_t)((q << 1) | (q >> 7)) ^ (uint8_t)((q << 2) | (q >> 6)) ^
                              (uint8_t)((q << 3) | (q >> 5)) ^ (uint8_t)((q << 4) | (q >> 4)));
        g_sbox[p] = (uint8_t)(x ^ 0x63);
    } while (p != 1);
    g_sbox[0] = 0x63;
    for (int i = 0; i < 256; i++) g_inv[g_sbox[i]] = (uint8_t)i;
    for (int i = 0; i < 256; i++) {
        uint8_t s = g_sbox[i];
        g_te[i] = ((uint32_t)mul(s, 2) << 24) | ((uint32_t)s << 16) | ((uint32_t)s << 8) | mul(s, 3);
        uint8_t v = g_inv[i];
        g_td[i] = ((uint32_t)mul(v, 14) << 24) | ((uint32_t)mul(v, 9) << 16) | ((uint32_t)mul(v, 13) << 8) | mul(v, 11);
    }
    g_ready = 1;
}

static uint32_t sub_word(uint32_t w) {
    return ((uint32_t)g_sbox[w >> 24] << 24) | ((uint32_t)g_sbox[(w >> 16) & 255] << 16) |
           ((uint32_t)g_sbox[(w >> 8) & 255] << 8) | g_sbox[w & 255];
}

void mc_aes_init(mc_aes* c, const uint8_t* key, size_t keyBytes) {
    aes_tables();
    int nk = (int)(keyBytes / 4);
    c->rounds = nk + 6;
    int total = 4 * (c->rounds + 1);
    for (int i = 0; i < nk; i++)
        c->enc[i] = ((uint32_t)key[4 * i] << 24) | ((uint32_t)key[4 * i + 1] << 16) | ((uint32_t)key[4 * i + 2] << 8) |
                    key[4 * i + 3];
    uint8_t rc = 1;
    for (int i = nk; i < total; i++) {
        uint32_t t = c->enc[i - 1];
        if (i % nk == 0) {
            t = sub_word((t << 8) | (t >> 24)) ^ ((uint32_t)rc << 24);
            rc = xt(rc);
        } else if (nk > 6 && i % nk == 4) {
            t = sub_word(t);
        }
        c->enc[i] = c->enc[i - nk] ^ t;
    }
    // Equivalent inverse cipher key schedule.
    for (int r = 0; r <= c->rounds; r++) {
        for (int j = 0; j < 4; j++) {
            uint32_t w = c->enc[4 * (c->rounds - r) + j];
            if (r != 0 && r != c->rounds) {
                uint32_t a = g_td[g_sbox[w >> 24]], b = g_td[g_sbox[(w >> 16) & 255]],
                         d = g_td[g_sbox[(w >> 8) & 255]], e = g_td[g_sbox[w & 255]];
                w = a ^ rotr8(b) ^ rotr8(rotr8(d)) ^ rotr8(rotr8(rotr8(e)));
            }
            c->dec[4 * r + j] = w;
        }
    }
}

#define LOAD32(p) (((uint32_t)(p)[0] << 24) | ((uint32_t)(p)[1] << 16) | ((uint32_t)(p)[2] << 8) | (p)[3])
#define STORE32(p, v)                  \
    do {                               \
        (p)[0] = (uint8_t)((v) >> 24); \
        (p)[1] = (uint8_t)((v) >> 16); \
        (p)[2] = (uint8_t)((v) >> 8);  \
        (p)[3] = (uint8_t)(v);         \
    } while (0)

void mc_aes_encrypt_block(const mc_aes* c, const uint8_t in[16], uint8_t out[16]) {
    const uint32_t* k = c->enc;
    uint32_t s0 = LOAD32(in) ^ k[0], s1 = LOAD32(in + 4) ^ k[1], s2 = LOAD32(in + 8) ^ k[2], s3 = LOAD32(in + 12) ^ k[3];
    k += 4;
    for (int r = 1; r < c->rounds; r++, k += 4) {
#define TE(a, b, cc, d)                                                                             \
    (g_te[(a) >> 24] ^ rotr8(g_te[((b) >> 16) & 255]) ^ rotr8(rotr8(g_te[((cc) >> 8) & 255])) ^ \
     rotr8(rotr8(rotr8(g_te[(d) & 255]))))
        uint32_t t0 = TE(s0, s1, s2, s3) ^ k[0], t1 = TE(s1, s2, s3, s0) ^ k[1], t2 = TE(s2, s3, s0, s1) ^ k[2],
                 t3 = TE(s3, s0, s1, s2) ^ k[3];
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
    }
#define SB(a, b, cc, d)                                                                         \
    (((uint32_t)g_sbox[(a) >> 24] << 24) | ((uint32_t)g_sbox[((b) >> 16) & 255] << 16) | \
     ((uint32_t)g_sbox[((cc) >> 8) & 255] << 8) | g_sbox[(d) & 255])
    uint32_t r0 = SB(s0, s1, s2, s3) ^ k[0], r1 = SB(s1, s2, s3, s0) ^ k[1], r2 = SB(s2, s3, s0, s1) ^ k[2],
             r3 = SB(s3, s0, s1, s2) ^ k[3];
    STORE32(out, r0); STORE32(out + 4, r1); STORE32(out + 8, r2); STORE32(out + 12, r3);
}

void mc_aes_decrypt_block(const mc_aes* c, const uint8_t in[16], uint8_t out[16]) {
    const uint32_t* k = c->dec;
    uint32_t s0 = LOAD32(in) ^ k[0], s1 = LOAD32(in + 4) ^ k[1], s2 = LOAD32(in + 8) ^ k[2], s3 = LOAD32(in + 12) ^ k[3];
    k += 4;
    for (int r = 1; r < c->rounds; r++, k += 4) {
#define TD(a, b, cc, d)                                                                             \
    (g_td[(a) >> 24] ^ rotr8(g_td[((b) >> 16) & 255]) ^ rotr8(rotr8(g_td[((cc) >> 8) & 255])) ^ \
     rotr8(rotr8(rotr8(g_td[(d) & 255]))))
        uint32_t t0 = TD(s0, s3, s2, s1) ^ k[0], t1 = TD(s1, s0, s3, s2) ^ k[1], t2 = TD(s2, s1, s0, s3) ^ k[2],
                 t3 = TD(s3, s2, s1, s0) ^ k[3];
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
    }
#define ISB(a, b, cc, d)                                                                      \
    (((uint32_t)g_inv[(a) >> 24] << 24) | ((uint32_t)g_inv[((b) >> 16) & 255] << 16) | \
     ((uint32_t)g_inv[((cc) >> 8) & 255] << 8) | g_inv[(d) & 255])
    uint32_t r0 = ISB(s0, s3, s2, s1) ^ k[0], r1 = ISB(s1, s0, s3, s2) ^ k[1], r2 = ISB(s2, s1, s0, s3) ^ k[2],
             r3 = ISB(s3, s2, s1, s0) ^ k[3];
    STORE32(out, r0); STORE32(out + 4, r1); STORE32(out + 8, r2); STORE32(out + 12, r3);
}

void mc_aes_cbc_decrypt(const mc_aes* c, uint8_t iv[16], uint8_t* data, size_t len) {
    uint8_t cur[16], plain[16];
    for (size_t off = 0; off + 16 <= len; off += 16) {
        memcpy(cur, data + off, 16);
        mc_aes_decrypt_block(c, cur, plain);
        for (int i = 0; i < 16; i++) data[off + i] = plain[i] ^ iv[i];
        memcpy(iv, cur, 16);
    }
}

// ---------------------------------------------------------------- SHA-256

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(mc_sha256* c, const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = LOAD32(p + 4 * i);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void mc_sha256_init(mc_sha256* c) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof iv);
    c->total = 0;
    c->used = 0;
}

void mc_sha256_update(mc_sha256* c, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    c->total += len;
    while (len) {
        size_t take = 64 - c->used;
        if (take > len) take = len;
        memcpy(c->buf + c->used, p, take);
        c->used += take;
        p += take;
        len -= take;
        if (c->used == 64) {
            sha256_block(c, c->buf);
            c->used = 0;
        }
    }
}

void mc_sha256_final(mc_sha256* c, uint8_t out[32]) {
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80, zero = 0, len[8];
    mc_sha256_update(c, &pad, 1);
    while (c->used != 56) mc_sha256_update(c, &zero, 1);
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    mc_sha256_update(c, len, 8);
    for (int i = 0; i < 8; i++) STORE32(out + 4 * i, c->h[i]);
}

// ---------------------------------------------------------------- SHA-1

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_block(mc_sha1* c, const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = LOAD32(p + 4 * i);
    for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & cc) | (~b & d); k = 0x5a827999; }
        else if (i < 40) { f = b ^ cc ^ d; k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8f1bbcdc; }
        else { f = b ^ cc ^ d; k = 0xca62c1d6; }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = ROL(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

void mc_sha1_init(mc_sha1* c) {
    static const uint32_t iv[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    memcpy(c->h, iv, sizeof iv);
    c->total = 0;
    c->used = 0;
}

void mc_sha1_update(mc_sha1* c, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    c->total += len;
    while (len) {
        size_t take = 64 - c->used;
        if (take > len) take = len;
        memcpy(c->buf + c->used, p, take);
        c->used += take;
        p += take;
        len -= take;
        if (c->used == 64) {
            sha1_block(c, c->buf);
            c->used = 0;
        }
    }
}

void mc_sha1_final(mc_sha1* c, uint8_t out[20]) {
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80, zero = 0, len[8];
    mc_sha1_update(c, &pad, 1);
    while (c->used != 56) mc_sha1_update(c, &zero, 1);
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    mc_sha1_update(c, len, 8);
    for (int i = 0; i < 5; i++) STORE32(out + 4 * i, c->h[i]);
}

void mc_hmac_sha1_init(mc_hmac_sha1* c, const uint8_t* key, size_t keyLen) {
    uint8_t k[64] = {0}, pad[64];
    if (keyLen > 64) {
        mc_sha1 t;
        mc_sha1_init(&t);
        mc_sha1_update(&t, key, keyLen);
        mc_sha1_final(&t, k);
    } else {
        memcpy(k, key, keyLen);
    }
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    mc_sha1_init(&c->inner);
    mc_sha1_update(&c->inner, pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    mc_sha1_init(&c->outer);
    mc_sha1_update(&c->outer, pad, 64);
}

void mc_hmac_sha1_update(mc_hmac_sha1* c, const void* data, size_t len) { mc_sha1_update(&c->inner, data, len); }

void mc_hmac_sha1_final(mc_hmac_sha1* c, uint8_t out[20]) {
    uint8_t in[20];
    mc_sha1_final(&c->inner, in);
    mc_sha1_update(&c->outer, in, 20);
    mc_sha1_final(&c->outer, out);
}

void mc_pbkdf2_sha1(const uint8_t* pw, size_t pwLen, const uint8_t* salt, size_t saltLen, unsigned rounds,
                    uint8_t* out, size_t outLen) {
    mc_hmac_sha1 base;
    mc_hmac_sha1_init(&base, pw, pwLen);
    uint32_t block = 1;
    while (outLen) {
        uint8_t ctr[4], u[20], t[20];
        STORE32(ctr, block);
        mc_hmac_sha1 h = base;
        mc_hmac_sha1_update(&h, salt, saltLen);
        mc_hmac_sha1_update(&h, ctr, 4);
        mc_hmac_sha1_final(&h, u);
        memcpy(t, u, 20);
        for (unsigned r = 1; r < rounds; r++) {
            h = base;
            mc_hmac_sha1_update(&h, u, 20);
            mc_hmac_sha1_final(&h, u);
            for (int i = 0; i < 20; i++) t[i] ^= u[i];
        }
        size_t take = outLen < 20 ? outLen : 20;
        memcpy(out, t, take);
        out += take;
        outLen -= take;
        block++;
    }
}

// ---------------------------------------------------------------- RAR key derivation

static void sha256_begin_pad(mc_sha256* c, const uint8_t* key, size_t keyLen, uint8_t fill) {
    uint8_t pad[64];
    memset(pad, fill, sizeof pad);
    for (size_t i = 0; i < keyLen && i < sizeof pad; i++) pad[i] ^= key[i];
    mc_sha256_init(c);
    mc_sha256_update(c, pad, sizeof pad);
}

void mc_hmac_sha256(const uint8_t* key, size_t keyLen, const void* data, size_t len, uint8_t out[32]) {
    uint8_t hashed[32];
    if (keyLen > 64) {
        mc_sha256 h;
        mc_sha256_init(&h);
        mc_sha256_update(&h, key, keyLen);
        mc_sha256_final(&h, hashed);
        key = hashed;
        keyLen = 32;
    }
    mc_sha256 c;
    sha256_begin_pad(&c, key, keyLen, 0x36);
    mc_sha256_update(&c, data, len);
    mc_sha256_final(&c, out);
    sha256_begin_pad(&c, key, keyLen, 0x5c);
    mc_sha256_update(&c, out, 32);
    mc_sha256_final(&c, out);
}

// RAR 5: PBKDF2-HMAC-SHA256 where the running value is taken at three points.
void mc_rar5_kdf(const uint8_t* pw, size_t pwLen, const uint8_t salt[16], unsigned lg2, uint8_t key[32],
                 uint8_t hashKey[32], uint8_t check[8]) {
    uint8_t hashed[32];
    if (pwLen > 64) {
        mc_sha256 h;
        mc_sha256_init(&h);
        mc_sha256_update(&h, pw, pwLen);
        mc_sha256_final(&h, hashed);
        pw = hashed;
        pwLen = 32;
    }
    mc_sha256 inner, outer, c;
    sha256_begin_pad(&inner, pw, pwLen, 0x36);
    sha256_begin_pad(&outer, pw, pwLen, 0x5c);

    static const uint8_t blockIndex[4] = {0, 0, 0, 1};
    uint8_t u[32], acc[32];
    c = inner;
    mc_sha256_update(&c, salt, 16);
    mc_sha256_update(&c, blockIndex, 4);
    mc_sha256_final(&c, u);
    c = outer;
    mc_sha256_update(&c, u, 32);
    mc_sha256_final(&c, u);
    memcpy(acc, u, 32);

    unsigned total = 1u << lg2;
    for (unsigned stage = 0; stage < 3; stage++) {
        unsigned rounds = stage == 0 ? total - 1 : 16;
        for (unsigned r = 0; r < rounds; r++) {
            c = inner;
            mc_sha256_update(&c, u, 32);
            mc_sha256_final(&c, u);
            c = outer;
            mc_sha256_update(&c, u, 32);
            mc_sha256_final(&c, u);
            for (int i = 0; i < 32; i++) acc[i] ^= u[i];
        }
        if (stage == 0) memcpy(key, acc, 32);
        else if (stage == 1) memcpy(hashKey, acc, 32);
        else {
            memset(check, 0, 8);
            for (int i = 0; i < 32; i++) check[i % 8] ^= acc[i];
        }
    }
}

// RAR 3/4 hashes with a SHA-1 whose whole blocks taken straight from the input are overwritten
// with the end of the message schedule. Long passwords reach that path, and it changes the key.
typedef struct {
    uint32_t h[5];
    uint64_t total;
    uint8_t buf[64];
    size_t used;
} Rar3Sha1;

static uint32_t rol32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void rar3_block(uint32_t h[5], const uint8_t* p, uint8_t* writeBack) {
    uint32_t w[16];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int t = 0; t < 80; t++) {
        if (t >= 16) w[t & 15] = rol32(w[(t + 13) & 15] ^ w[(t + 8) & 15] ^ w[(t + 2) & 15] ^ w[t & 15], 1);
        uint32_t f, k;
        if (t < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (t < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (t < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t tmp = rol32(a, 5) + f + e + k + w[t & 15];
        e = d; d = c; c = rol32(b, 30); b = a; a = tmp;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    if (writeBack)
        for (int i = 0; i < 16; i++) {
            writeBack[i * 4] = (uint8_t)w[i];
            writeBack[i * 4 + 1] = (uint8_t)(w[i] >> 8);
            writeBack[i * 4 + 2] = (uint8_t)(w[i] >> 16);
            writeBack[i * 4 + 3] = (uint8_t)(w[i] >> 24);
        }
}

static void rar3_update(Rar3Sha1* c, uint8_t* data, size_t len) {
    c->total += len;
    size_t i = 0;
    if (c->used + len >= 64) {
        i = 64 - c->used;
        memcpy(c->buf + c->used, data, i);
        rar3_block(c->h, c->buf, NULL);
        for (; i + 64 <= len; i += 64) rar3_block(c->h, data + i, data + i);
        c->used = 0;
    }
    memcpy(c->buf + c->used, data + i, len - i);
    c->used += len - i;
}

static void rar3_final(const Rar3Sha1* src, uint8_t out[20]) {
    Rar3Sha1 c = *src;
    uint64_t bits = c.total * 8;
    c.buf[c.used++] = 0x80;
    if (c.used > 56) {
        memset(c.buf + c.used, 0, 64 - c.used);
        rar3_block(c.h, c.buf, NULL);
        c.used = 0;
    }
    memset(c.buf + c.used, 0, 56 - c.used);
    for (int i = 0; i < 8; i++) c.buf[56 + i] = (uint8_t)(bits >> (56 - i * 8));
    rar3_block(c.h, c.buf, NULL);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(c.h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c.h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c.h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)c.h[i];
    }
}

void mc_rar3_kdf(const uint8_t* pwUtf16, size_t pwBytes, const uint8_t* salt8, uint8_t key[16], uint8_t iv[16]) {
    enum { Rounds = 1 << 18 };
    uint8_t raw[256 + 8];
    if (pwBytes > 256) pwBytes = 256;
    memcpy(raw, pwUtf16, pwBytes);
    size_t rawLen = pwBytes;
    if (salt8) {
        memcpy(raw + rawLen, salt8, 8);
        rawLen += 8;
    }
    Rar3Sha1 c = {{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0}, 0, {0}, 0};
    uint8_t digest[20];
    for (unsigned i = 0; i < Rounds; i++) {
        uint8_t counter[3] = {(uint8_t)i, (uint8_t)(i >> 8), (uint8_t)(i >> 16)};
        rar3_update(&c, raw, rawLen);
        rar3_update(&c, counter, 3);
        if (i % (Rounds / 16) == 0) {
            rar3_final(&c, digest);
            iv[i / (Rounds / 16)] = digest[19];
        }
    }
    rar3_final(&c, digest);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) key[i * 4 + j] = digest[i * 4 + 3 - j];
}
