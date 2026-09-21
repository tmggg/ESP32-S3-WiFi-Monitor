idf_component_register(
    SRCS
        "main.c"
        "wifi_manager.c"
        "provisioning_portal.c"
        "boot_button.c"
        "board_display.c"
        "status_dashboard.c"
        "openwrt_status.c"
        "cpu_load_led.c"
        "Vernon_ST7789T/Vernon_ST7789T.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_http_server esp_http_client
             esp_lcd driver esp_timer esp_system esp_psram heap lwip json lvgl led_strip
)
