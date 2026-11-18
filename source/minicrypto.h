// Small, dependency-free crypto for encrypted archives (AES, SHA-1, SHA-256, HMAC, PBKDF2, RAR key derivation).
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t enc[60], dec[60];
    int rounds;
} mc_aes;

void mc_aes_init(mc_aes* ctx, const uint8_t* key, size_t keyBytes);  // 16, 24 or 32 bytes
void mc_aes_encrypt_block(const mc_aes* ctx, const uint8_t in[16], uint8_t out[16]);
void mc_aes_decrypt_block(const mc_aes* ctx, const uint8_t in[16], uint8_t out[16]);
// In-place CBC decryption; `len` must be a multiple of 16. `iv` is updated for the next call.
void mc_aes_cbc_decrypt(const mc_aes* ctx, uint8_t iv[16], uint8_t* data, size_t len);

typedef struct {
    uint32_t h[8];
    uint64_t total;
    uint8_t buf[64];
    size_t used;
} mc_sha256;
void mc_sha256_init(mc_sha256* c);
void mc_sha256_update(mc_sha256* c, const void* data, size_t len);
void mc_sha256_final(mc_sha256* c, uint8_t out[32]);

typedef struct {
    uint32_t h[5];
    uint64_t total;
    uint8_t buf[64];
    size_t used;
} mc_sha1;
void mc_sha1_init(mc_sha1* c);
void mc_sha1_update(mc_sha1* c, const void* data, size_t len);
void mc_sha1_final(mc_sha1* c, uint8_t out[20]);

typedef struct {
    mc_sha1 inner, outer;
} mc_hmac_sha1;
void mc_hmac_sha1_init(mc_hmac_sha1* c, const uint8_t* key, size_t keyLen);
void mc_hmac_sha1_update(mc_hmac_sha1* c, const void* data, size_t len);
void mc_hmac_sha1_final(mc_hmac_sha1* c, uint8_t out[20]);

void mc_pbkdf2_sha1(const uint8_t* pw, size_t pwLen, const uint8_t* salt, size_t saltLen, unsigned rounds,
                    uint8_t* out, size_t outLen);

void mc_hmac_sha256(const uint8_t* key, size_t keyLen, const void* data, size_t len, uint8_t out[32]);
// `lg2` is the log2 of the PBKDF2 rounds; callers must reject values above 24.
void mc_rar5_kdf(const uint8_t* pw, size_t pwLen, const uint8_t salt[16], unsigned lg2, uint8_t key[32],
                 uint8_t hashKey[32], uint8_t check[8]);
// `pwUtf16` is little-endian; `salt8` may be NULL for archives without a salt.
void mc_rar3_kdf(const uint8_t* pwUtf16, size_t pwBytes, const uint8_t* salt8, uint8_t key[16], uint8_t iv[16]);

#ifdef __cplusplus
}
#endif
