#include "test_gif.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "extra/libs/gif/gifdec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "mbedtls/sha256.h"
#include "wifi_manager.h"

#define GIF_WIDTH 320U
#define GIF_HEIGHT 172U
#define GIF_PIXELS (GIF_WIDTH * GIF_HEIGHT)
#define GIF_FRAME_BYTES (GIF_PIXELS * sizeof(lv_color_t))
#define GIF_MAX_PREDECODED_FRAMES 60U
#define GIF_MIN_DELAY_MS 70U
#define GIF_TASK_STACK_SIZE 8192U
#define GIF_TASK_PRIORITY 2U
#define GIF_TASK_CORE 0
#define GIF_PSRAM_RESERVE_BYTES (512U * 1024U)
#define GIF_HTTP_TIMEOUT_MS 15000
#define GIF_FLASH_SECTOR_SIZE 4096U
#define GIF_FAILED_RETRY_MS 60000U

typedef enum {
    GIF_STATE_IDLE,
    GIF_STATE_DOWNLOADING,
    GIF_STATE_DECODING,
    GIF_STATE_READY,
    GIF_STATE_FAILED,
} gif_state_t;

typedef struct {
    char url[256];
    char version[65];
    uint32_t size;
} gif_config_t;

typedef struct {
    const esp_partition_t *partition;
    size_t expected_size;
    size_t written;
    esp_err_t error;
    mbedtls_sha256_context sha;
} download_context_t;

static const char *TAG = "test_gif";
static lv_color_t *s_frames[GIF_MAX_PREDECODED_FRAMES];
static lv_img_dsc_t s_descriptors[GIF_MAX_PREDECODED_FRAMES];
static uint16_t s_delays_ms[GIF_MAX_PREDECODED_FRAMES];
static uint16_t s_frame_count;
static uint16_t s_visible_frame;
static lv_obj_t *s_image;
static lv_timer_t *s_timer;
static atomic_int s_state;
static atomic_bool s_update_in_progress;
static atomic_bool s_replacement_pending;
static atomic_bool s_playing;
static esp_err_t s_error = ESP_OK;
static gif_config_t s_config;
static bool s_metadata_error_logged;
static char s_loaded_version[65];
static char s_failed_version[65];
static atomic_uint_fast32_t s_retry_after_tick;

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool hash_matches(const uint8_t digest[32], const char *expected)
{
    if (!expected || strlen(expected) != 64) return false;
    for (size_t i = 0; i < 32; ++i) {
        int high = hex_value(expected[i * 2]);
        int low = hex_value(expected[i * 2 + 1]);
        if (high < 0 || low < 0 || digest[i] != (uint8_t)((high << 4) | low)) {
            return false;
        }
    }
    return true;
}

static void url_encode(const char *input, char *output, size_t output_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; input[i] && j + 1 < output_size; ++i) {
        unsigned char value = (unsigned char)input[i];
        if (isalnum(value) || value == '-' || value == '_' || value == '.' || value == '~') {
            output[j++] = (char)value;
        } else if (j + 3 < output_size) {
            output[j++] = '%';
            output[j++] = hex[value >> 4];
            output[j++] = hex[value & 0x0f];
        }
    }
    output[j] = 0;
}

