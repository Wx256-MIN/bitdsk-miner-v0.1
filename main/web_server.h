/**
 * web_server.h — a small REST API (JSON) plus the single-page dashboard,
 * served with ESP-IDF's standard esp_http_server. This is the part of
 * the project that stands in for AxeOS's web UI - deliberately much
 * simpler than AxeOS's real Angular app (see webui/index.html), but the
 * same basic idea: one page showing live hashrate/temp/shares, plus a
 * settings form for Wi-Fi and pool configuration.
 */

#pragma once

void web_server_start(void);
