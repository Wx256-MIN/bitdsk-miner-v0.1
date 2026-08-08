/**
 * work_queue.c — see header. Plain circular-buffer FIFO guarded by a
 * mutex, with a counting semaphore so dequeue can block with a timeout
 * instead of busy-polling.
 */

#include <string.h>
#include "work_queue.h"

void work_queue_init(work_queue_t *q)
{
    memset(q, 0, sizeof(*q));
    q->mutex = xSemaphoreCreateMutex();
    q->items_available = xSemaphoreCreateCounting(WORK_QUEUE_SIZE, 0);
}

void work_queue_clear(work_queue_t *q)
{
    xSemaphoreTake(q->mutex, portMAX_DELAY);
    while (q->count > 0) {
        xSemaphoreTake(q->items_available, 0);
        q->count--;
    }
    q->head = 0;
    q->tail = 0;
    xSemaphoreGive(q->mutex);
}

bool work_queue_enqueue(work_queue_t *q, const mining_notify_t *item)
{
    xSemaphoreTake(q->mutex, portMAX_DELAY);
    if (q->count == WORK_QUEUE_SIZE) {
        /* Drop the oldest to make room, matching the documented
         * "drop oldest if full" behavior - a full queue means job
         * construction is falling behind, and the newest work is what
         * actually matters (older notifies are usually superseded). */
        q->head = (q->head + 1) % WORK_QUEUE_SIZE;
        q->count--;
        xSemaphoreTake(q->items_available, 0);
    }
    q->items[q->tail] = *item;
    q->tail = (q->tail + 1) % WORK_QUEUE_SIZE;
    q->count++;
    xSemaphoreGive(q->mutex);
    xSemaphoreGive(q->items_available);
    return true;
}

bool work_queue_dequeue_timeout(work_queue_t *q, mining_notify_t *out, uint32_t timeout_ms)
{
    if (xSemaphoreTake(q->items_available, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    xSemaphoreTake(q->mutex, portMAX_DELAY);
    *out = q->items[q->head];
    q->head = (q->head + 1) % WORK_QUEUE_SIZE;
    q->count--;
    xSemaphoreGive(q->mutex);
    return true;
}
