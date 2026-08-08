idf_component_register(
    SRCS
        "main.c"
        "global_state.c"
        "bm1397.c"
        "serial.c"
        "sha256_midstate.c"
        "work_queue.c"
        "stratum.c"
        "create_jobs_task.c"
        "asic_result_task.c"
        "power_management_task.c"
        "wifi_manager.c"
        "web_server.c"
        "nvs_config.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_http_server
        json
        lwip
        esp_system
    EMBED_FILES "webui/index.html"
)
