/**
 * stratum.c — see header. Standard Stratum V1 over a raw TCP socket
 * (lwip BSD sockets), newline-delimited JSON-RPC (cJSON).
 */

#include <string.h>
#include <stdio.h>
#include "stratum.h"
#include "global_state.h"
#include "esp_log.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "stratum";

static int  s_sock = -1;
static char s_username[128] = {0};

#define LINE_BUF_SIZE 4096
static char   s_linebuf[LINE_BUF_SIZE];
static size_t s_linebuf_len = 0;

static bool try_extract_line(char *out, size_t out_size)
{
    for (size_t i = 0; i < s_linebuf_len; i++) {
        if (s_linebuf[i] == '\n') {
            size_t line_len = i;
            if (line_len >= out_size) line_len = out_size - 1;
            memcpy(out, s_linebuf, line_len);
            out[line_len] = '\0';
            size_t remaining = s_linebuf_len - (i + 1);
            memmove(s_linebuf, s_linebuf + i + 1, remaining);
            s_linebuf_len = remaining;
            return true;
        }
    }
    return false;
}

static bool stratum_read_line(char *out, size_t out_size, uint32_t timeout_ms)
{
    if (try_extract_line(out, out_size)) return true;
    if (s_sock < 0) return false;

    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (1) {
        if (s_linebuf_len >= LINE_BUF_SIZE - 1) {
            ESP_LOGW(TAG, "line buffer overflow, resetting");
            s_linebuf_len = 0;
            return false;
        }
        int n = recv(s_sock, s_linebuf + s_linebuf_len, LINE_BUF_SIZE - 1 - s_linebuf_len, 0);
        if (n <= 0) return false;
        s_linebuf_len += (size_t)n;
        if (try_extract_line(out, out_size)) return true;
    }
}

static bool send_line(const char *json_line)
{
    if (s_sock < 0) return false;
    char buf[1024];
    int len = snprintf(buf, sizeof(buf), "%s\n", json_line);
    if (len <= 0 || len >= (int)sizeof(buf)) return false;
    return send(s_sock, buf, len, 0) == len;
}

bool stratum_connect(const stratum_config_t *cfg)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", cfg->pool_port);

    if (getaddrinfo(cfg->pool_host, port_str, &hints, &res) != 0 || !res) {
        ESP_LOGE(TAG, "DNS lookup failed for %s", cfg->pool_host);
        return false;
    }

    s_sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s_sock < 0) {
        freeaddrinfo(res);
        return false;
    }

    if (connect(s_sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "connect() failed to %s:%d", cfg->pool_host, cfg->pool_port);
        close(s_sock);
        s_sock = -1;
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);
    s_linebuf_len = 0;
    ESP_LOGI(TAG, "connected to %s:%d", cfg->pool_host, cfg->pool_port);
    return true;
}

void stratum_disconnect(void)
{
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
}

bool stratum_is_connected(void)
{
    return s_sock >= 0;
}

bool stratum_subscribe_and_authorize(const stratum_config_t *cfg)
{
    strncpy(s_username, cfg->username, sizeof(s_username) - 1);

    char req[256];
    snprintf(req, sizeof(req),
             "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[\"n8t-firmware/0.1\"]}");
    if (!send_line(req)) return false;

    char line[2048];
    if (!stratum_read_line(line, sizeof(line), 5000)) {
        ESP_LOGE(TAG, "no response to mining.subscribe");
        return false;
    }

    cJSON *resp = cJSON_Parse(line);
    if (!resp) return false;
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    if (!cJSON_IsArray(result) || cJSON_GetArraySize(result) < 3) {
        ESP_LOGE(TAG, "unexpected subscribe response: %s", line);
        cJSON_Delete(resp);
        return false;
    }
    const char *extranonce1 = cJSON_GetArrayItem(result, 1)->valuestring;
    int extranonce2_len = cJSON_GetArrayItem(result, 2)->valueint;

    xSemaphoreTake(g_state.mutex, portMAX_DELAY);
    strncpy(g_state.extranonce1, extranonce1, sizeof(g_state.extranonce1) - 1);
    g_state.extranonce2_len = extranonce2_len;
    xSemaphoreGive(g_state.mutex);
    cJSON_Delete(resp);

    snprintf(req, sizeof(req),
             "{\"id\":2,\"method\":\"mining.authorize\",\"params\":[\"%s\",\"%s\"]}",
             cfg->username, cfg->password[0] ? cfg->password : "x");
    if (!send_line(req)) return false;

    if (!stratum_read_line(line, sizeof(line), 5000)) {
        ESP_LOGE(TAG, "no response to mining.authorize");
        return false;
    }
    resp = cJSON_Parse(line);
    bool authorized = resp && cJSON_IsTrue(cJSON_GetObjectItem(resp, "result"));
    if (resp) cJSON_Delete(resp);

    if (!authorized) {
        ESP_LOGE(TAG, "pool rejected authorization for user %s", cfg->username);
        return false;
    }

    xSemaphoreTake(g_state.mutex, portMAX_DELAY);
    g_state.stratum_connected = true;
    g_state.stratum_authorized = true;
    xSemaphoreGive(g_state.mutex);

    ESP_LOGI(TAG, "subscribed + authorized as %s", cfg->username);
    return true;
}

