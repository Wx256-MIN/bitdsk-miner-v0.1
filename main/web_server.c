/**
 * web_server.c — see header.
 */

#include <string.h>
#include "web_server.h"
#include "global_state.h"
#include "nvs_config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "web_server";

/* Embedded via EMBED_FILES in main/CMakeLists.txt - see that file. */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    size_t len = index_html_end - index_html_start;
    httpd_resp_send(req, (const char *)index_html_start, len);
    return ESP_OK;
}

static esp_err_t handle_system_info(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    xSemaphoreTake(g_state.mutex, portMAX_DELAY);
    cJSON_AddStringToObject(root, "ASICModel", "BM1397");
    cJSON_AddNumberToObject(root, "hashRate", g_state.expected_hashrate_ghs);
    cJSON_AddNumberToObject(root, "temp", g_state.chip_temp_c);
    cJSON_AddNumberToObject(root, "vrTemp", g_state.vr_temp_c);
    cJSON_AddNumberToObject(root, "frequency", g_state.current_frequency_mhz);
    cJSON_AddNumberToObject(root, "coreVoltage", g_state.current_voltage_mv);
    cJSON_AddNumberToObject(root, "power", g_state.power_watts);
    cJSON_AddNumberToObject(root, "voltage", g_state.input_voltage_mv);
    cJSON_AddNumberToObject(root, "current", g_state.current_ma);
    cJSON_AddNumberToObject(root, "fanPercent", g_state.fan_percent);
    cJSON_AddNumberToObject(root, "fanRpm", g_state.fan_rpm);
    cJSON_AddBoolToObject(root, "wifiConnected", g_state.wifi_connected);
    cJSON_AddStringToObject(root, "ipAddress", g_state.ip_addr);
    cJSON_AddBoolToObject(root, "stratumConnected", g_state.stratum_connected);
    cJSON_AddNumberToObject(root, "poolDifficulty", g_state.pool_difficulty);
    cJSON_AddNumberToObject(root, "sharesAccepted", (double)g_state.shares_accepted);
    cJSON_AddNumberToObject(root, "sharesRejected", (double)g_state.shares_rejected);
    cJSON_AddNumberToObject(root, "bestDifficulty", g_state.best_difficulty);
    cJSON_AddBoolToObject(root, "overheatMode", g_state.overheat_mode);
    cJSON_AddBoolToObject(root, "asicInitialized", g_state.asic_initialized);
    xSemaphoreGive(g_state.mutex);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t handle_system_config_post(httpd_req_t *req)
{
    char buf[512];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    cJSON *body = cJSON_Parse(buf);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ssid = cJSON_GetObjectItem(body, "ssid");
    cJSON *pass = cJSON_GetObjectItem(body, "password");
    cJSON *pool_host = cJSON_GetObjectItem(body, "poolHost");
    cJSON *pool_port = cJSON_GetObjectItem(body, "poolPort");
    cJSON *pool_user = cJSON_GetObjectItem(body, "poolUser");

    if (cJSON_IsString(ssid) && cJSON_IsString(pass)) {
        nvs_config_set_wifi(ssid->valuestring, pass->valuestring);
    }
    if (cJSON_IsString(pool_host) && cJSON_IsNumber(pool_port) && cJSON_IsString(pool_user)) {
        nvs_config_set_pool(pool_host->valuestring, pool_port->valueint, pool_user->valuestring);
    }
    cJSON_Delete(body);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"result\":\"saved, restarting\"}", HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handle_restart(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"result\":\"restarting\"}", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

void web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start httpd");
        return;
    }

    httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = handle_root };
    httpd_uri_t info_uri = { .uri = "/api/system/info", .method = HTTP_GET, .handler = handle_system_info };
    httpd_uri_t config_uri = { .uri = "/api/system", .method = HTTP_POST, .handler = handle_system_config_post };
    httpd_uri_t restart_uri = { .uri = "/api/system/restart", .method = HTTP_POST, .handler = handle_restart };

    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &info_uri);
    httpd_register_uri_handler(server, &config_uri);
    httpd_register_uri_handler(server, &restart_uri);

    ESP_LOGI(TAG, "web server started");
}
