#include "board_display.h"

#include <stdatomic.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "Vernon_ST7789T/Vernon_ST7789T.h"

#define LCD_HOST SPI3_HOST
#define LCD_PIXEL_CLOCK_HZ (80 * 1000 * 1000)
#define LCD_PIN_SCLK 40
#define LCD_PIN_MOSI 45
#define LCD_PIN_CS 42
#define LCD_PIN_DC 41
#define LCD_PIN_RST 39
#define LCD_PIN_BACKLIGHT 46
#define LCD_H_RES 320
#define LCD_V_RES 172
#define LCD_Y_GAP 34
#define LCD_DRAW_LINES 40
#define LVGL_TICK_MS 2

static const char *TAG = "display";
static esp_lcd_panel_handle_t s_panel;
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;
static DMA_ATTR lv_color_t s_buffer_1[LCD_H_RES * LCD_DRAW_LINES];
static DMA_ATTR lv_color_t s_buffer_2[LCD_H_RES * LCD_DRAW_LINES];
static SemaphoreHandle_t s_lvgl_lock;
static atomic_uint s_refreshes;
static atomic_uint s_flushes;
static atomic_uint s_dma_completed;
static atomic_uint s_pixels;

static bool flush_ready(esp_lcd_panel_io_handle_t io,
                        esp_lcd_panel_io_event_data_t *event, void *ctx)
{
    atomic_fetch_add_explicit(&s_dma_completed, 1, memory_order_relaxed);
    lv_disp_flush_ready((lv_disp_drv_t *)ctx);
    return false;
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *pixels)
{
    esp_err_t err = esp_lcd_panel_draw_bitmap(drv->user_data, area->x1, area->y1,
                                               area->x2 + 1, area->y2 + 1, pixels);
    if (err == ESP_OK) {
        atomic_fetch_add_explicit(&s_flushes, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_pixels,
                                  (uint32_t)(area->x2 - area->x1 + 1) *
                                  (uint32_t)(area->y2 - area->y1 + 1),
                                  memory_order_relaxed);
        if (lv_disp_flush_is_last(drv)) {
            atomic_fetch_add_explicit(&s_refreshes, 1, memory_order_relaxed);
        }
    } else {
        ESP_LOGE(TAG, "LCD flush failed: %s", esp_err_to_name(err));
        /* No completion callback follows a rejected transfer. */
        lv_disp_flush_ready(drv);
    }
}

void board_display_take_perf(board_display_perf_t *perf)
{
    if (!perf) return;
    perf->refreshes = atomic_exchange_explicit(&s_refreshes, 0, memory_order_relaxed);
    perf->flushes = atomic_exchange_explicit(&s_flushes, 0, memory_order_relaxed);
    perf->dma_completed = atomic_exchange_explicit(&s_dma_completed, 0, memory_order_relaxed);
    perf->pixels = atomic_exchange_explicit(&s_pixels, 0, memory_order_relaxed);
}

static void flush_wait_cb(lv_disp_drv_t *drv)
{
    (void)drv;
    /* LVGL otherwise busy-spins while the partial double buffer is owned by
     * SPI DMA.  Let the idle task run so it can service the task watchdog. */
    vTaskDelay(1);
}

static void tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_MS);
}

static esp_err_t init_backlight(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_channel_config_t channel = {
        .gpio_num = LCD_PIN_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 6500,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer failed");
    return ledc_channel_config(&channel);
}

void board_display_set_brightness(uint8_t percent)
{
    if (percent > 100) percent = 100;
    uint32_t duty = (8191U * percent) / 100U;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

esp_err_t board_display_init(void)
{
    s_lvgl_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lvgl_lock) return ESP_ERR_NO_MEM;

    spi_bus_config_t bus = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_DRAW_LINES * sizeof(lv_color_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "SPI bus init failed");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = flush_ready,
        .user_ctx = &s_disp_drv,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                                 &io_config, &io), TAG, "panel IO failed");

    esp_lcd_panel_dev_st7789t_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_endian = LCD_RGB_ENDIAN_BGR,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789t(io, &panel_config, &s_panel),
                        TAG, "ST7789 panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, true), TAG, "panel rotation failed");
    /* With XY swapped, panel Y controls logical left/right. Flip that axis
     * to correct the horizontally mirrored image seen on the S3 panel. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, false, false), TAG, "panel mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, 0, LCD_Y_GAP), TAG, "panel gap failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "panel on failed");
    ESP_RETURN_ON_ERROR(init_backlight(), TAG, "backlight failed");
    board_display_set_brightness(80);

    lv_init();
    lv_disp_draw_buf_init(&s_draw_buf, s_buffer_1, s_buffer_2,
                          LCD_H_RES * LCD_DRAW_LINES);
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = LCD_H_RES;
    s_disp_drv.ver_res = LCD_V_RES;
    s_disp_drv.flush_cb = flush_cb;
    s_disp_drv.wait_cb = flush_wait_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.user_data = s_panel;
    lv_disp_drv_register(&s_disp_drv);

    const esp_timer_create_args_t timer_args = {
        .callback = tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t timer_handle;
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &timer_handle), TAG, "LVGL timer failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(timer_handle, LVGL_TICK_MS * 1000),
                        TAG, "LVGL tick failed");
    ESP_LOGI(TAG, "LCD ready in landscape mode (%dx%d)", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

void board_display_lock(void)
{
    xSemaphoreTakeRecursive(s_lvgl_lock, portMAX_DELAY);
}

void board_display_unlock(void)
{
    xSemaphoreGiveRecursive(s_lvgl_lock);
}

void board_display_handle(void)
{
    board_display_lock();
    lv_timer_handler();
    board_display_unlock();
}
