// libarchive without a crypto library has stub AES and HMAC, so WinZip-AES ZIPs can't be opened
// or created. Its crypto is reached only through these two tables, which are replaced here. With
// no crypto backend its context types are plain ints, so they hold a slot number for the state.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "minicrypto.h"

#define BLOCK 16
#define SLOTS 32

typedef struct {
    mc_aes aes;
    uint8_t nonce[BLOCK], encr[BLOCK];
    unsigned pos;
    int used;
} AesSlot;
typedef struct {
    mc_hmac_sha1 hmac;
    int used;
} HmacSlot;

static AesSlot g_aes[SLOTS];
static HmacSlot g_hmac[SLOTS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int pbkdf2(const char* pw, size_t pwLen, const uint8_t* salt, size_t saltLen, unsigned rounds, uint8_t* out,
                  size_t outLen) {
    mc_pbkdf2_sha1((const uint8_t*)pw, pwLen, salt, saltLen, rounds, out, outLen);
    return 0;
}

static int ctr_init(int* ctx, const uint8_t* key, size_t keyLen) {
    if (keyLen != 16 && keyLen != 24 && keyLen != 32) return -1;
    pthread_mutex_lock(&g_lock);
    int slot = -1;
    for (int i = 0; i < SLOTS && slot < 0; i++)
        if (!g_aes[i].used) slot = i;
    if (slot >= 0) g_aes[slot].used = 1;
    pthread_mutex_unlock(&g_lock);
    if (slot < 0) return -1;
    AesSlot* s = &g_aes[slot];
    mc_aes_init(&s->aes, key, keyLen);
    memset(s->nonce, 0, BLOCK);
    memset(s->encr, 0, BLOCK);
    s->pos = BLOCK;
    *ctx = slot;
    return 0;
}

// WinZip AES: little-endian counter starting at 1.
static int ctr_update(int* ctx, const uint8_t* in, size_t inLen, uint8_t* out, size_t* outLen) {
    if (*ctx < 0 || *ctx >= SLOTS) return -1;
    AesSlot* s = &g_aes[*ctx];
    size_t max = inLen < *outLen ? inLen : *outLen;
    for (size_t i = 0; i < max; i++) {
        if (s->pos == BLOCK) {
            for (int j = 0; j < 8; j++)
                if (++s->nonce[j]) break;
            mc_aes_encrypt_block(&s->aes, s->nonce, s->encr);
            s->pos = 0;
        }
        out[i] = in[i] ^ s->encr[s->pos++];
    }
    *outLen = max;
    return 0;
}

static int ctr_release(int* ctx) {
    if (*ctx >= 0 && *ctx < SLOTS) {
        pthread_mutex_lock(&g_lock);
        memset(&g_aes[*ctx], 0, sizeof(AesSlot));
        pthread_mutex_unlock(&g_lock);
    }
    *ctx = -1;
    return 0;
}

static int hmac_init(int* ctx, const uint8_t* key, size_t keyLen) {
    pthread_mutex_lock(&g_lock);
    int slot = -1;
    for (int i = 0; i < SLOTS && slot < 0; i++)
        if (!g_hmac[i].used) slot = i;
    if (slot >= 0) g_hmac[slot].used = 1;
    pthread_mutex_unlock(&g_lock);
    if (slot < 0) return -1;
    mc_hmac_sha1_init(&g_hmac[slot].hmac, key, keyLen);
    *ctx = slot;
    return 0;
}

static void hmac_update(int* ctx, const uint8_t* data, size_t len) {
    if (*ctx >= 0 && *ctx < SLOTS) mc_hmac_sha1_update(&g_hmac[*ctx].hmac, data, len);
}

// Callers pass a buffer of *outLen bytes and expect at most that many (ZIP stores 10 of 20).
static void hmac_final(int* ctx, uint8_t* out, size_t* outLen) {
    if (*ctx < 0 || *ctx >= SLOTS) return;
    uint8_t full[20];
    mc_hmac_sha1 copy = g_hmac[*ctx].hmac;
    mc_hmac_sha1_final(&copy, full);
    size_t n = *outLen < 20 ? *outLen : 20;
    memcpy(out, full, n);
    *outLen = n;
}

static void hmac_cleanup(int* ctx) {
    if (*ctx >= 0 && *ctx < SLOTS) {
        pthread_mutex_lock(&g_lock);
        memset(&g_hmac[*ctx], 0, sizeof(HmacSlot));
        pthread_mutex_unlock(&g_lock);
    }
    *ctx = -1;
}

// Same layouts as struct archive_cryptor / struct archive_hmac in libarchive 3.7.2.
const struct {
    int (*pbkdf2sha1)(const char*, size_t, const uint8_t*, size_t, unsigned, uint8_t*, size_t);
    int (*dec_init)(int*, const uint8_t*, size_t);
    int (*dec_update)(int*, const uint8_t*, size_t, uint8_t*, size_t*);
    int (*dec_release)(int*);
    int (*enc_init)(int*, const uint8_t*, size_t);
    int (*enc_update)(int*, const uint8_t*, size_t, uint8_t*, size_t*);
    int (*enc_release)(int*);
} __archive_cryptor = {pbkdf2, ctr_init, ctr_update, ctr_release, ctr_init, ctr_update, ctr_release};

const struct {
    int (*init)(int*, const uint8_t*, size_t);
    void (*update)(int*, const uint8_t*, size_t);
    void (*final)(int*, uint8_t*, size_t*);
    void (*cleanup)(int*);
} __archive_hmac = {hmac_init, hmac_update, hmac_final, hmac_cleanup};

// libarchive converts names stored in archives to the "current locale" charset, which it gets
// from here. The Switch only has the C locale, where non-ASCII names (Persian, Chinese...) in 7z
// and UTF-8 ZIP files can't be represented and would be lost. File names here are always UTF-8.
#ifndef CODESET_OVERRIDE_DONE
#define CODESET_OVERRIDE_DONE
char* nl_langinfo(int item) {
    (void)item;
    return (char*)"UTF-8";
}
#endif
