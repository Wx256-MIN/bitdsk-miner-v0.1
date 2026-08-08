/**
 * create_jobs_task.c
 *
 * Consumes mining_notify_t entries from the work queue and turns each into
 * a bm1397_job_t: build the coinbase transaction, hash the merkle tree,
 * assemble the 80-byte block header, and hand the first 64 bytes to
 * sha256_midstate() so the ASIC only has to grind the short remainder per
 * nonce attempt.
 *
 * HONESTY NOTE: Bitcoin's block header uses specific byte-order
 * conventions (several fields arrive from the pool in a different byte
 * order than the header needs) that are notoriously easy to get subtly
 * wrong in any stratum client, this one included. The overall pipeline
 * shape here (coinbase -> merkle root -> header -> midstate) is standard
 * and correct; the exact byte-reversal calls are this project's best
 * effort and worth verifying against known-good test vectors before
 * trusting the output, the same way you'd verify the BM1397 framing.
 */

#include <string.h>
#include <stdlib.h>
#include "global_state.h"
#include "work_queue.h"
#include "bm1397.h"
#include "sha256_midstate.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "create_jobs";

static int hex_decode(const char *hex, uint8_t *out, int max_out)
{
    int len = (int)strlen(hex) / 2;
    if (len > max_out) len = max_out;
    for (int i = 0; i < len; i++) {
        unsigned int byte;
        sscanf(hex + i * 2, "%2x", &byte);
        out[i] = (uint8_t)byte;
    }
    return len;
}

static void hex_encode(const uint8_t *data, int len, char *out)
{
    for (int i = 0; i < len; i++) {
        sprintf(out + i * 2, "%02x", data[i]);
    }
    out[len * 2] = '\0';
}

static void reverse_bytes(uint8_t *data, int len)
{
    for (int i = 0; i < len / 2; i++) {
        uint8_t tmp = data[i];
        data[i] = data[len - 1 - i];
        data[len - 1 - i] = tmp;
    }
}

static void double_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    uint8_t first[32];
    sha256_full(data, len, first);
    sha256_full(first, 32, out);
}

/* Builds the merkle root from the coinbase hash + the branch hashes the
 * pool supplied, per the standard Bitcoin merkle-path algorithm: at each
 * level, concatenate the running hash with the next branch hash and
 * double-SHA256 the pair. */
static void compute_merkle_root(const uint8_t coinbase_hash[32], const mining_notify_t *notify,
                                 uint8_t merkle_root_out[32])
{
    uint8_t acc[32];
    memcpy(acc, coinbase_hash, 32);

    for (int i = 0; i < notify->merkle_branch_count; i++) {
        uint8_t branch[32];
        hex_decode(notify->merkle_branches[i], branch, 32);

        uint8_t pair[64];
        memcpy(pair, acc, 32);
        memcpy(pair + 32, branch, 32);
        double_sha256(pair, 64, acc);
    }
    memcpy(merkle_root_out, acc, 32);
}

