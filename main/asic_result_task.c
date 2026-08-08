/**
 * asic_result_task.c
 *
 * Polls bm1397_read_response() for nonce results, re-hashes each
 * candidate using the job's saved midstate (see sha256_continue) to get
 * its actual difficulty, and submits it to the pool via Stratum if it
 * clears the current pool difficulty. Runs independently of
 * create_jobs_task so a burst of results doesn't delay new work going out.
 */

#include <string.h>
#include <math.h>
#include "global_state.h"
#include "bm1397.h"
#include "sha256_midstate.h"
#include "stratum.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "asic_result";

static void reverse_bytes(uint8_t *data, int len)
{
    for (int i = 0; i < len / 2; i++) {
        uint8_t t = data[i]; data[i] = data[len-1-i]; data[len-1-i] = t;
    }
}

/* Approximates log2(target1) - log2(hash) using the leading 64 significant
 * bits of the (big-endian) hash rather than a full bignum division - see
 * the write-up in the project README for why this is precise enough for
 * a difficulty *comparison* even though it's not exact bignum arithmetic. */
static double hash_to_difficulty(const uint8_t hash_be[32])
{
    int first_nonzero = 0;
    while (first_nonzero < 32 && hash_be[first_nonzero] == 0) first_nonzero++;
    if (first_nonzero >= 32) return 1e18; /* hash == 0, treat as effectively infinite */

    uint64_t top_bits = 0;
    for (int i = 0; i < 8; i++) {
        int idx = first_nonzero + i;
        uint8_t b = (idx < 32) ? hash_be[idx] : 0;
        top_bits = (top_bits << 8) | b;
    }
    if (top_bits == 0) top_bits = 1;

    double log2_hash = log2((double)top_bits) + 8.0 * (24 - first_nonzero);
    const double log2_target1 = 224.0; /* log2(0xFFFF * 2^208), accurate to ~0.002% */
    return pow(2.0, log2_target1 - log2_hash);
}

static void hex_encode(const uint8_t *data, int len, char *out)
{
    for (int i = 0; i < len; i++) sprintf(out + i * 2, "%02x", data[i]);
    out[len * 2] = '\0';
}

void asic_result_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "asic_result_task started");

    while (1) {
        bool asic_ready;
        xSemaphoreTake(g_state.mutex, portMAX_DELAY);
        asic_ready = g_state.asic_initialized;
        xSemaphoreGive(g_state.mutex);
        if (!asic_ready) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        bm1397_response_t resp;
        if (!bm1397_read_response(&resp, 200)) {
            continue; /* timeout - normal when the ASIC has nothing to report yet */
        }

        if (!resp.is_job_response) {
            /* Register response (e.g. hashrate/error-count telemetry) -
             * power_management_task / the web API can extend this to
             * actually record specific register values as needed. */
            continue;
        }

        xSemaphoreTake(g_state.mutex, portMAX_DELAY);
        bm_job_t job = g_state.active_jobs[resp.job_id & (MAX_ACTIVE_JOBS - 1)];
        xSemaphoreGive(g_state.mutex);

        if (!job.valid) {
            /* Stale or corrupted job_id - discard and keep going, matching
             * the documented behavior of silently dropping results for
             * jobs that have already been invalidated. */
            continue;
        }

        g_state.total_nonces_received++;

        /* Rebuild the second 64-byte SHA-256 block: merkle_tail(4) +
         * ntime(4) + nbits(4) + nonce(4) + 0x80 padding + zero padding +
         * 8-byte big-endian bit length (80 bytes total message = 640 bits). */
        uint8_t block2[64] = {0};
        memcpy(block2, job.merkle_root + 28, 4); /* last 4 bytes of the 32-byte merkle root */
        memcpy(block2 + 4, job.ntime, 4);
        memcpy(block2 + 8, job.nbits, 4);
        uint32_t nonce_be = resp.nonce;
        block2[12] = (uint8_t)(nonce_be >> 24);
        block2[13] = (uint8_t)(nonce_be >> 16);
        block2[14] = (uint8_t)(nonce_be >> 8);
        block2[15] = (uint8_t)(nonce_be);
        block2[16] = 0x80;
        uint64_t bit_len = 640;
        for (int i = 0; i < 8; i++) block2[56 + i] = (uint8_t)(bit_len >> (56 - 8 * i));

        uint8_t hash1[32], hash2[32];
        sha256_continue(job.midstate, block2, hash1);
        sha256_full(hash1, 32, hash2);
        reverse_bytes(hash2, 32); /* conventional big-endian block-hash order */

        double difficulty = hash_to_difficulty(hash2);

        xSemaphoreTake(g_state.mutex, portMAX_DELAY);
        if (difficulty > g_state.best_difficulty) g_state.best_difficulty = difficulty;
        uint32_t pool_diff = g_state.pool_difficulty ? g_state.pool_difficulty : 1;
        xSemaphoreGive(g_state.mutex);

        if (difficulty >= (double)pool_diff) {
            char extranonce2_hex[17];
            char counter_hex[9];
            snprintf(counter_hex, sizeof(counter_hex), "%08x", job.extranonce2);
            int en2_len = g_state.extranonce2_len > 0 ? g_state.extranonce2_len : 4;
            int pad = en2_len * 2 - (int)strlen(counter_hex);
            if (pad < 0) pad = 0;
            memset(extranonce2_hex, '0', pad);
            strncpy(extranonce2_hex + pad, counter_hex, sizeof(extranonce2_hex) - pad - 1);
            extranonce2_hex[en2_len * 2] = '\0';

            char ntime_hex[9];
            hex_encode(job.ntime, 4, ntime_hex);

            ESP_LOGI(TAG, "share found: difficulty %.0f (pool wants %u)", difficulty, pool_diff);
            stratum_submit_share(job.pool_job_id, extranonce2_hex, ntime_hex, resp.nonce, 0);
        }
    }
}
