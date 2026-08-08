/**
 * nvs_config.c — see header. All values live in a single "n8tcfg"
 * namespace for simplicity.
 */

#include <string.h>
#include "nvs_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "nvs_config";
#define NAMESPACE "n8tcfg"

bool nvs_config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase (err=0x%x), erasing and retrying", err);
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    return err == ESP_OK;
}

static bool open_ns(nvs_handle_t *h, bool write)
{
    esp_err_t err = nvs_open(NAMESPACE, write ? NVS_READWRITE : NVS_READONLY, h);
    return err == ESP_OK;
}

bool nvs_config_get_wifi(char *ssid_out, size_t ssid_len, char *pass_out, size_t pass_len)
{
    nvs_handle_t h;
    if (!open_ns(&h, false)) return false;
    size_t sl = ssid_len, pl = pass_len;
    bool ok = nvs_get_str(h, "wifi_ssid", ssid_out, &sl) == ESP_OK &&
              nvs_get_str(h, "wifi_pass", pass_out, &pl) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool nvs_config_set_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (!open_ns(&h, true)) return false;
    bool ok = nvs_set_str(h, "wifi_ssid", ssid) == ESP_OK &&
              nvs_set_str(h, "wifi_pass", pass) == ESP_OK &&
              nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool nvs_config_get_pool(char *host_out, size_t host_len, int *port_out,
                          char *user_out, size_t user_len)
{
    nvs_handle_t h;
    if (!open_ns(&h, false)) return false;
    size_t hl = host_len, ul = user_len;
    int32_t port = 0;
    bool ok = nvs_get_str(h, "pool_host", host_out, &hl) == ESP_OK &&
              nvs_get_i32(h, "pool_port", &port) == ESP_OK &&
              nvs_get_str(h, "pool_user", user_out, &ul) == ESP_OK;
    if (ok) *port_out = (int)port;
    nvs_close(h);
    return ok;
}

bool nvs_config_set_pool(const char *host, int port, const char *user)
{
    nvs_handle_t h;
    if (!open_ns(&h, true)) return false;
    bool ok = nvs_set_str(h, "pool_host", host) == ESP_OK &&
              nvs_set_i32(h, "pool_port", port) == ESP_OK &&
              nvs_set_str(h, "pool_user", user) == ESP_OK &&
              nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool nvs_config_get_operating_point(float *freq_mhz_out, int *voltage_mv_out)
{
    nvs_handle_t h;
    if (!open_ns(&h, false)) return false;
    int32_t freq_x10 = 0, voltage = 0;
    bool ok = nvs_get_i32(h, "freq_x10", &freq_x10) == ESP_OK &&
              nvs_get_i32(h, "voltage_mv", &voltage) == ESP_OK;
    if (ok) {
        *freq_mhz_out = freq_x10 / 10.0f;
        *voltage_mv_out = (int)voltage;
    }
    nvs_close(h);
    return ok;
}

bool nvs_config_save_operating_point(float freq_mhz, int voltage_mv)
{
    nvs_handle_t h;
    if (!open_ns(&h, true)) return false;
    bool ok = nvs_set_i32(h, "freq_x10", (int32_t)(freq_mhz * 10)) == ESP_OK &&
              nvs_set_i32(h, "voltage_mv", (int32_t)voltage_mv) == ESP_OK &&
              nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool nvs_config_get_overheat_mode(void)
{
    nvs_handle_t h;
    if (!open_ns(&h, false)) return false;
    uint8_t val = 0;
    bool ok = nvs_get_u8(h, "overheat", &val) == ESP_OK;
    nvs_close(h);
    return ok && val != 0;
}

bool nvs_config_set_overheat_mode(bool active)
{
    nvs_handle_t h;
    if (!open_ns(&h, true)) return false;
    bool ok = nvs_set_u8(h, "overheat", active ? 1 : 0) == ESP_OK &&
              nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}