static bool build_job(const mining_notify_t *notify, uint32_t extranonce2_counter, bm1397_job_t *out)
{
    char extranonce2_hex[17] = {0};
    int en2_len = g_state.extranonce2_len > 0 ? g_state.extranonce2_len : 4;
    /* Represent the counter as a big-endian hex string of the pool-assigned
     * length - simple incrementing counter, unique per job as long as we
     * don't wrap within a single pool session (32-bit counter is generous
     * for that). */
    char counter_hex[9];
    snprintf(counter_hex, sizeof(counter_hex), "%08x", extranonce2_counter);
    int pad = en2_len * 2 - (int)strlen(counter_hex);
    if (pad < 0) pad = 0;
    memset(extranonce2_hex, '0', pad);
    strncpy(extranonce2_hex + pad, counter_hex, sizeof(extranonce2_hex) - pad - 1);

    /* ---- Coinbase = coinbase1 + extranonce1 + extranonce2 + coinbase2 ---- */
    uint8_t coinbase[512];
    int off = 0;
    off += hex_decode(notify->coinbase1, coinbase + off, sizeof(coinbase) - off);
    off += hex_decode(g_state.extranonce1, coinbase + off, sizeof(coinbase) - off);
    off += hex_decode(extranonce2_hex, coinbase + off, sizeof(coinbase) - off);
    off += hex_decode(notify->coinbase2, coinbase + off, sizeof(coinbase) - off);

    uint8_t coinbase_hash[32];
    double_sha256(coinbase, off, coinbase_hash);

    uint8_t merkle_root[32];
    compute_merkle_root(coinbase_hash, notify, merkle_root);

    /* ---- Assemble the 80-byte header ---- */
    uint8_t header[80];
    uint8_t version_bytes[4], prevhash_bytes[32], ntime_bytes[4], nbits_bytes[4];
    hex_decode(notify->version, version_bytes, 4);
    hex_decode(notify->prevhash, prevhash_bytes, 32);
    hex_decode(notify->ntime, ntime_bytes, 4);
    hex_decode(notify->nbits, nbits_bytes, 4);

    /* Stratum sends prevhash as 8 little-endian 32-bit words; the header
     * wants it as the raw internal hash byte order, which means
     * reversing each 4-byte word in place (not reversing the whole 32
     * bytes as one block). */
    for (int w = 0; w < 8; w++) {
        reverse_bytes(prevhash_bytes + w * 4, 4);
    }

    memcpy(header, version_bytes, 4);
    memcpy(header + 4, prevhash_bytes, 32);
    memcpy(header + 36, merkle_root, 32);
    memcpy(header + 68, ntime_bytes, 4);
    memcpy(header + 72, nbits_bytes, 4);
    memset(header + 76, 0, 4); /* nonce - the ASIC fills this in, starts at 0 */

    sha256_midstate(header, out->midstate);
    memcpy(out->merkle_root_tail, header + 64, 4); /* last 4 bytes of merkle_root */
    memcpy(out->ntime, ntime_bytes, 4);
    memcpy(out->nbits, nbits_bytes, 4);
    out->starting_nonce = 0;

    xSemaphoreTake(g_state.mutex, portMAX_DELAY);
    out->job_id = g_state.next_job_id;
    bm_job_t *slot = &g_state.active_jobs[out->job_id];
    slot->valid = true;
    slot->extranonce2 = extranonce2_counter;
    memcpy(slot->ntime, ntime_bytes, 4);
    memcpy(slot->nbits, nbits_bytes, 4);
    memcpy(slot->merkle_root, merkle_root, 32);
    memcpy(slot->midstate, out->midstate, 32);
    slot->job_difficulty = g_state.pool_difficulty;
    strncpy(slot->pool_job_id, notify->job_id, sizeof(slot->pool_job_id) - 1);
    g_state.next_job_id = BM1397_NEXT_JOB_ID(g_state.next_job_id);
    xSemaphoreGive(g_state.mutex);

    return true;
}

void create_jobs_task(void *pvParameters)
{
    work_queue_t *queue = (work_queue_t *)pvParameters;
    uint32_t extranonce2_counter = 0;

    ESP_LOGI(TAG, "create_jobs_task started");

    while (1) {
        mining_notify_t notify;
        if (!work_queue_dequeue_timeout(queue, &notify, 1000)) {
            continue;
        }

        bool asic_ready;
        xSemaphoreTake(g_state.mutex, portMAX_DELAY);
        asic_ready = g_state.asic_initialized;
        xSemaphoreGive(g_state.mutex);
        if (!asic_ready) {
            /* Don't burn jobs while the ASIC is mid-reinit (e.g. overheat
             * recovery in power_management_task.c) - matches the documented
             * behavior of pausing job creation until ASIC_initialized is
             * true again. */
            continue;
        }

        bm1397_job_t job;
        if (build_job(&notify, extranonce2_counter++, &job)) {
            bm1397_send_job(&job);
        }
    }
}
