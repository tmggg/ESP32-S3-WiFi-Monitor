#include "openwrt_status.h"

#include <stdio.h>
#include <math.h>
#include <string.h>
#include "cJSON.h"
#include "status_json.h"
#include "cpu_load_led.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_manager.h"

/* Poll serially at most once per second; v2 returns a cached sample. */
#define STATUS_INTERVAL_MS 10
#define RESPONSE_LIMIT 8192
#define STATUS_TASK_STACK_SIZE 16384
#define STATUS_TASK_CORE 0

static const char *TAG = "openwrt_status";
static SemaphoreHandle_t s_lock;
static openwrt_status_t s_status = { .message = "Waiting for Wi-Fi" };
static uint64_t s_sample_id;
static char s_sample_gateway[16];

typedef struct {
    char data[RESPONSE_LIMIT];
    size_t length;
    bool overflow;
} response_buffer_t;

static void set_message(const char *message)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.valid = false;
    strlcpy(s_status.message, message, sizeof(s_status.message));
    xSemaphoreGive(s_lock);
    cpu_load_led_set_unavailable();
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    response_buffer_t *response = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA && response && event->data_len > 0) {
        size_t available = sizeof(response->data) - response->length - 1;
        size_t copy = event->data_len < available ? event->data_len : available;
        if ((size_t)event->data_len > available) response->overflow = true;
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

static float json_measurement(cJSON *root, const char *name, double minimum, double maximum)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < minimum || item->valuedouble > maximum) return NAN;
    return (float)item->valuedouble;
}

static uint64_t json_traffic(cJSON *root, const char *name)
{
    uint64_t value;
    return status_json_u64(root, name, &value) &&
           value <= OPENWRT_TRAFFIC_MAX_JSON_INTEGER ? value : OPENWRT_TRAFFIC_UNAVAILABLE;
}

static bool valid_interface_name(const cJSON *name)
{
    if (!cJSON_IsString(name) || !name->valuestring || !name->valuestring[0] ||
        strlen(name->valuestring) >= OPENWRT_INTERFACE_NAME_LEN) return false;
    for (const unsigned char *p = (const unsigned char *)name->valuestring; *p; ++p) {
        if (*p <= 32 || *p >= 127) return false;
    }
    return true;
}

static void parse_interface(cJSON *object, openwrt_interface_status_t *interface,
                            const openwrt_status_t *status)
{
    interface->ifindex = 0;
    status_json_u64(object, "ifindex", &interface->ifindex);
    interface->interval_ms = status->interval_ms;
    if (status->protocol_v2 && cJSON_GetObjectItemCaseSensitive(object, "interval_ms") &&
        !status_json_u64(object, "interval_ms", &interface->interval_ms))
        interface->interval_ms = 0;
    interface->download_bps = json_traffic(object, "download_bps");
    interface->upload_bps = json_traffic(object, "upload_bps");
    if (status->protocol_v2 && (!interface->interval_ms ||
        interface->interval_ms > status->sample_end_ms)) {
        interface->download_bps = OPENWRT_TRAFFIC_UNAVAILABLE;
        interface->upload_bps = OPENWRT_TRAFFIC_UNAVAILABLE;
    }
    interface->download_bytes_valid = status_json_u64(object, "download_bytes",
                                                      &interface->download_bytes);
    interface->upload_bytes_valid = status_json_u64(object, "upload_bytes",
                                                    &interface->upload_bytes);
}

static uint64_t traffic_sum(uint64_t total, uint64_t value)
{
    if (total == OPENWRT_TRAFFIC_UNAVAILABLE || value == OPENWRT_TRAFFIC_UNAVAILABLE ||
        value >= UINT64_MAX - total) return OPENWRT_TRAFFIC_UNAVAILABLE;
    return total + value;
}

static bool json_identity(cJSON *root, const char *name, char *output, size_t size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0] ||
        strlen(item->valuestring) >= size) return false;
    strlcpy(output, item->valuestring, size);
    return true;
}

