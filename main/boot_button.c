#include "boot_button.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "status_dashboard.h"
#include "wifi_manager.h"

static const char *TAG = "boot_button";

static void button_task(void *arg)
{
    int held_ms = 0;
    bool fired = false;
    while (true) {
        if (gpio_get_level(CONFIG_PROV_BOOT_BUTTON_GPIO) == 0) {
            held_ms += 50;
            if (!fired && held_ms >= CONFIG_PROV_BOOT_BUTTON_HOLD_MS) {
                fired = true;
                ESP_LOGW(TAG, "BOOT held: clearing Wi-Fi configuration and starting portal");
                ESP_ERROR_CHECK_WITHOUT_ABORT(wifi_manager_clear_credentials());
            }
        } else {
            if (held_ms >= 50 && !fired) {
                ESP_LOGI(TAG, "BOOT pressed: toggling dashboard page");
                status_dashboard_request_page_toggle();
            }
            held_ms = 0;
            fired = false;
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
    xTaskCreatePinnedToCore(button_task, "boot_button", 2048, NULL, 4, NULL, 0);
}
