/*
 * spake_port.h - minimal BoringSSL-compat shim for standalone SPAKE2
 * extraction (adb pairing_auth). Replaces openssl/base.h, crypto/internal.h,
 * constant-time helpers and memory macros. SHA-512/RAND_bytes come from the
 * system OpenSSL (>= 1.1.1).
 *
 * Extracted from BoringSSL (MIT license), adapted for the legacy adb 5.0.2
 * tree which links OpenSSL 1.1.1.
 */
#ifndef SPAKE_PORT_H
#define SPAKE_PORT_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* real crypto from system OpenSSL */
#include <openssl/sha.h>
#include <openssl/rand.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- basic types ---- */
typedef size_t crypto_word_t;

/* ---- memory helpers (system OpenSSL >= 3.0 may already define the malloc
 * macros in crypto.h; only define when absent) ---- */
#ifndef OPENSSL_malloc
#define OPENSSL_malloc  malloc
#endif
#ifndef OPENSSL_free
#define OPENSSL_free    free
#endif
#ifndef OPENSSL_realloc
#define OPENSSL_realloc realloc
#endif
#define OPENSSL_memcpy  memcpy
#define OPENSSL_memset  memset
#define OPENSSL_memmove memmove
#ifndef OPENSSL_strdup
#define OPENSSL_strdup  strdup
#endif

/* compiler value barrier (from boringssl crypto/internal.h) */
static inline uint32_t value_barrier_u32(uint32_t a) {
#if defined(__GNUC__) && !defined(__clang__)
    __asm__("" : "+r"(a) : : "memory");
#else
    __asm__ volatile("" : "+r"(a) :: "memory");
#endif
    return a;
}

/* ---- static assert (gcc 4.8 / gnu99 safe) ---- */
#define OPENSSL_STATIC_ASSERT_INTERNAL(cond, line) \
    typedef char openssl_static_assert_##line[(cond) ? 1 : -1]
#define OPENSSL_STATIC_ASSERT(cond, msg) \
    OPENSSL_STATIC_ASSERT_INTERNAL(cond, __LINE__)

/* ---- constant-time helpers (functionally correct) ---- */
static inline crypto_word_t constant_time_eq_w(crypto_word_t a, crypto_word_t b) {
    return (crypto_word_t)((a == b) ? ~(crypto_word_t)0 : 0);
}
static inline crypto_word_t constant_time_select_w(crypto_word_t mask, crypto_word_t a,
                                                   crypto_word_t b) {
    return (mask != 0) ? a : b;
}

/* ---- SPAKE2 public API (from boringssl include/openssl/curve25519.h) ---- */
#define SPAKE2_MAX_MSG_SIZE 32
#define SPAKE2_MAX_KEY_SIZE 64

enum spake2_role_t {
    spake2_role_alice = 0,
    spake2_role_bob = 1,
};

typedef struct spake2_ctx_st SPAKE2_CTX;

SPAKE2_CTX *SPAKE2_CTX_new(enum spake2_role_t my_role, const uint8_t *my_name,
                           size_t my_name_len, const uint8_t *their_name,
                           size_t their_name_len);
void SPAKE2_CTX_free(SPAKE2_CTX *ctx);
int SPAKE2_generate_msg(SPAKE2_CTX *ctx, uint8_t *out, size_t *out_len,
                        size_t max_out_len, const uint8_t *password,
                        size_t password_len);
int SPAKE2_process_msg(SPAKE2_CTX *ctx, uint8_t *out_key, size_t *out_key_len,
                       size_t max_out_key_len, const uint8_t *their_msg,
                       size_t their_msg_len);

#ifdef __cplusplus
}
#endif

#endif /* SPAKE_PORT_H */
