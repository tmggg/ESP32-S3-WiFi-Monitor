#include "openwrt_status.h"

#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_manager.h"

/* The OpenWrt CGI uses a one-second sampling window. Start the next request
 * immediately afterwards, leaving only a short scheduler yield. */
#define STATUS_INTERVAL_MS 10
#define RESPONSE_LIMIT 8192
#define STATUS_TASK_STACK_SIZE 16384
#define STATUS_TASK_CORE 0

static const char *TAG = "openwrt_status";
static SemaphoreHandle_t s_lock;
static openwrt_status_t s_status = { .message = "Waiting for Wi-Fi" };
static uint32_t s_sample_id;

typedef struct {
    char data[RESPONSE_LIMIT];
    size_t length;
} response_buffer_t;

static void set_message(const char *message)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.valid = false;
    strlcpy(s_status.message, message, sizeof(s_status.message));
    xSemaphoreGive(s_lock);
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    response_buffer_t *response = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA && response && event->data_len > 0) {
        size_t available = sizeof(response->data) - response->length - 1;
        size_t copy = event->data_len < available ? event->data_len : available;
        memcpy(response->data + response->length, event->data, copy);
        response->length += copy;
        response->data[response->length] = 0;
    }
    return ESP_OK;
}

static void url_encode(const char *input, char *output, size_t output_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; input[i] && j + 1 < output_size; ++i) {
        unsigned char c = input[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            output[j++] = c;
        } else if (j + 3 < output_size) {
            output[j++] = '%';
            output[j++] = hex[c >> 4];
            output[j++] = hex[c & 0x0f];
        }
    }
    output[j] = 0;
}

static double json_number(cJSON *root, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsNumber(item) ? item->valuedouble : 0;
}

static esp_err_t fetch_status(void)
{
    char gateway[16];
    char token[129];
    char encoded_token[385];
    if (!wifi_manager_get_monitor_ip(gateway, sizeof(gateway)) &&
        !wifi_manager_get_gateway(gateway, sizeof(gateway))) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!wifi_manager_get_status_token(token, sizeof(token)) || token[0] == 0) {
        set_message("Token not configured");
        return ESP_ERR_INVALID_STATE;
    }
    url_encode(token, encoded_token, sizeof(encoded_token));

    char url[480];
    snprintf(url, sizeof(url), "http://%s/cgi-bin/esp32-status?token=%s",
             gateway, encoded_token);
    response_buffer_t response = {0};
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event,
        .user_data = &response,
        .timeout_ms = 6000,
        .buffer_size = 512,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status_code != 200) {
        ESP_LOGW(TAG, "Status request failed: %s, HTTP %d", esp_err_to_name(err), status_code);
        set_message(status_code == 401 ? "Token rejected" : "Gateway unavailable");
        return err == ESP_OK ? ESP_FAIL : err;
    }

    cJSON *root = cJSON_Parse(response.data);
    cJSON *ok = root ? cJSON_GetObjectItemCaseSensitive(root, "ok") : NULL;
    if (!root || !cJSON_IsTrue(ok)) {
        cJSON_Delete(root);
        set_message("Invalid gateway response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    openwrt_status_t next = {
        .valid = true,
        .sample_id = ++s_sample_id,
        .cpu_percent = json_number(root, "cpu_percent"),
        .memory_percent = json_number(root, "memory_percent"),
        .temperature_c = json_number(root, "temperature_c"),
        .uptime_seconds = json_number(root, "uptime_seconds"),
        .wan_interface_index = -1,
        .message = "Online",
    };
    cJSON *wan_device = cJSON_GetObjectItemCaseSensitive(root, "wan_device");
    const char *wan_name = cJSON_IsString(wan_device) ? wan_device->valuestring : NULL;
    cJSON *interfaces = cJSON_GetObjectItemCaseSensitive(root, "interfaces");
    if (cJSON_IsArray(interfaces)) {
        cJSON *interface;
        cJSON_ArrayForEach(interface, interfaces) {
            uint64_t download_bps = (uint64_t)json_number(interface, "download_bps");
            uint64_t upload_bps = (uint64_t)json_number(interface, "upload_bps");
            next.download_bps += download_bps;
            next.upload_bps += upload_bps;
            next.download_bytes += (uint64_t)json_number(interface, "download_bytes");
            next.upload_bytes += (uint64_t)json_number(interface, "upload_bytes");
            cJSON *name = cJSON_GetObjectItemCaseSensitive(interface, "name");
            int stored_index = -1;
            if (cJSON_IsString(name) && next.interface_count < OPENWRT_MAX_INTERFACES) {
                stored_index = next.interface_count++;
                openwrt_interface_status_t *stored = &next.interfaces[stored_index];
                strlcpy(stored->name, name->valuestring, sizeof(stored->name));
                stored->download_bps = download_bps;
                stored->upload_bps = upload_bps;
                stored->download_bytes = (uint64_t)json_number(interface, "download_bytes");
                stored->upload_bytes = (uint64_t)json_number(interface, "upload_bytes");
            }
            if (wan_name && cJSON_IsString(name) && strcmp(name->valuestring, wan_name) == 0) {
                next.wan_download_bps = download_bps;
                next.wan_upload_bps = upload_bps;
                next.wan_interface_index = stored_index;
            }
        }
        snprintf(next.wan_device, sizeof(next.wan_device), "ALL/%d",
                 cJSON_GetArraySize(interfaces));
    } else {
        /* Accept the older single-interface response during script upgrades. */
        next.download_bps = json_number(root, "download_bps");
        next.upload_bps = json_number(root, "upload_bps");
        next.wan_download_bps = next.download_bps;
        next.wan_upload_bps = next.upload_bps;
        next.download_bytes = json_number(root, "download_bytes");
        next.upload_bytes = json_number(root, "upload_bytes");
        if (cJSON_IsString(wan_device)) {
            strlcpy(next.wan_device, wan_device->valuestring, sizeof(next.wan_device));
            next.interface_count = 1;
            next.wan_interface_index = 0;
            strlcpy(next.interfaces[0].name, wan_device->valuestring,
                    sizeof(next.interfaces[0].name));
            next.interfaces[0].download_bps = next.download_bps;
            next.interfaces[0].upload_bps = next.upload_bps;
            next.interfaces[0].download_bytes = next.download_bytes;
            next.interfaces[0].upload_bytes = next.upload_bytes;
        }
    }
    cJSON *hostname = cJSON_GetObjectItemCaseSensitive(root, "hostname");
    if (cJSON_IsString(hostname)) strlcpy(next.hostname, hostname->valuestring, sizeof(next.hostname));
    cJSON_Delete(root);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status = next;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static void status_task(void *arg)
{
    while (true) {
        if (wifi_manager_get_state() == WIFI_MANAGER_CONNECTED) {
            fetch_status();
            vTaskDelay(pdMS_TO_TICKS(STATUS_INTERVAL_MS));
        } else {
            set_message(wifi_manager_get_state() == WIFI_MANAGER_PROVISIONING ?
                        "Wi-Fi setup mode" : "Waiting for Wi-Fi");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

esp_err_t openwrt_status_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    return xTaskCreatePinnedToCore(status_task, "openwrt_status", STATUS_TASK_STACK_SIZE, NULL, 4,
                                   NULL, STATUS_TASK_CORE) == pdPASS ?
           ESP_OK : ESP_ERR_NO_MEM;
}

void openwrt_status_get(openwrt_status_t *status)
{
    if (!status || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *status = s_status;
    xSemaphoreGive(s_lock);
}
