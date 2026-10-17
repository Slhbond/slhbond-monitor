/*
 * sha256.h - SHA-256, implemented here rather than pulled in from OpenSSL.
 *
 * The whole project links against nothing but libc and pthreads, and the one
 * place that genuinely needs a digest is the self-update path: without it,
 * whoever can alter the download can take over the machine. FIPS 180-4 is a
 * short, well-specified algorithm, so carrying ~150 lines is cheaper than
 * carrying a TLS library.
 */
#ifndef SLH_SHA256_H
#define SLH_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SLH_SHA256_HEX_LEN 65   /* 64 hex digits + NUL */

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[64];
    size_t   buflen;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *c);
void sha256_update(sha256_ctx_t *c, const void *data, size_t len);
void sha256_final(sha256_ctx_t *c, uint8_t out[32]);

/** Lower-case hex digest into `hex_out` (at least SLH_SHA256_HEX_LEN bytes). */
void sha256_hex(const uint8_t digest[32], char *hex_out, size_t hex_size);

/**
 * Digest a file in streaming fashion (no size limit, constant memory).
 * Returns 0 on success, -1 when the file cannot be read.
 */
int sha256_file(const char *path, char *hex_out, size_t hex_size);

/** Digest a NUL-terminated string. Always succeeds. */
void sha256_string(const char *s, char *hex_out, size_t hex_size);

/** Case-insensitive comparison of two hex digests. 1 when equal. */
int sha256_hex_equal(const char *a, const char *b);

#endif /* SLH_SHA256_H */
