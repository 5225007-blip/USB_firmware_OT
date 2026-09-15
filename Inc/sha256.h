/**
 * @file    sha256.h
 * @brief   Minimal, dependency-free SHA-256 implementation.
 *
 * The STM32F407 has no hardware crypto accelerator (that's F415/F417/
 * F437/F439), so fingerprinting uses this software implementation.
 * It's used only for device fingerprinting (VID/PID + serial + salted
 * descriptor bytes), not for any high-throughput or timing-critical path.
 */

#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buffer[64];
    uint32_t buffer_len;
} sha256_ctx_t;

void SHA256_Init(sha256_ctx_t *ctx);
void SHA256_Update(sha256_ctx_t *ctx, const uint8_t *data, size_t len);
void SHA256_Final(sha256_ctx_t *ctx, uint8_t digest[32]);

/** Convenience one-shot: digest = SHA256(salt || data). */
void SHA256_Salted(const uint8_t *salt, size_t salt_len,
                    const uint8_t *data, size_t data_len,
                    uint8_t digest[32]);

#endif /* SHA256_H */