static bool parse_sample_metadata(cJSON *root, openwrt_status_t *next, const char *gateway)
{
    bool new_target = strcmp(gateway, s_sample_gateway) != 0;
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    next->history_generation = s_status.history_generation;
    if (!version) {
        /* Keep the older uncached endpoint usable during upgrades. */
        if (new_target || s_status.protocol_v2 || !next->history_generation)
            next->history_generation++;
        next->sample_id = ++s_sample_id;
        next->sample_local_us = next->received_us;
        return true;
    }
    uint64_t schema;
    if (!status_json_u64(root, "schema_version", &schema) || schema != 2 ||
        !json_identity(root, "boot_id", next->boot_id, sizeof(next->boot_id)) ||
        !json_identity(root, "collector_id", next->collector_id, sizeof(next->collector_id)) ||
        !status_json_u64(root, "sample_seq", &next->sample_seq) || !next->sample_seq ||
        !status_json_u64(root, "sample_end_ms", &next->sample_end_ms) ||
        !status_json_u64(root, "age_ms", &next->age_ms) ||
        !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(root, "stale")) ||
        !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "interfaces"))) return false;
    cJSON *interval = cJSON_GetObjectItemCaseSensitive(root, "interval_ms");
    if (!cJSON_IsNull(interval) &&
        !status_json_u64(root, "interval_ms", &next->interval_ms)) return false;
    if (next->interval_ms > next->sample_end_ms) return false;
    next->protocol_v2 = true;
    next->sample_id = next->sample_seq;
    bool reset = new_target || !s_status.protocol_v2 ||
                 strcmp(next->boot_id, s_status.boot_id) != 0 ||
                 strcmp(next->collector_id, s_status.collector_id) != 0;
    if (reset) next->history_generation++;
    else {
        if (next->sample_seq < s_status.sample_seq ||
            next->sample_end_ms < s_status.sample_end_ms ||
            (next->sample_seq == s_status.sample_seq &&
             next->sample_end_ms != s_status.sample_end_ms) ||
            (next->sample_seq > s_status.sample_seq &&
             next->sample_end_ms == s_status.sample_end_ms)) return false;
    }
    uint64_t transit_ms = (uint64_t)(next->received_us - next->request_started_us) / 1000;
    next->age_ms = transit_ms > UINT64_MAX - next->age_ms ?
                   UINT64_MAX : next->age_ms + transit_ms;
    if (!reset && next->sample_seq == s_status.sample_seq) {
        uint64_t elapsed = (uint64_t)(next->received_us - s_status.received_us) / 1000;
        uint64_t previous_age = elapsed > UINT64_MAX - s_status.age_ms ?
                                UINT64_MAX : s_status.age_ms + elapsed;
        if (next->age_ms < previous_age) next->age_ms = previous_age;
        next->sample_local_us = s_status.sample_local_us;
    } else if (next->age_ms <= OPENWRT_STALE_AFTER_MS) {
        int64_t observed_end_us = next->received_us - (int64_t)next->age_ms * 1000;
        next->sample_local_us = observed_end_us;
        if (!reset && s_status.sample_local_us > 0 &&
            next->sample_end_ms - s_status.sample_end_ms <= 120000) {
            /* Only compare deltas on the server clock, never subtract a server
             * uptime from ESP32 uptime. Keep one stable local history timeline. */
            int64_t mapped_us = s_status.sample_local_us +
                               (int64_t)(next->sample_end_ms - s_status.sample_end_ms) * 1000;
            if (mapped_us <= next->received_us) next->sample_local_us = mapped_us;
        }
    }
    next->stale = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "stale")) ||
                  next->age_ms > OPENWRT_STALE_AFTER_MS;
    if (next->stale) {
        next->valid = false;
        strlcpy(next->message, "Data expired", sizeof(next->message));
    }
    return true;
}

