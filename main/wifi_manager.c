#include "wifi_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "provisioning_portal.h"

#define NVS_NAMESPACE "wifi_cfg"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASSWORD "password"
#define NVS_KEY_AUTH "auth"
#define NVS_KEY_STATUS_TOKEN "status_token"
#define NVS_KEY_MONITOR_IP "monitor_ip"
#define MAX_SCAN_RESULTS 30
#define CAPTIVE_PORTAL_URL "http://192.168.4.1/"

static const char *TAG = "wifi_manager";
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_scan_lock;
static SemaphoreHandle_t s_scan_done;
static wifi_manager_state_t s_state = WIFI_MANAGER_IDLE;
static char s_status[128] = "Starting";
static char s_ap_ssid[33];
static bool s_provisioning;
static bool s_pending_save;
static char s_pending_ssid[33];
static char s_pending_password[65];
static char s_pending_status_token[129];
static char s_pending_monitor_ip[16];
static wifi_auth_mode_t s_pending_auth;
static int s_retry_count;

static void set_status(wifi_manager_state_t state, const char *text)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = state;
    strlcpy(s_status, text, sizeof(s_status));
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%s", text);
}

static esp_err_t load_credentials(char *ssid, size_t ssid_len, char *password,
                                  size_t password_len, wifi_auth_mode_t *authmode)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;

    size_t length = ssid_len;
    err = nvs_get_str(nvs, NVS_KEY_SSID, ssid, &length);
    if (err == ESP_OK) {
        length = password_len;
        err = nvs_get_str(nvs, NVS_KEY_PASSWORD, password, &length);
    }
    if (err == ESP_OK) {
        uint8_t auth = WIFI_AUTH_WPA2_PSK;
        err = nvs_get_u8(nvs, NVS_KEY_AUTH, &auth);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        *authmode = (wifi_auth_mode_t)auth;
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t save_credentials(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    if ((err = nvs_set_str(nvs, NVS_KEY_SSID, s_pending_ssid)) == ESP_OK &&
        (err = nvs_set_str(nvs, NVS_KEY_PASSWORD, s_pending_password)) == ESP_OK &&
        (err = nvs_set_u8(nvs, NVS_KEY_AUTH, (uint8_t)s_pending_auth)) == ESP_OK &&
        (err = nvs_set_str(nvs, NVS_KEY_STATUS_TOKEN, s_pending_status_token)) == ESP_OK &&
        (err = nvs_set_str(nvs, NVS_KEY_MONITOR_IP, s_pending_monitor_ip)) == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static void close_portal_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(5000));
    provisioning_portal_stop();
    s_provisioning = false;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_LOGI(TAG, "Provisioning AP closed; device remains connected as a station");
    vTaskDelete(NULL);
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        xSemaphoreGive(s_scan_done);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        /* esp_wifi_disconnect() is called before applying new credentials and
         * generates ASSOC_LEAVE. It is not a failed connection attempt. */
        if (s_pending_save && event->reason == WIFI_REASON_ASSOC_LEAVE) {
            ESP_LOGI(TAG, "Ignoring expected disconnect while changing Wi-Fi configuration");
            return;
        }
        char message[128];
        snprintf(message, sizeof(message), "Connection failed/disconnected (reason %d)", event->reason);
        bool submitted_connection_failed = s_provisioning && s_pending_save;
        set_status(submitted_connection_failed ? WIFI_MANAGER_FAILED :
                   (s_provisioning ? WIFI_MANAGER_PROVISIONING : WIFI_MANAGER_FAILED), message);
        if (submitted_connection_failed) {
            s_pending_save = false;
        }
        if (!s_provisioning && s_retry_count++ < 5) {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        char message[128];
        snprintf(message, sizeof(message), "Connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        set_status(WIFI_MANAGER_CONNECTED, message);
        s_retry_count = 0;
        if (s_pending_save) {
            esp_err_t err = save_credentials();
            if (err == ESP_OK) {
                s_pending_save = false;
                ESP_LOGI(TAG, "Verified Wi-Fi configuration saved to NVS");
                xTaskCreate(close_portal_task, "close_portal", 3072, NULL, 3, NULL);
            } else {
                ESP_LOGE(TAG, "Could not save Wi-Fi configuration: %s", esp_err_to_name(err));
            }
        }
    }
}

esp_err_t wifi_manager_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_scan_lock = xSemaphoreCreateMutex();
    s_scan_done = xSemaphoreCreateBinary();
    if (!s_lock || !s_scan_lock || !s_scan_done) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_sta_netif || !s_ap_netif) return ESP_ERR_NO_MEM;

    /* Advertise the configuration page using DHCP captive-portal option 114. */
    static const char captive_portal_uri[] = CAPTIVE_PORTAL_URL;
    ESP_RETURN_ON_ERROR(
        esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                               ESP_NETIF_CAPTIVEPORTAL_URI,
                               (void *)captive_portal_uri,
                               strlen(captive_portal_uri)),
        TAG, "set captive portal URI failed");

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&config), TAG, "esp_wifi_init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   event_handler, NULL), TAG, "Wi-Fi handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   event_handler, NULL), TAG, "IP handler failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Wi-Fi storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "STA mode failed");
    return esp_wifi_start();
}

