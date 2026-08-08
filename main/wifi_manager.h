/**
 * wifi_manager.h — standard ESP-IDF Wi-Fi provisioning shape: if no
 * credentials are saved (or the saved ones fail to connect), start a
 * SoftAP the user can join to submit new credentials via the web
 * dashboard's setup form; otherwise connect in station mode using the
 * saved credentials. This part of the project is ordinary, extremely
 * well-documented ESP-IDF usage - nothing board- or BM1397-specific.
 */

#pragma once

#include <stdbool.h>

#define WIFI_AP_SSID_PREFIX "n8t-"   /* full SSID gets a MAC-derived suffix appended */

void wifi_manager_start(void);
bool wifi_manager_is_ap_mode(void);
bool wifi_manager_connect_sta(const char *ssid, const char *password);
