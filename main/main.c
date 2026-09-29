#include "boot_button.h"
#include <string.h>
#include "board_display.h"
#include "clock_screensaver.h"
#include "cpu_load_led.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "openwrt_status.h"
#include "status_dashboard.h"
#include "test_gif.h"
#include "wifi_manager.h"

static const char *TAG = "main";

#define DISPLAY_DIM_DELAY_MS 60000
#define DISPLAY_TASK_STACK_SIZE 6144
#define DISPLAY_TASK_PRIORITY 5
#define NETWORK_CORE 0
#define DISPLAY_CORE 1

typedef enum {
    SCREENSAVER_NONE,
    SCREENSAVER_GIF,
    SCREENSAVER_CLOCK,
} screensaver_kind_t;

static screensaver_kind_t configured_screensaver(const openwrt_status_t *status)
{
    if (!status || status->screensaver_timeout == 0) return SCREENSAVER_NONE;
    if (strcmp(status->screensaver_type, "gif") == 0) return SCREENSAVER_GIF;
    if (strcmp(status->screensaver_type, "clock") == 0) return SCREENSAVER_CLOCK;
    return SCREENSAVER_NONE;
}

static esp_err_t set_screensaver(screensaver_kind_t desired,
                                 screensaver_kind_t *active)
{
    if (!active || desired == *active) return ESP_OK;

    if (*active != SCREENSAVER_NONE) {
        board_display_lock();
        if (*active == SCREENSAVER_GIF) test_gif_hide();
        else if (*active == SCREENSAVER_CLOCK) clock_screensaver_hide();
        board_display_unlock();
        status_dashboard_set_render_paused(false);
        *active = SCREENSAVER_NONE;
        status_dashboard_update();
    }
    if (desired == SCREENSAVER_NONE) return ESP_OK;

    esp_err_t error = ESP_OK;
    board_display_lock();
    if (desired == SCREENSAVER_GIF) error = test_gif_show();
    else clock_screensaver_show();
    board_display_unlock();
    if (error != ESP_OK) return error;

    *active = desired;
    status_dashboard_set_render_paused(true);
    ESP_LOGI(TAG, "%s screensaver started",
             desired == SCREENSAVER_GIF ? "GIF" : "Clock");
    return ESP_OK;
}

