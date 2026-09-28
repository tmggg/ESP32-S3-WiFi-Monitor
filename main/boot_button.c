#include "boot_button.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "status_dashboard.h"
#include "wifi_manager.h"

static const char *TAG = "boot_button";
#define BOOT_DOUBLE_CLICK_MS 350
#define BOOT_BUTTON_TASK_STACK_BYTES 4096

static void button_task(void *arg)
{
    int held_ms = 0;
    bool fired = false;
    bool click_pending = false;
    bool second_press = false;
    TickType_t first_release_tick = 0;
    while (true) {
        TickType_t now = xTaskGetTickCount();
        if (gpio_get_level(CONFIG_PROV_BOOT_BUTTON_GPIO) == 0) {
            if (held_ms == 0 && click_pending) {
                if (now - first_release_tick <= pdMS_TO_TICKS(BOOT_DOUBLE_CLICK_MS)) {
                    second_press = true;
                } else {
                    status_dashboard_request_page_toggle();
                    click_pending = false;
                }
            }
            held_ms += 50;
            if (!fired && held_ms >= CONFIG_PROV_BOOT_BUTTON_HOLD_MS) {
                fired = true;
                click_pending = false;
                second_press = false;
                ESP_LOGW(TAG, "BOOT held: clearing Wi-Fi configuration and starting portal");
                ESP_ERROR_CHECK_WITHOUT_ABORT(wifi_manager_clear_credentials());
            }
        } else {
            if (held_ms >= 50 && !fired) {
                if (second_press && click_pending) {
                    ESP_LOGI(TAG, "BOOT double-click: toggling interface rotation lock");
                    status_dashboard_request_interface_lock_toggle();
                    click_pending = false;
                } else if (status_dashboard_is_default_page()) {
                    click_pending = true;
                    first_release_tick = now;
                } else {
                    ESP_LOGI(TAG, "BOOT pressed: toggling dashboard page");
                    status_dashboard_request_page_toggle();
                }
            }
            held_ms = 0;
            fired = false;
            second_press = false;
            if (click_pending && now - first_release_tick >=
                                     pdMS_TO_TICKS(BOOT_DOUBLE_CLICK_MS)) {
                ESP_LOGI(TAG, "BOOT pressed: toggling dashboard page");
                status_dashboard_request_page_toggle();
                click_pending = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void boot_button_start(void)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << CONFIG_PROV_BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    /* Logging and the long-press Wi-Fi reset path need more than 2 KiB. */
    BaseType_t created = xTaskCreatePinnedToCore(
        button_task, "boot_button", BOOT_BUTTON_TASK_STACK_BYTES, NULL, 4, NULL, 0);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
