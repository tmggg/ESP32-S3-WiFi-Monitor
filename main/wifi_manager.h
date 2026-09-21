#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

typedef enum {
    WIFI_MANAGER_IDLE,
    WIFI_MANAGER_CONNECTING,
    WIFI_MANAGER_CONNECTED,
    WIFI_MANAGER_PROVISIONING,
    WIFI_MANAGER_FAILED,
} wifi_manager_state_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    wifi_auth_mode_t authmode;
} wifi_manager_ap_t;

esp_err_t wifi_manager_init(void);
bool wifi_manager_has_credentials(void);
esp_err_t wifi_manager_connect_saved(void);
esp_err_t wifi_manager_start_provisioning(void);
esp_err_t wifi_manager_submit_credentials(const char *ssid, const char *password,
                                          wifi_auth_mode_t authmode,
                                          const char *status_token,
                                          const char *monitor_ip);
esp_err_t wifi_manager_scan(wifi_manager_ap_t **records, uint16_t *count);
void wifi_manager_free_scan(wifi_manager_ap_t *records);
esp_err_t wifi_manager_clear_credentials(void);
wifi_manager_state_t wifi_manager_get_state(void);
void wifi_manager_get_status(char *buffer, size_t length);
const char *wifi_manager_get_ap_ssid(void);
const char *wifi_manager_auth_name(wifi_auth_mode_t authmode);
bool wifi_manager_get_gateway(char *buffer, size_t length);
bool wifi_manager_get_status_token(char *buffer, size_t length);
bool wifi_manager_get_monitor_ip(char *buffer, size_t length);