static esp_err_t make_download_url(char *output, size_t output_size,
                                   char *token, size_t token_size)
{
    char gateway[16];
    if (!wifi_manager_get_monitor_ip(gateway, sizeof(gateway)) &&
        !wifi_manager_get_gateway(gateway, sizeof(gateway))) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!wifi_manager_get_status_token(token, token_size) || token[0] == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    char base[320];
    int base_length;
    if (strncmp(s_config.url, "http://", 7) == 0) {
        base_length = snprintf(base, sizeof(base), "%s", s_config.url);
    } else if (s_config.url[0] == '/') {
        base_length = snprintf(base, sizeof(base), "http://%s%s", gateway, s_config.url);
    } else {
        base_length = snprintf(base, sizeof(base), "http://%s/%s", gateway, s_config.url);
    }
    if (base_length < 0 || (size_t)base_length >= sizeof(base)) return ESP_ERR_INVALID_SIZE;

    char encoded_token[385];
    url_encode(token, encoded_token, sizeof(encoded_token));
    int length = snprintf(output, output_size, "%s%ctoken=%s", base,
                          strchr(base, '?') ? '&' : '?', encoded_token);
    return length >= 0 && (size_t)length < output_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t download_event(esp_http_client_event_t *event)
{
    download_context_t *context = event->user_data;
    if (!context || event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }
    if (context->error != ESP_OK) return context->error;
    if (context->written + (size_t)event->data_len > context->expected_size) {
        context->error = ESP_ERR_INVALID_SIZE;
        return context->error;
    }
    context->error = esp_partition_write(context->partition, context->written,
                                         event->data, event->data_len);
    if (context->error != ESP_OK) return context->error;
    if (mbedtls_sha256_update(&context->sha,
                              (const unsigned char *)event->data,
                              event->data_len) != 0) {
        context->error = ESP_FAIL;
        return context->error;
    }
    context->written += event->data_len;
    return ESP_OK;
}

static esp_err_t validate_gif(const uint8_t *data, size_t size)
{
    if (!data || size < 13 || memcmp(data, "GIF89a", 6) != 0) {
        ESP_LOGE(TAG, "Downloaded file is not a supported GIF89a image");
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint16_t width = (uint16_t)data[6] | ((uint16_t)data[7] << 8);
    uint16_t height = (uint16_t)data[8] | ((uint16_t)data[9] << 8);
    if (width != GIF_WIDTH || height != GIF_HEIGHT) {
        ESP_LOGE(TAG, "GIF size must be %ux%u, got %ux%u",
                 GIF_WIDTH, GIF_HEIGHT, (unsigned)width, (unsigned)height);
        return ESP_ERR_INVALID_SIZE;
    }
    if ((data[10] & 0x80U) == 0) {
        ESP_LOGE(TAG, "GIF has no global color table");
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

static void free_frames(void)
{
    for (uint16_t i = 0; i < s_frame_count; ++i) {
        heap_caps_free(s_frames[i]);
        s_frames[i] = NULL;
    }
    s_frame_count = 0;
}

static void copy_rgb565(lv_color_t *destination, const uint8_t *canvas)
{
    for (size_t i = 0; i < GIF_PIXELS; ++i) {
        destination[i].full = (uint16_t)canvas[i * 3U] |
                              ((uint16_t)canvas[i * 3U + 1U] << 8);
    }
}

static esp_err_t download_to_partition(const esp_partition_t *partition)
{
    size_t erase_size = (s_config.size + GIF_FLASH_SECTOR_SIZE - 1U) &
                        ~(GIF_FLASH_SECTOR_SIZE - 1U);
    if (s_config.size == 0 || erase_size > partition->size) return ESP_ERR_INVALID_SIZE;
    ESP_RETURN_ON_ERROR(esp_partition_erase_range(partition, 0, erase_size),
                        TAG, "Failed to erase GIF storage");

    char url[768];
    char token[129];
    ESP_RETURN_ON_ERROR(make_download_url(url, sizeof(url), token, sizeof(token)),
                        TAG, "Unable to build GIF URL");

    download_context_t context = {
        .partition = partition,
        .expected_size = s_config.size,
        .error = ESP_OK,
    };
    mbedtls_sha256_init(&context.sha);
    if (mbedtls_sha256_starts(&context.sha, false) != 0) {
        mbedtls_sha256_free(&context.sha);
        return ESP_FAIL;
    }

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = download_event,
        .user_data = &context,
        .timeout_ms = GIF_HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        mbedtls_sha256_free(&context.sha);
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "X-API-Token", token);
    ESP_LOGI(TAG, "Downloading %lu-byte GIF to storage partition",
             (unsigned long)s_config.size);
    esp_err_t error = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    int64_t content_length = esp_http_client_get_content_length(client);
    esp_http_client_cleanup(client);

    uint8_t digest[32];
    int hash_error = mbedtls_sha256_finish(&context.sha, digest);
    mbedtls_sha256_free(&context.sha);
    if (error != ESP_OK) return error;
    if (context.error != ESP_OK) return context.error;
    if (status_code != 200) {
        ESP_LOGE(TAG, "GIF request returned HTTP %d", status_code);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (content_length >= 0 && (uint64_t)content_length != s_config.size) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (context.written != s_config.size) return ESP_ERR_INVALID_SIZE;
    if (hash_error != 0 || !hash_matches(digest, s_config.version)) {
        ESP_LOGE(TAG, "Downloaded GIF SHA-256 does not match gif_version");
        return ESP_ERR_INVALID_CRC;
    }
    ESP_LOGI(TAG, "GIF download and SHA-256 verification succeeded");
    return ESP_OK;
}

static esp_err_t predecode_partition(const esp_partition_t *partition)
{
    const void *mapped = NULL;
    esp_partition_mmap_handle_t map_handle = 0;
    esp_err_t error = esp_partition_mmap(partition, 0, s_config.size,
                                         ESP_PARTITION_MMAP_DATA,
                                         &mapped, &map_handle);
    if (error != ESP_OK) return error;

    error = validate_gif(mapped, s_config.size);
    gd_GIF *decoder = NULL;
    if (error == ESP_OK) {
        decoder = gd_open_gif_data(mapped);
        if (!decoder) error = ESP_ERR_NO_MEM;
    }
    while (error == ESP_OK && s_frame_count < GIF_MAX_PREDECODED_FRAMES) {
        uint32_t stream_position = decoder->f_rw_p;
        int result = gd_get_frame(decoder);
        if (result == 0) break;
        if (result < 0) {
            error = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        /* gifdec automatically seeks back to anim_start for an infinitely
         * looping GIF. Stop at that first wrap so a short animation is not
         * copied repeatedly until the 60-frame PSRAM limit is reached. */
        if (s_frame_count > 0 && decoder->f_rw_p <= stream_position) {
            ESP_LOGI(TAG, "GIF loop boundary reached after %u unique frames",
                     (unsigned)s_frame_count);
            break;
        }
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        if (free_psram < GIF_FRAME_BYTES + GIF_PSRAM_RESERVE_BYTES) {
            ESP_LOGW(TAG, "PSRAM reserve reached after %u GIF frames",
                     (unsigned)s_frame_count);
            break;
        }

        gd_render_frame(decoder, decoder->canvas);
        lv_color_t *frame = heap_caps_malloc(GIF_FRAME_BYTES,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!frame) {
            error = ESP_ERR_NO_MEM;
            break;
        }
        copy_rgb565(frame, decoder->canvas);

        uint16_t index = s_frame_count;
        s_frames[index] = frame;
        uint16_t delay = decoder->gce.delay ? decoder->gce.delay * 10U : GIF_MIN_DELAY_MS;
        s_delays_ms[index] = delay < GIF_MIN_DELAY_MS ? GIF_MIN_DELAY_MS : delay;
        s_descriptors[index].header.always_zero = 0;
        s_descriptors[index].header.w = GIF_WIDTH;
        s_descriptors[index].header.h = GIF_HEIGHT;
        s_descriptors[index].header.cf = LV_IMG_CF_TRUE_COLOR;
        s_descriptors[index].data_size = GIF_FRAME_BYTES;
        s_descriptors[index].data = (const uint8_t *)frame;
        s_frame_count++;
        vTaskDelay(1);
    }

    if (decoder) gd_close_gif(decoder);
    esp_partition_munmap(map_handle);
    if (error == ESP_OK && s_frame_count == 0) error = ESP_ERR_INVALID_RESPONSE;
    return error;
}

static void download_and_decode_task(void *arg)
{
    bool replacing_ready_gif = (bool)(uintptr_t)arg;
    int64_t started_us = esp_timer_get_time();
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "storage");
    esp_err_t error = partition ? ESP_OK : ESP_ERR_NOT_FOUND;
    if (error == ESP_OK && s_config.size > partition->size) error = ESP_ERR_INVALID_SIZE;
    if (error == ESP_OK) {
        for (unsigned int attempt = 1; attempt <= 3; ++attempt) {
            error = download_to_partition(partition);
            if (error == ESP_OK) break;
            ESP_LOGW(TAG, "GIF download attempt %u/3 failed: %s",
                     attempt, esp_err_to_name(error));
            if (attempt < 3) vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    if (error == ESP_OK && replacing_ready_gif) {
        /* Keep playing the old PSRAM frames while the replacement downloads.
         * Once verified, make is_ready() false so the display task hides the
         * GIF and switches to the clock before these frames are released. */
        atomic_store_explicit(&s_replacement_pending, true, memory_order_release);
        while (atomic_load_explicit(&s_playing, memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        free_frames();
    }
    if (error == ESP_OK) {
        atomic_store_explicit(&s_state, GIF_STATE_DECODING, memory_order_release);
        error = predecode_partition(partition);
    }

    if (error != ESP_OK) {
        s_error = error;
        strlcpy(s_failed_version, s_config.version, sizeof(s_failed_version));
        atomic_store(&s_retry_after_tick,
                     xTaskGetTickCount() + pdMS_TO_TICKS(GIF_FAILED_RETRY_MS));
        if (replacing_ready_gif && !atomic_load(&s_replacement_pending)) {
            /* The replacement failed before touching the old decoded frames. */
            atomic_store_explicit(&s_state, GIF_STATE_READY, memory_order_release);
        } else {
            free_frames();
            atomic_store_explicit(&s_state, GIF_STATE_FAILED, memory_order_release);
        }
        ESP_LOGE(TAG, "Dynamic GIF preparation failed: %s", esp_err_to_name(error));
    } else {
        strlcpy(s_loaded_version, s_config.version, sizeof(s_loaded_version));
        s_failed_version[0] = 0;
        atomic_store_explicit(&s_state, GIF_STATE_READY, memory_order_release);
        ESP_LOGI(TAG, "Prepared %u GIF frames in %u KiB PSRAM; free %u KiB; %lld ms",
                 (unsigned)s_frame_count,
                 (unsigned)((s_frame_count * GIF_FRAME_BYTES) / 1024U),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U),
                 (long long)((esp_timer_get_time() - started_us) / 1000));
    }
    atomic_store_explicit(&s_replacement_pending, false, memory_order_release);
    atomic_store_explicit(&s_update_in_progress, false, memory_order_release);
    vTaskDelete(NULL);
}

esp_err_t test_gif_configure(const openwrt_status_t *status)
{
    if (!status || status->screensaver_timeout == 0 ||
        strcmp(status->screensaver_type, "gif") != 0) {
        return ESP_OK;
    }
    if (!status->gif_url[0] || strlen(status->gif_version) != 64 || status->gif_size == 0) {
        if (!s_metadata_error_logged) {
            ESP_LOGE(TAG, "GIF screensaver metadata is incomplete");
            s_metadata_error_logged = true;
        }
        return ESP_ERR_INVALID_ARG;
    }

    if (atomic_load_explicit(&s_update_in_progress, memory_order_acquire)) return ESP_OK;
    gif_state_t state = atomic_load_explicit(&s_state, memory_order_acquire);
    if (state == GIF_STATE_READY && strcmp(status->gif_version, s_loaded_version) == 0) {
        return ESP_OK;
    }
    TickType_t now_tick = xTaskGetTickCount();
    TickType_t retry_tick = (TickType_t)atomic_load(&s_retry_after_tick);
    if (strcmp(status->gif_version, s_failed_version) == 0 &&
        (int32_t)(retry_tick - now_tick) > 0) {
        return s_error;
    }

    bool replacing_ready_gif = state == GIF_STATE_READY;
    bool expected_update = false;
    if (!atomic_compare_exchange_strong(&s_update_in_progress,
                                        &expected_update, true)) {
        return ESP_OK;
    }
    strlcpy(s_config.url, status->gif_url, sizeof(s_config.url));
    strlcpy(s_config.version, status->gif_version, sizeof(s_config.version));
    s_config.size = status->gif_size;
    if (!replacing_ready_gif) {
        atomic_store_explicit(&s_state, GIF_STATE_DOWNLOADING, memory_order_release);
    }

    BaseType_t created = xTaskCreatePinnedToCore(
        download_and_decode_task, "gif_prepare", GIF_TASK_STACK_SIZE,
        (void *)(uintptr_t)replacing_ready_gif,
        GIF_TASK_PRIORITY, NULL, GIF_TASK_CORE);
    if (created != pdPASS) {
        s_error = ESP_ERR_NO_MEM;
        atomic_store(&s_update_in_progress, false);
        if (!replacing_ready_gif) atomic_store(&s_state, GIF_STATE_FAILED);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "%s GIF version %.12s scheduled on CPU%d",
             replacing_ready_gif ? "Replacement" : "Initial",
             s_config.version, GIF_TASK_CORE);
    return ESP_OK;
}

bool test_gif_is_ready(void)
{
    return atomic_load_explicit(&s_state, memory_order_acquire) == GIF_STATE_READY &&
           !atomic_load_explicit(&s_replacement_pending, memory_order_acquire);
}

static void next_frame(lv_timer_t *timer)
{
    if (!s_image || s_frame_count == 0) return;
    s_visible_frame = (s_visible_frame + 1U) % s_frame_count;
    lv_img_set_src(s_image, &s_descriptors[s_visible_frame]);
    lv_timer_set_period(timer, s_delays_ms[s_visible_frame]);
}

esp_err_t test_gif_show(void)
{
    gif_state_t state = atomic_load_explicit(&s_state, memory_order_acquire);
    if (state == GIF_STATE_FAILED) return s_error;
    if (state != GIF_STATE_READY || atomic_load(&s_replacement_pending)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_image) {
        s_visible_frame = 0;
        lv_img_set_src(s_image, &s_descriptors[0]);
        lv_obj_move_foreground(s_image);
        lv_obj_clear_flag(s_image, LV_OBJ_FLAG_HIDDEN);
        if (s_timer) {
            lv_timer_set_period(s_timer, s_delays_ms[0]);
            lv_timer_resume(s_timer);
            lv_timer_reset(s_timer);
        }
        atomic_store_explicit(&s_playing, true, memory_order_release);
        return ESP_OK;
    }

    s_visible_frame = 0;
    s_image = lv_img_create(lv_scr_act());
    if (!s_image) return ESP_ERR_NO_MEM;
    lv_img_set_src(s_image, &s_descriptors[0]);
    lv_obj_set_pos(s_image, 0, 0);
    lv_obj_clear_flag(s_image, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_image);
    s_timer = lv_timer_create(next_frame, s_delays_ms[0], NULL);
    if (!s_timer) {
        lv_obj_del(s_image);
        s_image = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Dynamic GIF playback started: %u frames", (unsigned)s_frame_count);
    atomic_store_explicit(&s_playing, true, memory_order_release);
    return ESP_OK;
}

void test_gif_hide(void)
{
    if (!s_image || lv_obj_has_flag(s_image, LV_OBJ_FLAG_HIDDEN)) {
        atomic_store_explicit(&s_playing, false, memory_order_release);
        return;
    }
    if (s_timer) lv_timer_pause(s_timer);
    lv_obj_add_flag(s_image, LV_OBJ_FLAG_HIDDEN);
    atomic_store_explicit(&s_playing, false, memory_order_release);
    ESP_LOGI(TAG, "GIF playback stopped");
}

bool test_gif_is_visible(void)
{
    return s_image && !lv_obj_has_flag(s_image, LV_OBJ_FLAG_HIDDEN);
}
