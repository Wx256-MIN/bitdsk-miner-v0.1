/**
 * wifi_manager.c — see header.
 */

#include <string.h>
#include <stdio.h>
#include "wifi_manager.h"
#include "global_state.h"
#include "nvs_config.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi_mgr";
static bool s_ap_mode = false;
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "STA disconnected, retrying");
        xSemaphoreTake(g_state.mutex, portMAX_DELAY);
        g_state.wifi_connected = false;
        xSemaphoreGive(g_state.mutex);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip_info.ip));

        xSemaphoreTake(g_state.mutex, portMAX_DELAY);
        g_state.wifi_connected = true;
        strncpy(g_state.ip_addr, ip_str, sizeof(g_state.ip_addr) - 1);
        xSemaphoreGive(g_state.mutex);

        ESP_LOGI(TAG, "got IP: %s", ip_str);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void start_ap_mode(void)
{
    s_ap_mode = true;
    esp_netif_create_default_wifi_ap();

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "%s%02x%02x%02x", WIFI_AP_SSID_PREFIX, mac[3], mac[4], mac[5]);

    wifi_config_t ap_config = {0};
    strncpy((char *)ap_config.ap.ssid, ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN; /* open setup network, matches how these devices are commonly provisioned */

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "AP mode: join \"%s\" and open http://192.168.4.1/ to configure Wi-Fi + pool", ssid);
}

bool wifi_manager_connect_sta(const char *ssid, const char *password)
{
    esp_netif_create_default_wifi_sta();

    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    esp_wifi_start();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

void wifi_manager_start(void)
{
    s_wifi_event_group = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL);

    char ssid[64] = {0}, pass[64] = {0};
    if (nvs_config_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass)) && strlen(ssid) > 0) {
        ESP_LOGI(TAG, "found saved Wi-Fi credentials for \"%s\", attempting STA connect", ssid);
        if (wifi_manager_connect_sta(ssid, pass)) {
            s_ap_mode = false;
            return;
        }
        ESP_LOGW(TAG, "saved credentials failed to connect, falling back to AP mode");
    } else {
        ESP_LOGI(TAG, "no saved Wi-Fi credentials, starting AP mode for first-time setup");
    }

    start_ap_mode();
}

bool wifi_manager_is_ap_mode(void)
{
    return s_ap_mode;
}
