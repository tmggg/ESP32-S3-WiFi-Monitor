#include "cpu_load_led.h"

#include <stdint.h>
#include "esp_log.h"
#include "led_strip.h"

#define RGB_LED_GPIO 38
#define LED_LEVEL_UNAVAILABLE UINT8_MAX

static const char *TAG = "cpu_led";
static led_strip_handle_t s_led;
static uint8_t s_last_level = LED_LEVEL_UNAVAILABLE;

static uint8_t load_level(float load_percent)
{
    if (load_percent < 25.0f) return 0;
    if (load_percent < 50.0f) return 1;
    if (load_percent < 75.0f) return 2;
    return 3;
}

void cpu_load_led_set_openwrt_load(float load_percent)
{
    if (!s_led) return;
    if (!(load_percent >= 0.0f)) load_percent = 0.0f;
    if (load_percent > 100.0f) load_percent = 100.0f;

    uint8_t level = load_level(load_percent);
    if (level == s_last_level) return;

    uint8_t red = 0, green = 0, blue = 0;
    if (level == 0) {
        green = 32;                    /* 0-24%: green */
    } else if (level == 1) {
        blue = 32;                     /* 25-49%: blue */
    } else if (level == 2) {
        red = 32; green = 22;          /* 50-74%: yellow */
    } else {
        red = 40;                      /* 75-100%: red */
    }

    /* The onboard LED's physical red/green order is reversed relative to
     * the led_strip logical RGB arguments on this board. */
    led_strip_set_pixel(s_led, 0, green, red, blue);
    if (led_strip_refresh(s_led) == ESP_OK) s_last_level = level;
}

void cpu_load_led_set_unavailable(void)
{
    if (!s_led || s_last_level == LED_LEVEL_UNAVAILABLE) return;
    if (led_strip_clear(s_led) == ESP_OK) s_last_level = LED_LEVEL_UNAVAILABLE;
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
    err = led_strip_clear(s_led);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "OpenWRT CPU load RGB indicator started on GPIO%d", RGB_LED_GPIO);
    return ESP_OK;
}
