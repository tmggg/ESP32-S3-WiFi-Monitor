#include "boot_button.h"
#include "board_display.h"
#include "cpu_load_led.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "openwrt_status.h"
#include "status_dashboard.h"
#include "wifi_manager.h"

static const char *TAG = "main";

#define DISPLAY_DIM_DELAY_MS 60000
#define DISPLAY_TASK_STACK_SIZE 6144
#define DISPLAY_TASK_PRIORITY 5
#define NETWORK_CORE 0
#define DISPLAY_CORE 1

static void display_task(void *arg)
{
    (void)arg;
    TickType_t display_started = xTaskGetTickCount();
    TickType_t last_update = 0;
    TickType_t last_frame_wake = display_started;
    uint32_t frame_tick_remainder = 0;
    bool display_dimmed = false;

    while (true) {
        TickType_t now = xTaskGetTickCount();
        if (!display_dimmed && now - display_started >= pdMS_TO_TICKS(DISPLAY_DIM_DELAY_MS)) {
            board_display_set_brightness(50);
            display_dimmed = true;
            ESP_LOGI(TAG, "Display brightness reduced to 50%%");
        }
        if (now - last_update >= pdMS_TO_TICKS(500)) {
            status_dashboard_update();
            last_update = now;
        }
        if (status_dashboard_process_ui_requests()) {
            board_display_set_brightness(50);
            display_started = now;
            display_dimmed = false;
            ESP_LOGI(TAG, "Manual page switch: brightness set to 50%%; dim timer reset");
        }
        board_display_handle();

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
    status_dashboard_init();
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