bool wifi_manager_has_credentials(void)
{
    char ssid[33] = {0};
    char password[65] = {0};
    wifi_auth_mode_t auth;
    return load_credentials(ssid, sizeof(ssid), password, sizeof(password), &auth) == ESP_OK && ssid[0];
}

static esp_err_t apply_sta_config(const char *ssid, const char *password,
                                  wifi_auth_mode_t authmode)
{
    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, password, sizeof(config.sta.password));
    /* threshold.authmode is a minimum accepted security level, not an exact
     * copy of the scan result. A WPA/WPA2 transition AP may negotiate WPA-PSK,
     * so using WIFI_AUTH_WPA_WPA2_PSK (4) here incorrectly rejects it. */
    switch (authmode) {
        case WIFI_AUTH_WPA_WPA2_PSK:
            config.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
            break;
        case WIFI_AUTH_WPA2_WPA3_PSK:
            config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
            break;
        default:
            config.sta.threshold.authmode = authmode;
            break;
    }
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = authmode == WIFI_AUTH_WPA3_PSK;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG, "set STA config failed");
    esp_wifi_disconnect();
    return esp_wifi_connect();
}

esp_err_t wifi_manager_connect_saved(void)
{
    char ssid[33] = {0};
    char password[65] = {0};
    wifi_auth_mode_t auth = WIFI_AUTH_WPA2_PSK;
    ESP_RETURN_ON_ERROR(load_credentials(ssid, sizeof(ssid), password, sizeof(password), &auth),
                        TAG, "load credentials failed");
    set_status(WIFI_MANAGER_CONNECTING, "Connecting to saved Wi-Fi");
    return apply_sta_config(ssid, password, auth);
}

esp_err_t wifi_manager_start_provisioning(void)
{
    if (s_provisioning) return ESP_OK;
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), TAG, "read MAC failed");
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X%02X%02X%02X%02X",
             CONFIG_PROV_AP_SSID_PREFIX, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, CONFIG_PROV_AP_PASSWORD, sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = strlen(CONFIG_PROV_AP_PASSWORD) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ap.ap.pmf_cfg.capable = true;

    s_provisioning = true;
    s_pending_save = false;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "APSTA mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "AP config failed");
    set_status(WIFI_MANAGER_PROVISIONING, "Provisioning portal active");
    ESP_RETURN_ON_ERROR(provisioning_portal_start(), TAG, "portal start failed");
    ESP_LOGI(TAG, "Connect to AP '%s' with password '%s'; captive portal: %s",
             s_ap_ssid, CONFIG_PROV_AP_PASSWORD, CAPTIVE_PORTAL_URL);
    return ESP_OK;
}

esp_err_t wifi_manager_submit_credentials(const char *ssid, const char *password,
                                          wifi_auth_mode_t authmode,
                                          const char *status_token,
                                          const char *monitor_ip)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || !password || strlen(password) > 63 ||
        !status_token || !status_token[0] || strlen(status_token) > 128 ||
        !monitor_ip || strlen(monitor_ip) > 15) {
        return ESP_ERR_INVALID_ARG;
    }
    if (monitor_ip[0]) {
        ip4_addr_t parsed_ip;
        if (!ip4addr_aton(monitor_ip, &parsed_ip)) return ESP_ERR_INVALID_ARG;
    }
    size_t password_len = strlen(password);
    if (authmode == WIFI_AUTH_WEP) {
        if (password_len != 5 && password_len != 10 && password_len != 13 && password_len != 26) {
            return ESP_ERR_INVALID_ARG;
        }
    } else if (authmode != WIFI_AUTH_OPEN && (password_len < 8 || password_len > 63)) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_pending_ssid, ssid, sizeof(s_pending_ssid));
    strlcpy(s_pending_password, password, sizeof(s_pending_password));
    strlcpy(s_pending_status_token, status_token, sizeof(s_pending_status_token));
    strlcpy(s_pending_monitor_ip, monitor_ip, sizeof(s_pending_monitor_ip));
    s_pending_auth = authmode;
    s_pending_save = true;
    set_status(WIFI_MANAGER_CONNECTING, "Testing submitted Wi-Fi configuration");
    esp_err_t err = apply_sta_config(ssid, password, authmode);
    if (err != ESP_OK) s_pending_save = false;
    return err;
}

