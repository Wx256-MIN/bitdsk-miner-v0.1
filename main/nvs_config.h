/**
 * nvs_config.h — persists user/runtime settings across reboots using
 * ESP-IDF's standard NVS (non-volatile storage) key-value API. Ordinary,
 * well-documented ESP-IDF usage throughout.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

bool nvs_config_init(void);

/* Wi-Fi */
bool nvs_config_get_wifi(char *ssid_out, size_t ssid_len, char *pass_out, size_t pass_len);
bool nvs_config_set_wifi(const char *ssid, const char *pass);

/* Pool */
bool nvs_config_get_pool(char *host_out, size_t host_len, int *port_out,
                          char *user_out, size_t user_len);
bool nvs_config_set_pool(const char *host, int port, const char *user);

/* ASIC operating point */
bool nvs_config_get_operating_point(float *freq_mhz_out, int *voltage_mv_out);
bool nvs_config_save_operating_point(float freq_mhz, int voltage_mv);

/* Overheat latch (survives reboot until explicitly cleared) */
bool nvs_config_get_overheat_mode(void);
bool nvs_config_set_overheat_mode(bool active);