static void handle_notify(cJSON *params, work_queue_t *queue)
{
    if (!cJSON_IsArray(params) || cJSON_GetArraySize(params) < 9) return;

    mining_notify_t job = {0};
    strncpy(job.job_id, cJSON_GetArrayItem(params, 0)->valuestring, sizeof(job.job_id) - 1);
    strncpy(job.prevhash, cJSON_GetArrayItem(params, 1)->valuestring, sizeof(job.prevhash) - 1);
    strncpy(job.coinbase1, cJSON_GetArrayItem(params, 2)->valuestring, sizeof(job.coinbase1) - 1);
    strncpy(job.coinbase2, cJSON_GetArrayItem(params, 3)->valuestring, sizeof(job.coinbase2) - 1);

    cJSON *branches = cJSON_GetArrayItem(params, 4);
    job.merkle_branch_count = 0;
    if (cJSON_IsArray(branches)) {
        int n = cJSON_GetArraySize(branches);
        if (n > 16) n = 16;
        for (int i = 0; i < n; i++) {
            strncpy(job.merkle_branches[i], cJSON_GetArrayItem(branches, i)->valuestring, 64);
        }
        job.merkle_branch_count = n;
    }

    strncpy(job.version, cJSON_GetArrayItem(params, 5)->valuestring, sizeof(job.version) - 1);
    strncpy(job.nbits, cJSON_GetArrayItem(params, 6)->valuestring, sizeof(job.nbits) - 1);
    strncpy(job.ntime, cJSON_GetArrayItem(params, 7)->valuestring, sizeof(job.ntime) - 1);
    cJSON *clean = cJSON_GetArrayItem(params, 8);
    job.clean_jobs = cJSON_IsTrue(clean);

    if (job.clean_jobs) {
        work_queue_clear(queue);
    }
    work_queue_enqueue(queue, &job);
}

static void handle_set_difficulty(cJSON *params)
{
    if (!cJSON_IsArray(params) || cJSON_GetArraySize(params) < 1) return;
    double diff = cJSON_GetArrayItem(params, 0)->valuedouble;
    xSemaphoreTake(g_state.mutex, portMAX_DELAY);
    g_state.pool_difficulty = (uint32_t)diff;
    xSemaphoreGive(g_state.mutex);
    ESP_LOGI(TAG, "pool set difficulty = %.0f", diff);
}

static void handle_set_version_mask(cJSON *params)
{
    if (!cJSON_IsArray(params) || cJSON_GetArraySize(params) < 1) return;
    uint32_t mask = (uint32_t)strtoul(cJSON_GetArrayItem(params, 0)->valuestring, NULL, 16);
    xSemaphoreTake(g_state.mutex, portMAX_DELAY);
    g_state.version_mask = mask;
    xSemaphoreGive(g_state.mutex);
    /* Note: ESP-Miner's docs list BM1397 as NOT supporting version rolling,
     * unlike the newer chips it also drives - so this mask may need to be
     * tracked without ever being pushed down to the ASIC on this board. */
}

void stratum_run(work_queue_t *queue)
{
    char line[4096];
    while (stratum_is_connected()) {
        if (!stratum_read_line(line, sizeof(line), 30000)) {
            ESP_LOGW(TAG, "stratum read timeout/closed, disconnecting");
            break;
        }

        cJSON *msg = cJSON_Parse(line);
        if (!msg) continue;

        cJSON *method = cJSON_GetObjectItem(msg, "method");
        if (cJSON_IsString(method)) {
            cJSON *params = cJSON_GetObjectItem(msg, "params");
            if (strcmp(method->valuestring, "mining.notify") == 0) {
                handle_notify(params, queue);
            } else if (strcmp(method->valuestring, "mining.set_difficulty") == 0) {
                handle_set_difficulty(params);
            } else if (strcmp(method->valuestring, "mining.set_version_mask") == 0) {
                handle_set_version_mask(params);
            } else {
                ESP_LOGD(TAG, "unhandled method: %s", method->valuestring);
            }
        } else {
            /* A response to something we sent (most likely mining.submit -
             * subscribe/authorize responses are consumed synchronously in
             * stratum_subscribe_and_authorize() before this loop starts). */
            cJSON *result = cJSON_GetObjectItem(msg, "result");
            bool accepted = cJSON_IsTrue(result);
            xSemaphoreTake(g_state.mutex, portMAX_DELAY);
            if (accepted) g_state.shares_accepted++;
            else          g_state.shares_rejected++;
            xSemaphoreGive(g_state.mutex);
            if (!accepted) {
                cJSON *err = cJSON_GetObjectItem(msg, "error");
                char *err_str = err ? cJSON_PrintUnformatted(err) : NULL;
                ESP_LOGW(TAG, "share rejected: %s", err_str ? err_str : "unknown reason");
                if (err_str) free(err_str);
            }
        }
        cJSON_Delete(msg);
    }

    xSemaphoreTake(g_state.mutex, portMAX_DELAY);
    g_state.stratum_connected = false;
    g_state.stratum_authorized = false;
    xSemaphoreGive(g_state.mutex);
    stratum_disconnect();
    work_queue_clear(queue);
}

bool stratum_submit_share(const char *job_id, const char *extranonce2_hex,
                           const char *ntime_hex, uint32_t nonce, uint32_t version_bits)
{
    static uint32_t s_submit_id = 100;
    char nonce_hex[9];
    snprintf(nonce_hex, sizeof(nonce_hex), "%08x", nonce);

    char req[512];
    if (version_bits != 0) {
        char version_hex[9];
        snprintf(version_hex, sizeof(version_hex), "%08x", version_bits);
        snprintf(req, sizeof(req),
                 "{\"id\":%u,\"method\":\"mining.submit\",\"params\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"]}",
                 s_submit_id++, s_username, job_id, extranonce2_hex, ntime_hex, nonce_hex, version_hex);
    } else {
        snprintf(req, sizeof(req),
                 "{\"id\":%u,\"method\":\"mining.submit\",\"params\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"]}",
                 s_submit_id++, s_username, job_id, extranonce2_hex, ntime_hex, nonce_hex);
    }
    return send_line(req);
}