static void display_task(void *arg)
{
    (void)arg;
    TickType_t display_started = xTaskGetTickCount();
    TickType_t last_update = 0;
    TickType_t last_frame_wake = display_started;
    uint32_t frame_tick_remainder = 0;
    int64_t perf_window_started = esp_timer_get_time();
    uint64_t handler_total_us = 0;
    uint32_t handler_max_us = 0;
    uint32_t handler_calls = 0;
    uint32_t handler_over_budget = 0;
    bool display_dimmed = false;
    screensaver_kind_t active_screensaver = SCREENSAVER_NONE;
    TickType_t screensaver_retry_at = 0;

    while (true) {
        TickType_t now = xTaskGetTickCount();
        if (!display_dimmed && now - display_started >= pdMS_TO_TICKS(DISPLAY_DIM_DELAY_MS)) {
            board_display_set_brightness(25);
            display_dimmed = true;
            ESP_LOGI(TAG, "Display idle: brightness reduced to 25%%");
        }
        if (now - last_update >= pdMS_TO_TICKS(500)) {
            static openwrt_status_t status;
            memset(&status, 0, sizeof(status));
            openwrt_status_get(&status);
            clock_screensaver_update(&status);
            status_dashboard_update();
            last_update = now;

            screensaver_kind_t desired = SCREENSAVER_NONE;
            if (status.screensaver_timeout > 0) {
                uint64_t elapsed_ms = (uint64_t)(now - display_started) * portTICK_PERIOD_MS;
                uint64_t timeout_ms = (uint64_t)status.screensaver_timeout * 1000ULL;
                if (elapsed_ms >= timeout_ms) desired = configured_screensaver(&status);
            }
            if (desired != active_screensaver &&
                (desired == SCREENSAVER_NONE || now >= screensaver_retry_at)) {
                esp_err_t screen_error = set_screensaver(desired, &active_screensaver);
                if (screen_error != ESP_OK) {
                    ESP_LOGE(TAG, "Unable to start configured screensaver: %s",
                             esp_err_to_name(screen_error));
                    screensaver_retry_at = now + pdMS_TO_TICKS(1000);
                }
            }
        }
        if (status_dashboard_process_ui_requests()) {
            bool screensaver_dismissed = active_screensaver != SCREENSAVER_NONE;
            if (screensaver_dismissed) {
                set_screensaver(SCREENSAVER_NONE, &active_screensaver);
                ESP_LOGI(TAG, "Screensaver dismissed; dashboard resumed");
            }
            board_display_set_brightness(50);
            display_started = now;
            display_dimmed = false;
            ESP_LOGI(TAG, "%s: brightness set to 50%%; dim timer reset",
                     screensaver_dismissed ? "User activity" : "Manual page switch");
        }
        int64_t handler_started = esp_timer_get_time();
        status_dashboard_animate_frame();
        board_display_handle();
        uint32_t handler_us = (uint32_t)(esp_timer_get_time() - handler_started);
        handler_total_us += handler_us;
        if (handler_us > handler_max_us) handler_max_us = handler_us;
        if (handler_us > 16667) handler_over_budget++;
        handler_calls++;

        int64_t perf_now = esp_timer_get_time();
        if (perf_now - perf_window_started >= 10000000) {
            board_display_perf_t lcd_perf;
            status_dashboard_perf_t dashboard_perf;
            board_display_take_perf(&lcd_perf);
            status_dashboard_take_perf(&dashboard_perf);
            ESP_LOGI(TAG, "LVGL/10s: calls=%lu avg=%llu us max=%lu us >16.7ms=%lu",
                     (unsigned long)handler_calls,
                     (unsigned long long)(handler_calls ? handler_total_us / handler_calls : 0),
                     (unsigned long)handler_max_us,
                     (unsigned long)handler_over_budget);
            ESP_LOGI(TAG, "LCD/10s: page=%s refresh=%lu flush=%lu DMA=%lu tx=%lu KiB wave CPU/MEM/TEMP/D/U=%lu/%lu/%lu/%lu/%lu",
                     dashboard_perf.traffic_page_visible ? "traffic" : "default",
                     (unsigned long)lcd_perf.refreshes,
                     (unsigned long)lcd_perf.flushes,
                     (unsigned long)lcd_perf.dma_completed,
                     (unsigned long)(lcd_perf.pixels / 512U),
                     (unsigned long)dashboard_perf.liquid_updates[0],
                     (unsigned long)dashboard_perf.liquid_updates[1],
                     (unsigned long)dashboard_perf.liquid_updates[2],
                     (unsigned long)dashboard_perf.liquid_updates[3],
                     (unsigned long)dashboard_perf.liquid_updates[4]);
            perf_window_started = perf_now;
            handler_total_us = 0;
            handler_max_us = 0;
            handler_calls = 0;
            handler_over_budget = 0;
        }

        /* Fractional tick scheduler: at the default 100 Hz FreeRTOS tick this
         * alternates 1/2/2 ticks, averaging exactly 60 handler calls/second. */
        frame_tick_remainder += configTICK_RATE_HZ;
        TickType_t frame_ticks = frame_tick_remainder / 60;
        frame_tick_remainder %= 60;
        if (frame_ticks == 0) frame_ticks = 1;
        xTaskDelayUntil(&last_frame_wake, frame_ticks);
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(board_display_init());
    ESP_ERROR_CHECK(cpu_load_led_start());
    ESP_ERROR_CHECK(status_dashboard_init());
    ESP_ERROR_CHECK(clock_screensaver_init());
    ESP_ERROR_CHECK(test_gif_preload_start());
    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(openwrt_status_start());
    BaseType_t display_created = xTaskCreatePinnedToCore(
        display_task, "lvgl_display", DISPLAY_TASK_STACK_SIZE, NULL,
        DISPLAY_TASK_PRIORITY, NULL, DISPLAY_CORE);
    ESP_ERROR_CHECK(display_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGI(TAG, "LVGL display task pinned to CPU%d; network collection uses CPU%d",
             DISPLAY_CORE, NETWORK_CORE);
    boot_button_start();

    bool connected = false;
    if (wifi_manager_has_credentials()) {
        ESP_LOGI(TAG, "Saved Wi-Fi configuration found; connecting for up to %d seconds",
                 CONFIG_PROV_CONNECT_TIMEOUT_SECONDS);
        ESP_ERROR_CHECK(wifi_manager_connect_saved());

        for (int i = 0; i < CONFIG_PROV_CONNECT_TIMEOUT_SECONDS * 10; ++i) {
            if (wifi_manager_get_state() == WIFI_MANAGER_CONNECTED) {
                ESP_LOGI(TAG, "Connected using saved configuration");
                connected = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!connected) {
            ESP_LOGW(TAG, "Saved network connection timed out; opening provisioning portal");
        }
    } else {
        ESP_LOGI(TAG, "No saved Wi-Fi configuration; opening provisioning portal");
    }

    if (!connected) ESP_ERROR_CHECK(wifi_manager_start_provisioning());

    while (true) {
        /* app_main only owns startup policy after the UI task is running. */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