esp_err_t wifi_manager_scan(wifi_manager_ap_t **records, uint16_t *count)
{
    if (!records || !count) return ESP_ERR_INVALID_ARG;
    *records = NULL;
    *count = 0;

    if (xSemaphoreTake(s_scan_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    while (xSemaphoreTake(s_scan_done, 0) == pdTRUE) {}
    esp_wifi_scan_stop();

    wifi_scan_config_t config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {
            .min = 40,
            .max = 120,
        },
        .home_chan_dwell_time = 30,
    };
    esp_err_t err = esp_wifi_scan_start(&config, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not start Wi-Fi scan: %s", esp_err_to_name(err));
        xSemaphoreGive(s_scan_lock);
        return err;
    }
    if (xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(15000)) != pdTRUE) {
        ESP_LOGE(TAG, "Wi-Fi scan timed out");
        esp_wifi_scan_stop();
        xSemaphoreGive(s_scan_lock);
        return ESP_ERR_TIMEOUT;
    }

    uint16_t found = 0;
    err = esp_wifi_scan_get_ap_num(&found);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not read Wi-Fi scan count: %s", esp_err_to_name(err));
        xSemaphoreGive(s_scan_lock);
        return err;
    }
    if (found > MAX_SCAN_RESULTS) found = MAX_SCAN_RESULTS;
    if (!found) {
        xSemaphoreGive(s_scan_lock);
        return ESP_OK;
    }
    wifi_ap_record_t *raw = calloc(found, sizeof(*raw));
    wifi_manager_ap_t *result = calloc(found, sizeof(*result));
    if (!raw || !result) {
        free(raw); free(result);
        xSemaphoreGive(s_scan_lock);
        return ESP_ERR_NO_MEM;
    }
    err = esp_wifi_scan_get_ap_records(&found, raw);
    if (err == ESP_OK) {
        for (uint16_t i = 0; i < found; ++i) {
            strlcpy(result[i].ssid, (char *)raw[i].ssid, sizeof(result[i].ssid));
            result[i].rssi = raw[i].rssi;
            result[i].channel = raw[i].primary;
            result[i].authmode = raw[i].authmode;
        }
        *records = result;
        *count = found;
    } else {
        free(result);
    }
    free(raw);
    xSemaphoreGive(s_scan_lock);
    return err;
}

void wifi_manager_free_scan(wifi_manager_ap_t *records) { free(records); }

esp_err_t wifi_manager_clear_credentials(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_erase_all(nvs);
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    s_pending_save = false;
    s_pending_status_token[0] = 0;
    s_pending_monitor_ip[0] = 0;
    ESP_LOGW(TAG, "Saved Wi-Fi configuration cleared");
    if (err == ESP_OK) err = wifi_manager_start_provisioning();
    return err;
}

wifi_manager_state_t wifi_manager_get_state(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    wifi_manager_state_t state = s_state;
    xSemaphoreGive(s_lock);
    return state;
}

void wifi_manager_get_status(char *buffer, size_t length)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(buffer, s_status, length);
    xSemaphoreGive(s_lock);
}

const char *wifi_manager_get_ap_ssid(void) { return s_ap_ssid; }

bool wifi_manager_get_gateway(char *buffer, size_t length)
{
    if (!buffer || length == 0 || !s_sta_netif) return false;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(s_sta_netif, &info) != ESP_OK || info.gw.addr == 0) return false;
    snprintf(buffer, length, IPSTR, IP2STR(&info.gw));
    return true;
}

bool wifi_manager_get_status_token(char *buffer, size_t length)
{
    if (!buffer || length == 0) return false;
    if (s_pending_status_token[0]) {
        strlcpy(buffer, s_pending_status_token, length);
        return true;
    }
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t required = length;
    esp_err_t err = nvs_get_str(nvs, NVS_KEY_STATUS_TOKEN, buffer, &required);
    nvs_close(nvs);
    return err == ESP_OK && buffer[0];
}

bool wifi_manager_get_monitor_ip(char *buffer, size_t length)
{
    if (!buffer || length == 0) return false;
    if (s_pending_monitor_ip[0]) {
        strlcpy(buffer, s_pending_monitor_ip, length);
        return true;
    }
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t required = length;
    esp_err_t err = nvs_get_str(nvs, NVS_KEY_MONITOR_IP, buffer, &required);
    nvs_close(nvs);
    return err == ESP_OK && buffer[0];
}

const char *wifi_manager_auth_name(wifi_auth_mode_t mode)
{
    switch (mode) {
        case WIFI_AUTH_OPEN: return "OPEN";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
        default: return "UNKNOWN";
    }
}
