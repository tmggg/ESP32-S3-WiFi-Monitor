#include "cpu_load_led.h"

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_freertos_hooks.h"
#include "esp_log.h"
#include "led_strip.h"

#define RGB_LED_GPIO 38
#define CPU_SAMPLE_MS 1000
#define CPU_LOAD_TASK_CORE 0

static const char *TAG = "cpu_led";
static led_strip_handle_t s_led;
static volatile uint32_t s_idle_iterations;

static bool idle_hook(void)
{
    s_idle_iterations++;
    /* false asks ESP-IDF to call this hook repeatedly while the CPU remains
     * idle. Returning true limits it to once per RTOS tick and badly
     * overestimates load when a 60 Hz UI task wakes on most ticks. */
    return false;
}

static void set_load_color(uint32_t load_percent)
{
    uint8_t red = 0, green = 0, blue = 0;
    if (load_percent < 25) {
        green = 32;                    /* 0-24%: green */
    } else if (load_percent < 50) {
        blue = 32;                     /* 25-49%: blue */
    } else if (load_percent < 75) {
        red = 32; green = 22;          /* 50-74%: yellow */
    } else {
        red = 40;                      /* 75-100%: red */
    }
    /* The S3 board vendor demo uses the led_strip component's normal logical
     * RGB order for its GPIO38 addressable LED. */
    led_strip_set_pixel(s_led, 0, red, green, blue);
    led_strip_refresh(s_led);
}

static void cpu_led_task(void *arg)
{
    uint32_t maximum_idle_iterations = 0;
    uint32_t smoothed_load = 0;
    while (true) {
        s_idle_iterations = 0;
        vTaskDelay(pdMS_TO_TICKS(CPU_SAMPLE_MS));
        uint32_t idle_iterations = s_idle_iterations;
        if (idle_iterations > maximum_idle_iterations) {
            maximum_idle_iterations = idle_iterations;
        }

        uint32_t raw_load = 0;
        if (maximum_idle_iterations > 0 && idle_iterations < maximum_idle_iterations) {
            raw_load = 100U - (uint32_t)(((uint64_t)idle_iterations * 100U) /
                                         maximum_idle_iterations);
        }
        if (raw_load > 100) raw_load = 100;
        smoothed_load = (smoothed_load * 3U + raw_load + 2U) / 4U;
        set_load_color(smoothed_load);
    }
}

esp_err_t cpu_load_led_start(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_LED_GPIO,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led);
    if (err != ESP_OK) return err;
    led_strip_clear(s_led);

    err = esp_register_freertos_idle_hook_for_cpu(idle_hook, 0);
    if (err != ESP_OK) return err;
    if (xTaskCreatePinnedToCore(cpu_led_task, "cpu_load_led", 3072, NULL, 3,
                                NULL, CPU_LOAD_TASK_CORE) != pdPASS) {
        esp_deregister_freertos_idle_hook_for_cpu(idle_hook, 0);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "CPU load RGB indicator started on GPIO%d", RGB_LED_GPIO);
    return ESP_OK;
}