static esp_err_t fetch_status(void)
{
    char gateway[16];
    char token[129];
    char encoded_token[385];
    if (!wifi_manager_get_monitor_ip(gateway, sizeof(gateway)) &&
        !wifi_manager_get_gateway(gateway, sizeof(gateway))) {
        set_message("No gateway IP");
        return ESP_ERR_INVALID_STATE;
    }
    if (!wifi_manager_get_status_token(token, sizeof(token)) || token[0] == 0) {
        set_message("Token not configured");
        return ESP_ERR_INVALID_STATE;
    }

    char url[480];
    /* Stock uhttpd does not forward arbitrary headers to shell CGI scripts.
     * Send the configured URL token as well as the header for compatibility. */
    url_encode(token, encoded_token, sizeof(encoded_token));
    snprintf(url, sizeof(url), "http://%s/cgi-bin/esp32-status?token=%s",
             gateway, encoded_token);
    response_buffer_t response = {0};
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event,
        .user_data = &response,
        .timeout_ms = 5000,
        .buffer_size = 512,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client allocation failed for monitor %s", gateway);
        set_message("HTTP out of memory");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t header_err = esp_http_client_set_header(client, "X-API-Token", token);
    if (header_err != ESP_OK) {
        esp_http_client_cleanup(client);
        set_message("HTTP header failed");
        return header_err;
    }
    int64_t request_started_us = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(client);
    int64_t received_us = esp_timer_get_time();
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status_code != 200) {
        ESP_LOGW(TAG, "Monitor %s request failed: %s, HTTP %d",
                 gateway, esp_err_to_name(err), status_code);
        /* A CGI 401 without WWW-Authenticate also causes ESP_ERR_NOT_SUPPORTED
         * in esp_http_client; the HTTP status is the more useful diagnosis. */
        if (status_code == 401 || status_code == 403) {
            set_message("Token rejected");
        } else if (status_code == 503) {
            set_message("Status unavailable");
        } else if (status_code == 404) {
            set_message("Status API not found");
        } else if (err != ESP_OK) {
            set_message(err == ESP_ERR_TIMEOUT ? "Monitor timeout" : "Monitor connect failed");
        } else {
            char message[32];
            snprintf(message, sizeof(message), "Monitor HTTP %d", status_code);
            set_message(message);
        }
        return err == ESP_OK ? ESP_FAIL : err;
    }

    if (response.overflow) {
        set_message("Status response too large");
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *root = status_json_parse(response.data);
    cJSON *ok = root ? cJSON_GetObjectItemCaseSensitive(root, "ok") : NULL;
    if (!root || !cJSON_IsTrue(ok)) {
        cJSON_Delete(root);
        set_message("Invalid gateway response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    openwrt_status_t next = {
        .valid = true,
        .request_started_us = request_started_us,
        .received_us = received_us,
        .cpu_percent = json_measurement(root, "cpu_percent", 0, 100),
        .memory_percent = json_measurement(root, "memory_percent", 0, 100),
        .temperature_c = json_measurement(root, "temperature_c", -273.15, 1000),
        .uptime_seconds = 0,
        .wan_interface_index = -1,
        .message = "Online",
    };
    if (!parse_sample_metadata(root, &next, gateway)) {
        cJSON_Delete(root);
        set_message("Invalid sample metadata");
        return ESP_ERR_INVALID_RESPONSE;
    }
    status_json_u64(root, "uptime_seconds", &next.uptime_seconds);
    cJSON *wan_device = cJSON_GetObjectItemCaseSensitive(root, "wan_device");
    const char *wan_name = cJSON_IsString(wan_device) ? wan_device->valuestring : NULL;
    cJSON *interfaces = cJSON_GetObjectItemCaseSensitive(root, "interfaces");
    next.wan_download_bps = OPENWRT_TRAFFIC_UNAVAILABLE;
    next.wan_upload_bps = OPENWRT_TRAFFIC_UNAVAILABLE;
    if (interfaces && !cJSON_IsArray(interfaces)) {
        cJSON_Delete(root);
        set_message("Invalid interface list");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (cJSON_IsArray(interfaces)) {
        cJSON *object;
        cJSON_ArrayForEach(object, interfaces) {
            cJSON *name = cJSON_GetObjectItemCaseSensitive(object, "name");
            if (!valid_interface_name(name)) continue;
            bool duplicate = false;
            for (uint8_t i = 0; i < next.interface_count; ++i) {
                if (strcmp(next.interfaces[i].name, name->valuestring) == 0) duplicate = true;
            }
            if (duplicate) continue;
            int index = next.interface_count;
            if (index >= OPENWRT_MAX_INTERFACES) {
                /* Keep WAN addressable even if it follows the storage limit. */
                if (!wan_name || strcmp(name->valuestring, wan_name) != 0) continue;
                index = OPENWRT_MAX_INTERFACES - 1;
            } else {
                next.interface_count++;
            }
            openwrt_interface_status_t *stored = &next.interfaces[index];
            strlcpy(stored->name, name->valuestring, sizeof(stored->name));
            parse_interface(object, stored, &next);
        }
        if (valid_interface_name(wan_device))
            strlcpy(next.wan_device, wan_name, sizeof(next.wan_device));
    } else if (valid_interface_name(wan_device)) {
        /* Accept the older single-interface response during script upgrades. */
        next.interface_count = 1;
        strlcpy(next.interfaces[0].name, wan_device->valuestring,
                sizeof(next.interfaces[0].name));
        parse_interface(root, &next.interfaces[0], &next);
        strlcpy(next.wan_device, wan_device->valuestring, sizeof(next.wan_device));
    }
    for (uint8_t i = 0; i < next.interface_count; ++i) {
        const openwrt_interface_status_t *stored = &next.interfaces[i];
        next.download_bps = traffic_sum(next.download_bps, stored->download_bps);
        next.upload_bps = traffic_sum(next.upload_bps, stored->upload_bps);
        next.download_bytes = traffic_sum(next.download_bytes, stored->download_bytes_valid ?
                                         stored->download_bytes : OPENWRT_TRAFFIC_UNAVAILABLE);
        next.upload_bytes = traffic_sum(next.upload_bytes, stored->upload_bytes_valid ?
                                       stored->upload_bytes : OPENWRT_TRAFFIC_UNAVAILABLE);
        if (wan_name && strcmp(stored->name, wan_name) == 0) {
            next.wan_interface_index = i;
            next.wan_download_bps = stored->download_bps;
            next.wan_upload_bps = stored->upload_bps;
        }
    }
    cJSON *hostname = cJSON_GetObjectItemCaseSensitive(root, "hostname");
    if (cJSON_IsString(hostname)) strlcpy(next.hostname, hostname->valuestring, sizeof(next.hostname));
    cJSON *system_date = cJSON_GetObjectItemCaseSensitive(root, "system_date");
    if (cJSON_IsString(system_date)) {
        strlcpy(next.system_date, system_date->valuestring, sizeof(next.system_date));
    }
    cJSON *system_time = cJSON_GetObjectItemCaseSensitive(root, "system_time");
    if (cJSON_IsString(system_time)) {
        strlcpy(next.system_time, system_time->valuestring, sizeof(next.system_time));
    }
    cJSON *screensaver_timeout = cJSON_GetObjectItemCaseSensitive(root, "screensaver_timeout");
    if (cJSON_IsNumber(screensaver_timeout) && screensaver_timeout->valuedouble > 0) {
        double timeout = screensaver_timeout->valuedouble;
        next.screensaver_timeout = timeout > UINT32_MAX ? UINT32_MAX : (uint32_t)timeout;
    }
    cJSON *screensaver_type = cJSON_GetObjectItemCaseSensitive(root, "screensaver_type");
    if (cJSON_IsString(screensaver_type)) {
        strlcpy(next.screensaver_type, screensaver_type->valuestring,
                sizeof(next.screensaver_type));
    }
    cJSON *gif_url = cJSON_GetObjectItemCaseSensitive(root, "gif_url");
    if (cJSON_IsString(gif_url)) {
        strlcpy(next.gif_url, gif_url->valuestring, sizeof(next.gif_url));
    }
    cJSON *gif_version = cJSON_GetObjectItemCaseSensitive(root, "gif_version");
    if (cJSON_IsString(gif_version)) {
        strlcpy(next.gif_version, gif_version->valuestring, sizeof(next.gif_version));
    }
    cJSON *gif_size = cJSON_GetObjectItemCaseSensitive(root, "gif_size");
    if (cJSON_IsNumber(gif_size) && gif_size->valuedouble > 0) {
        double size = gif_size->valuedouble;
        next.gif_size = size > UINT32_MAX ? UINT32_MAX : (uint32_t)size;
    }
    cJSON_Delete(root);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status = next;
    strlcpy(s_sample_gateway, gateway, sizeof(s_sample_gateway));
    xSemaphoreGive(s_lock);
    if (next.valid && isfinite(next.cpu_percent)) cpu_load_led_set_openwrt_load(next.cpu_percent);
    else cpu_load_led_set_unavailable();
    return ESP_OK;
}

static void status_task(void *arg)
{
    while (true) {
        if (wifi_manager_get_state() == WIFI_MANAGER_CONNECTED) {
            int64_t started_us = esp_timer_get_time();
            fetch_status();
            int64_t remaining_ms = 1000 - (esp_timer_get_time() - started_us) / 1000;
            vTaskDelay(pdMS_TO_TICKS(remaining_ms > STATUS_INTERVAL_MS ?
                                    remaining_ms : STATUS_INTERVAL_MS));
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
    if (status->valid && status->protocol_v2) {
        uint64_t elapsed_ms = (uint64_t)(esp_timer_get_time() - status->received_us) / 1000;
        if (status->age_ms > OPENWRT_STALE_AFTER_MS ||
            elapsed_ms > OPENWRT_STALE_AFTER_MS - status->age_ms) {
            status->valid = false;
            status->stale = true;
            strlcpy(status->message, "Data expired", sizeof(status->message));
        }
    }
}
