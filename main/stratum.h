/**
 * stratum.h
 *
 * Stratum V1 is a plain, publicly documented, newline-delimited JSON-RPC
 * protocol that's been the mining industry standard since ~2012 - unlike
 * the BM1397 UART protocol, there's nothing proprietary or reverse-
 * engineered to guess at here. This client implements the handful of
 * methods a solo/pool miner actually needs: mining.subscribe,
 * mining.authorize, mining.notify, mining.set_difficulty,
 * mining.set_version_mask, and mining.submit.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "work_queue.h"

typedef struct {
    char pool_host[128];
    int  pool_port;
    char username[128];   /* wallet address (or wallet.workername) as stratum user */
    char password[64];    /* usually "x", pools generally ignore this */
} stratum_config_t;

bool stratum_connect(const stratum_config_t *cfg);
void stratum_disconnect(void);
bool stratum_subscribe_and_authorize(const stratum_config_t *cfg);

/* Blocks reading the socket and dispatching messages (queueing
 * mining.notify into `queue`, updating GlobalState for difficulty/
 * version-mask/connection-state changes) until the connection drops.
 * Intended to be called in a loop from stratum_task. */
void stratum_run(work_queue_t *queue);

bool stratum_submit_share(const char *job_id, const char *extranonce2_hex,
                           const char *ntime_hex, uint32_t nonce, uint32_t version_bits);

bool stratum_is_connected(void);
