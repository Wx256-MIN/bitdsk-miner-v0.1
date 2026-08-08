/**
 * main.c
 *
 * Startup sequence:
 *   1. NVS + shared state init.
 *   2. Wi-Fi (AP for first-time setup, or STA using saved credentials).
 *   3. Web server (always - the dashboard needs to work in both AP setup
 *      mode and normal STA operation).
 *   4. If actually on a network: bring up the ASIC and start mining.
 *
 * Task priorities mirror the documented ESP-Miner architecture (higher
 * number = higher priority in FreeRTOS): job creation is the most
 * latency-sensitive (stale work wastes hashrate), result handling next,
 * then power/thermal safety, then the network-facing stratum task, which
 * spends most of its time blocked on socket reads anyway.
 */

#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_config.h"
#include "global_state.h"
#include "board_config.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "bm1397.h"
#include "stratum.h"
#include "work_queue.h"
#include "power_management_task.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";
static work_queue_t s_work_queue;

extern void create_jobs_task(void *pvParameters);
extern void asic_result_task(void *pvParameters);

static void stratum_task(void *pvParameters)
{
    char host[128], user[128];
    int port = 0;

    if (!nvs_config_get_pool(host, sizeof(host), &port, user, sizeof(user)) || port == 0) {
        ESP_LOGE(TAG, "no pool configured - open the dashboard and fill in the pool settings form");
        vTaskDelete(NULL);
        return;
    }

    stratum_config_t cfg = { .pool_port = port, .password = "x" };
    strncpy(cfg.pool_host, host, sizeof(cfg.pool_host) - 1);
    strncpy(cfg.username, user, sizeof(cfg.username) - 1);

    while (1) {
        ESP_LOGI(TAG, "connecting to pool %s:%d", cfg.pool_host, cfg.pool_port);
        if (stratum_connect(&cfg) && stratum_subscribe_and_authorize(&cfg)) {
            stratum_run(&s_work_queue); /* blocks until the connection drops */
        }
        ESP_LOGW(TAG, "stratum connection lost, retrying in 5s");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void start_mining(void)
{
    work_queue_init(&s_work_queue);

    /* Release the ASIC from reset before talking to it. Power sequencing
     * (bringing the core voltage rail up cleanly) belongs here too once
     * vcore_set_voltage_mv() in power_management_task.c is implemented
     * against real hardware - for now this only handles the reset line. */
    gpio_set_direction(PIN_ASIC_RESET, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_ASIC_RESET, 1); /* release reset - active-low assumption, verify */
    vTaskDelay(pdMS_TO_TICKS(200));

    float freq = ASIC_DEFAULT_FREQUENCY_MHZ;
    int voltage = ASIC_DEFAULT_VOLTAGE_MV;
    nvs_config_get_operating_point(&freq, &voltage); /* falls back to the passed-in defaults if unset */

    bool asic_ok = bm1397_init(ASIC_CHIP_COUNT, freq, (uint16_t)voltage);
    xSemaphoreTake(g_state.mutex, portMAX_DELAY);
    g_state.asic_initialized = asic_ok;
    g_state.current_frequency_mhz = freq;
    g_state.current_voltage_mv = voltage;
    xSemaphoreGive(g_state.mutex);

    if (!asic_ok) {
        ESP_LOGE(TAG, "ASIC init failed - check board_config.h pin assignments against your actual hardware. "
                      "The rest of the system (Wi-Fi, dashboard, pool connection) will still run so you can "
                      "debug without needing to re-flash.");
    }

    xTaskCreate(create_jobs_task, "create_jobs", 8192, &s_work_queue, 20, NULL);
    xTaskCreate(asic_result_task, "asic_result", 8192, NULL, 15, NULL);
    xTaskCreate(power_management_task, "power_mgmt", 4096, NULL, 10, NULL);
    xTaskCreate(stratum_task, "stratum", 8192, NULL, 5, NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "N8-T firmware starting (reference build - see README before flashing to real hardware)");

    if (!nvs_config_init()) {
        ESP_LOGE(TAG, "NVS init failed");
    }
    global_state_init();

    wifi_manager_start();
    web_server_start();

    if (wifi_manager_is_ap_mode()) {
        ESP_LOGI(TAG, "in AP setup mode - waiting for Wi-Fi/pool configuration via the dashboard");
        /* Nothing else to do here; the web server's config handler calls
         * esp_restart() once the user submits the setup form. */
        return;
    }

    if (nvs_config_get_overheat_mode()) {
        ESP_LOGW(TAG, "booting after a recorded overheat event - starting mining tasks anyway; "
                      "power_management_task will re-apply reduced settings from NVS.");
    }

    start_mining();
}
