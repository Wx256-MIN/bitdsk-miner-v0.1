/**
 * sha256_midstate.h
 *
 * Plain SHA-256 (FIPS 180-4) plus the "midstate" trick Bitcoin miners use:
 * a block header is 80 bytes = one 64-byte SHA-256 block plus a 16-byte
 * remainder. The first block's compression output ("midstate") never
 * changes as you vary the nonce, so it's computed once per job and handed
 * to the ASIC instead of the full header - the chip only has to run the
 * second, much shorter compression per nonce attempt.
 *
 * This file is ordinary, publicly specified cryptography (not anything
 * BitDsk- or Bitaxe-specific) and can be trusted at face value; test it
 * against the NIST SHA-256 test vectors if you want independent proof
 * before relying on it.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t  buffer[64];
    size_t   buffer_len;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len);
void sha256_final(sha256_ctx_t *ctx, uint8_t digest[32]);
void sha256_full(const uint8_t *data, size_t len, uint8_t digest[32]);

/**
 * Runs SHA-256's compression function on exactly one 64-byte block against
 * the standard initial hash values, returning the resulting 8-word state
 * as 32 raw bytes (big-endian words, matching what most ASIC job formats
 * expect). Use this on the first 64 bytes of an 80-byte block header.
 */
void sha256_midstate(const uint8_t block[64], uint8_t midstate_out[32]);

/**
 * Continues a SHA-256 computation from a previously-saved 32-byte state
 * (as produced by sha256_midstate) through exactly one more 64-byte
 * block, returning the resulting state as digest bytes. This is how
 * asic_result_task recomputes a candidate block hash cheaply: it already
 * has the job's midstate, so it only needs to run the second block
 * (header tail + nonce + SHA-256 padding) rather than re-hash all 80
 * header bytes from scratch for every nonce it validates.
 */
void sha256_continue(const uint8_t state_in[32], const uint8_t block[64], uint8_t digest_out[32]);
