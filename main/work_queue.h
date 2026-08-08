/**
 * work_queue.h — fixed-size FIFO that buffers incoming pool work
 * (mining.notify) so a burst of messages, or a slow job-construction
 * cycle, doesn't block the Stratum socket task. Mirrors the documented
 * stratum_queue role in ESP-Miner's architecture: decouple network I/O
 * from ASIC job creation so neither stalls the other.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define WORK_QUEUE_SIZE 8

typedef struct {
    char     job_id[64];
    char     prevhash[65];
    char     coinbase1[256];
    char     coinbase2[256];
    char     merkle_branches[16][65];
    int      merkle_branch_count;
    char     version[9];
    char     nbits[9];
    char     ntime[9];
    bool     clean_jobs;
} mining_notify_t;

typedef struct {
    mining_notify_t items[WORK_QUEUE_SIZE];
    int head;
    int tail;
    int count;
    SemaphoreHandle_t mutex;
    SemaphoreHandle_t items_available; /* counting semaphore for blocking dequeue */
} work_queue_t;

void work_queue_init(work_queue_t *q);
void work_queue_clear(work_queue_t *q);
bool work_queue_enqueue(work_queue_t *q, const mining_notify_t *item);
bool work_queue_dequeue_timeout(work_queue_t *q, mining_notify_t *out, uint32_t timeout_ms);
