#include "test_gif.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "extra/libs/gif/gifdec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#define GIF_WIDTH 320U
#define GIF_HEIGHT 172U
#define GIF_PIXELS (GIF_WIDTH * GIF_HEIGHT)
#define GIF_FRAME_BYTES (GIF_PIXELS * sizeof(lv_color_t))
#define GIF_MAX_PREDECODED_FRAMES 60U
#define GIF_DEFAULT_DELAY_MS 70U
#define GIF_DECODE_TASK_STACK_SIZE 4096U
#define GIF_DECODE_TASK_PRIORITY 2U
#define GIF_DECODE_CORE 0

extern const uint8_t test_gif_start[] asm("_binary_screensaver_gif_start");
extern const uint8_t test_gif_end[] asm("_binary_screensaver_gif_end");

typedef enum {
    GIF_STATE_IDLE,
    GIF_STATE_DECODING,
    GIF_STATE_READY,
    GIF_STATE_FAILED,
} gif_state_t;

static const char *TAG = "test_gif";
static lv_color_t *s_frames[GIF_MAX_PREDECODED_FRAMES];
static lv_img_dsc_t s_descriptors[GIF_MAX_PREDECODED_FRAMES];
static uint16_t s_delays_ms[GIF_MAX_PREDECODED_FRAMES];
static uint16_t s_frame_count;
static uint16_t s_visible_frame;
static lv_obj_t *s_image;
static lv_timer_t *s_timer;
static atomic_int s_state;
static esp_err_t s_decode_error = ESP_OK;

static esp_err_t validate_gif(void)
{
    size_t size = (size_t)(test_gif_end - test_gif_start);
    if (size < 13 || memcmp(test_gif_start, "GIF89a", 6) != 0) {
        ESP_LOGE(TAG, "Embedded file is not a supported GIF89a image");
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint16_t width = (uint16_t)test_gif_start[6] |
                     ((uint16_t)test_gif_start[7] << 8);
    uint16_t height = (uint16_t)test_gif_start[8] |
                      ((uint16_t)test_gif_start[9] << 8);
    if (width != GIF_WIDTH || height != GIF_HEIGHT) {
        ESP_LOGE(TAG, "GIF size must be %ux%u, got %ux%u",
                 GIF_WIDTH, GIF_HEIGHT, (unsigned)width, (unsigned)height);
        return ESP_ERR_INVALID_SIZE;
    }
    if ((test_gif_start[10] & 0x80U) == 0) {
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
    /* In LV_COLOR_DEPTH=16 mode gifdec stores two RGB565 bytes and one alpha
     * byte per pixel. The display is opaque, so only RGB565 is retained. */
    for (size_t i = 0; i < GIF_PIXELS; ++i) {
        destination[i].full = (uint16_t)canvas[i * 3U] |
                              ((uint16_t)canvas[i * 3U + 1U] << 8);
    }
}

static void predecode_task(void *arg)
{
    (void)arg;
    int64_t started_us = esp_timer_get_time();
    esp_err_t error = validate_gif();
    gd_GIF *decoder = NULL;

    if (error == ESP_OK) {
        decoder = gd_open_gif_data(test_gif_start);
        if (!decoder) error = ESP_ERR_NO_MEM;
    }

    while (error == ESP_OK && s_frame_count < GIF_MAX_PREDECODED_FRAMES) {
        int result = gd_get_frame(decoder);
        if (result == 0) break;
        if (result < 0) {
            error = ESP_ERR_INVALID_RESPONSE;
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
        s_delays_ms[index] = decoder->gce.delay
                                 ? decoder->gce.delay * 10U
                                 : GIF_DEFAULT_DELAY_MS;
        s_descriptors[index].header.always_zero = 0;
        s_descriptors[index].header.w = GIF_WIDTH;
        s_descriptors[index].header.h = GIF_HEIGHT;
        s_descriptors[index].header.cf = LV_IMG_CF_TRUE_COLOR;
        s_descriptors[index].data_size = GIF_FRAME_BYTES;
        s_descriptors[index].data = (const uint8_t *)frame;
        s_frame_count++;

        /* A complex frame can take more than 100 ms to decode. */
        vTaskDelay(1);
    }

    if (decoder) gd_close_gif(decoder);
    if (error == ESP_OK && s_frame_count == 0) error = ESP_ERR_INVALID_RESPONSE;

    if (error != ESP_OK) {
        free_frames();
        s_decode_error = error;
        atomic_store_explicit(&s_state, GIF_STATE_FAILED, memory_order_release);
        ESP_LOGE(TAG, "GIF predecode failed: %s", esp_err_to_name(error));
    } else {
        atomic_store_explicit(&s_state, GIF_STATE_READY, memory_order_release);
        ESP_LOGI(TAG, "Predecoded %u frames on CPU%d: %u KiB PSRAM, free %u KiB, %lld ms",
                 (unsigned)s_frame_count, GIF_DECODE_CORE,
                 (unsigned)((s_frame_count * GIF_FRAME_BYTES) / 1024U),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U),
                 (long long)((esp_timer_get_time() - started_us) / 1000));
        if (s_frame_count == GIF_MAX_PREDECODED_FRAMES) {
            ESP_LOGW(TAG, "GIF was limited to the first %u frames",
                     GIF_MAX_PREDECODED_FRAMES);
        }
    }
    vTaskDelete(NULL);
}

esp_err_t test_gif_preload_start(void)
{
    int expected = GIF_STATE_IDLE;
    if (!atomic_compare_exchange_strong(&s_state, &expected, GIF_STATE_DECODING)) {
        return expected == GIF_STATE_FAILED ? s_decode_error : ESP_ERR_INVALID_STATE;
    }

    BaseType_t created = xTaskCreatePinnedToCore(
        predecode_task, "gif_predecode", GIF_DECODE_TASK_STACK_SIZE, NULL,
        GIF_DECODE_TASK_PRIORITY, NULL, GIF_DECODE_CORE);
    if (created != pdPASS) {
        s_decode_error = ESP_ERR_NO_MEM;
        atomic_store(&s_state, GIF_STATE_FAILED);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "GIF predecode started on CPU%d", GIF_DECODE_CORE);
    return ESP_OK;
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
    if (state == GIF_STATE_FAILED) return s_decode_error;
    if (state != GIF_STATE_READY) return ESP_ERR_INVALID_STATE;

    if (s_image) {
        lv_obj_move_foreground(s_image);
        lv_obj_clear_flag(s_image, LV_OBJ_FLAG_HIDDEN);
        if (s_timer) {
            lv_timer_resume(s_timer);
            lv_timer_reset(s_timer);
        }
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
    ESP_LOGI(TAG, "Predecoded GIF playback started: %u frames",
             (unsigned)s_frame_count);
    return ESP_OK;
}

void test_gif_hide(void)
{
    if (!s_image || lv_obj_has_flag(s_image, LV_OBJ_FLAG_HIDDEN)) return;
    if (s_timer) lv_timer_pause(s_timer);
    lv_obj_add_flag(s_image, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "GIF playback stopped by user");
}

bool test_gif_is_visible(void)
{
    return s_image && !lv_obj_has_flag(s_image, LV_OBJ_FLAG_HIDDEN);
}
