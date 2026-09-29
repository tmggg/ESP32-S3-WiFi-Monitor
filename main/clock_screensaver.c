#include "clock_screensaver.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "board_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

LV_FONT_DECLARE(dseg14_64);
LV_FONT_DECLARE(dseg14_40);

typedef struct {
    bool synced;
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    int64_t sync_us;
} local_clock_t;

static const char *TAG = "clock_screensaver";
static lv_obj_t *s_page;
static lv_obj_t *s_time;
static lv_obj_t *s_date;
static local_clock_t s_clock;
static uint8_t s_shift_index = UINT8_MAX;

static bool leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int days_in_month(int year, int month)
{
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30,
                                   31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && leap_year(year)) return 29;
    return days[month - 1];
}

static void sync_once(const openwrt_status_t *status)
{
    if (s_clock.synced || !status || !status->valid ||
        !status->system_date[0] || !status->system_time[0]) {
        return;
    }

    local_clock_t parsed = {0};
    if (sscanf(status->system_date, "%d-%d-%d",
               &parsed.year, &parsed.month, &parsed.day) != 3 ||
        sscanf(status->system_time, "%d:%d:%d",
               &parsed.hour, &parsed.minute, &parsed.second) != 3 ||
        parsed.year < 2000 || parsed.month < 1 || parsed.month > 12 ||
        parsed.day < 1 || parsed.day > days_in_month(parsed.year, parsed.month) ||
        parsed.hour < 0 || parsed.hour > 23 ||
        parsed.minute < 0 || parsed.minute > 59 ||
        parsed.second < 0 || parsed.second > 59) {
        ESP_LOGW(TAG, "Ignoring invalid gateway date/time: %s %s",
                 status->system_date, status->system_time);
        return;
    }
    parsed.sync_us = esp_timer_get_time();
    parsed.synced = true;
    s_clock = parsed;
    ESP_LOGI(TAG, "Clock synchronized from gateway: %s %s",
             status->system_date, status->system_time);
}

static void set_label_if_changed(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void update_labels(void)
{
    if (!s_clock.synced) {
        set_label_if_changed(s_time, "--:--:--");
        set_label_if_changed(s_date, "----------");
        return;
    }

    uint64_t elapsed = (uint64_t)(esp_timer_get_time() - s_clock.sync_us) / 1000000ULL;
    uint64_t total = (uint64_t)s_clock.hour * 3600ULL +
                     (uint64_t)s_clock.minute * 60ULL +
                     (uint64_t)s_clock.second + elapsed;
    uint64_t extra_days = total / 86400ULL;
    total %= 86400ULL;
    int year = s_clock.year;
    int month = s_clock.month;
    int day = s_clock.day;
    while (extra_days > 0) {
        extra_days--;
        if (++day > days_in_month(year, month)) {
            day = 1;
            if (++month > 12) {
                month = 1;
                year++;
            }
        }
    }

    unsigned int hour = (unsigned int)(total / 3600ULL);
    unsigned int minute = (unsigned int)((total % 3600ULL) / 60ULL);
    unsigned int second = (unsigned int)(total % 60ULL);
    static const int8_t shift_x[] = {0, 2, 3, 2, 0, -2, -3, -2};
    static const int8_t shift_y[] = {-3, -2, 0, 2, 3, 2, 0, -2};
    uint8_t shift = (uint8_t)((total / 60ULL) %
                              (sizeof(shift_x) / sizeof(shift_x[0])));
    if (shift != s_shift_index) {
        lv_obj_align(s_time, LV_ALIGN_CENTER, shift_x[shift], -42 + shift_y[shift]);
        lv_obj_align(s_date, LV_ALIGN_CENTER, shift_x[shift], 42 + shift_y[shift]);
        s_shift_index = shift;
    }

    char time_text[40];
    char date_text[40];
    if ((second & 1U) == 0U) {
        snprintf(time_text, sizeof(time_text), "%02u:%02u:%02u", hour, minute, second);
    } else {
        snprintf(time_text, sizeof(time_text), "%02u#071321 :#%02u#071321 :#%02u",
                 hour, minute, second);
    }
    snprintf(date_text, sizeof(date_text), "%04d-%02d-%02d", year, month, day);
    set_label_if_changed(s_time, time_text);
    set_label_if_changed(s_date, date_text);
}

esp_err_t clock_screensaver_init(void)
{
    board_display_lock();
    s_page = lv_obj_create(lv_scr_act());
    if (!s_page) {
        board_display_unlock();
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_size(s_page, 320, 172);
    lv_obj_set_pos(s_page, 0, 0);
    lv_obj_set_style_radius(s_page, 0, 0);
    lv_obj_set_style_border_width(s_page, 0, 0);
    lv_obj_set_style_pad_all(s_page, 0, 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_page, lv_color_hex(0x071321), 0);
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);

    s_time = lv_label_create(s_page);
    lv_label_set_text(s_time, "--:--:--");
    lv_label_set_recolor(s_time, true);
    lv_obj_set_style_text_font(s_time, &dseg14_64, 0);
    lv_obj_set_style_text_letter_space(s_time, -4, 0);
    lv_obj_set_style_text_color(s_time, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_time, LV_ALIGN_CENTER, 0, -42);

    s_date = lv_label_create(s_page);
    lv_label_set_text(s_date, "----------");
    lv_obj_set_style_text_font(s_date, &dseg14_40, 0);
    lv_obj_set_style_text_letter_space(s_date, -2, 0);
    lv_obj_set_style_text_color(s_date, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_date, LV_ALIGN_CENTER, 0, 42);
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    board_display_unlock();
    return ESP_OK;
}

void clock_screensaver_update(const openwrt_status_t *status)
{
    sync_once(status);
    board_display_lock();
    update_labels();
    board_display_unlock();
}

void clock_screensaver_show(void)
{
    if (!s_page) return;
    lv_obj_move_foreground(s_page);
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_HIDDEN);
}

void clock_screensaver_hide(void)
{
    if (!s_page || lv_obj_has_flag(s_page, LV_OBJ_FLAG_HIDDEN)) return;
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);
}

bool clock_screensaver_is_visible(void)
{
    return s_page && !lv_obj_has_flag(s_page, LV_OBJ_FLAG_HIDDEN);
}
