/**
 * global_state.h
 *
 * A single shared state struct that every task reads/writes, following the
 * same general pattern documented for ESP-Miner's GlobalState: rather than
 * passing data through many small queues, the mining, power, and web-server
 * tasks share one struct and use FreeRTOS primitives (mutex here) to guard
 * concurrent access. Simpler to reason about at this project's scale than a
 * fully message-passing design.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define MAX_ACTIVE_JOBS 128   /* indexed by the 7-bit job_id space */

typedef struct {
    bool     valid;
    uint32_t extranonce2;
    uint8_t  ntime[4];
    uint8_t  nbits[4];
    uint8_t  merkle_root[32];
    uint8_t  midstate[32];
    uint32_t job_difficulty;
    char     pool_job_id[64];
} bm_job_t;

typedef struct {
    /* -- Wi-Fi / network -- */
    bool     wifi_connected;
    char     ip_addr[16];

    /* -- Stratum / pool -- */
    bool     stratum_connected;
    bool     stratum_authorized;
    uint32_t pool_difficulty;
    uint32_t version_mask;
    char     extranonce1[32];
    int      extranonce2_len;

    /* -- ASIC state -- */
    bool     asic_initialized;
    int      asic_chip_count_detected;
    float    current_frequency_mhz;
    int      current_voltage_mv;

    /* -- Job tracking (mirrors the documented active_jobs / valid_jobs
     *    pattern: index = job_id, so lookups on nonce return are O(1)) -- */
    bm_job_t active_jobs[MAX_ACTIVE_JOBS];
    uint8_t  next_job_id;

    /* -- Power / thermal (mirrors the documented PowerManagementModule
     *    fields so the web API and display can read a single struct) -- */
    float    fan_percent;
    uint16_t fan_rpm;
    float    chip_temp_c;
    float    vr_temp_c;
    float    input_voltage_mv;
    float    power_watts;
    float    current_ma;
    float    expected_hashrate_ghs;
    bool     overheat_mode;

    /* -- Live mining stats -- */
    uint64_t shares_accepted;
    uint64_t shares_rejected;
    double   best_difficulty;
    uint64_t total_nonces_received;
    int64_t  boot_time_unix;

    /* -- Guards concurrent access from stratum / mining / power / web
     *    tasks. Take it for the shortest time possible - never hold it
     *    across a blocking UART or network call. -- */
    SemaphoreHandle_t mutex;
} global_state_t;

extern global_state_t g_state;

void global_state_init(void);
