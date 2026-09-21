#include "status_dashboard.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "board_display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "openwrt_status.h"

#define HIGH_TRAFFIC_THRESHOLD_BPS (10ULL * 1024ULL * 1024ULL)
#define FULLSCREEN_TRAFFIC_ENABLED 1
#define DOWNLOAD_FULL_SCALE_BPS (250ULL * 1024ULL * 1024ULL)
#define UPLOAD_FULL_SCALE_BPS (10ULL * 1024ULL * 1024ULL)
#define TRAFFIC_HISTORY_SECONDS 120
#define TRAFFIC_DEFAULT_WINDOW_SECONDS 10
#define X_AXIS_MAJOR_TICKS 7
#define CHART_ANIMATION_MS 650
#define CHART_SCALE_DIVISOR 1250ULL /* 0.01 Mbps expressed as bytes/second. */
#define CHART_VALUE_LIMIT 30000
#define SMALL_CARD_WIDTH 76
#define SMALL_CARD_HEIGHT 55
#define WAN_CARD_WIDTH 156
#define WAN_CARD_HEIGHT 78
#define LIQUID_WAVE_PERIOD 48
#define LIQUID_LEVEL_ANIMATION_MS 600
#define LIQUID_WAVE_ANIMATION_MS 1067
#define DEFAULT_INTERFACE_ROTATION_US (10LL * 1000LL * 1000LL)

static lv_obj_t *s_connection;
static lv_obj_t *s_title;
static lv_obj_t *s_cpu;
static lv_obj_t *s_memory;
static lv_obj_t *s_temperature;
static lv_obj_t *s_uptime;
static lv_obj_t *s_download_speed;
static lv_obj_t *s_download_total;
static lv_obj_t *s_upload_speed;
static lv_obj_t *s_upload_total;
static lv_obj_t *s_traffic_alert;
static lv_obj_t *s_traffic_alert_title;
static lv_obj_t *s_traffic_alert_rate;
static lv_obj_t *s_traffic_alert_upload_rate;
static lv_obj_t *s_traffic_alert_stats;
static lv_obj_t *s_traffic_chart;
static lv_chart_series_t *s_download_series;
static lv_chart_series_t *s_upload_series;
typedef struct {
    bool active;
    char name[OPENWRT_INTERFACE_NAME_LEN];
    uint32_t download[TRAFFIC_HISTORY_SECONDS];
    uint32_t upload[TRAFFIC_HISTORY_SECONDS];
    uint32_t index;
    uint32_t count;
} traffic_history_t;

static traffic_history_t *s_interface_histories;
static uint32_t s_last_sample_id;
static bool s_was_high_traffic;
static int8_t s_manual_history_slot = -1;
static int8_t s_visible_history_slot = -1;
static int8_t s_wan_history_slot = -1;
static uint8_t s_default_interface_index;
static int64_t s_default_interface_switch_us;
static atomic_bool s_page_toggle_requested;
static uint32_t s_x_window_seconds = TRAFFIC_DEFAULT_WINDOW_SECONDS;
static int32_t s_liquid_wave_phase;
static uint8_t s_liquid_render_slot;

typedef struct {
    lv_obj_t *canvas;
    lv_obj_t *title;
    lv_color_t *buffer;
    int16_t width;
    int16_t height;
    int32_t level;
    int32_t target;
    uint8_t phase_offset;
    lv_color_t accent;
    lv_color_t water;
    lv_color_t crest;
} liquid_card_t;

static liquid_card_t s_cpu_liquid;
static liquid_card_t s_memory_liquid;
static liquid_card_t s_temperature_liquid;
static liquid_card_t s_download_liquid;
static liquid_card_t s_upload_liquid;
static lv_color_t s_cpu_canvas_buffer[(SMALL_CARD_WIDTH - 2) * (SMALL_CARD_HEIGHT - 2)];
static lv_color_t s_memory_canvas_buffer[(SMALL_CARD_WIDTH - 2) * (SMALL_CARD_HEIGHT - 2)];
static lv_color_t s_temperature_canvas_buffer[(SMALL_CARD_WIDTH - 2) * (SMALL_CARD_HEIGHT - 2)];
static lv_color_t s_download_canvas_buffer[(WAN_CARD_WIDTH - 2) * (WAN_CARD_HEIGHT - 2)];
static lv_color_t s_upload_canvas_buffer[(WAN_CARD_WIDTH - 2) * (WAN_CARD_HEIGHT - 2)];

typedef struct {
    uint16_t point_count;
    uint16_t revealed;
    lv_coord_t download[TRAFFIC_HISTORY_SECONDS];
    lv_coord_t upload[TRAFFIC_HISTORY_SECONDS];
} chart_redraw_animation_t;

static chart_redraw_animation_t s_redraw_animation;

static void animate_waveform_redraw(void *context, int32_t visible_points)
{
    chart_redraw_animation_t *redraw = context;
    if (visible_points > redraw->point_count) visible_points = redraw->point_count;
    while (redraw->revealed < visible_points) {
        uint16_t point = redraw->revealed++;
        lv_chart_set_value_by_id(s_traffic_chart, s_download_series, point,
                                 redraw->download[point]);
        lv_chart_set_value_by_id(s_traffic_chart, s_upload_series, point,
                                 redraw->upload[point]);
    }
}

static void start_waveform_redraw(int history_slot)
{
    if (history_slot < 0 || history_slot >= OPENWRT_MAX_INTERFACES) return;
    traffic_history_t *history = &s_interface_histories[history_slot];
    lv_anim_del(&s_redraw_animation, NULL);
    uint32_t desired_window = history->count > TRAFFIC_DEFAULT_WINDOW_SECONDS ?
                              history->count : TRAFFIC_DEFAULT_WINDOW_SECONDS;
    if (desired_window > TRAFFIC_HISTORY_SECONDS) desired_window = TRAFFIC_HISTORY_SECONDS;
    if (desired_window != s_x_window_seconds) {
        s_x_window_seconds = desired_window;
        lv_chart_set_point_count(s_traffic_chart, s_x_window_seconds);
        lv_chart_set_range(s_traffic_chart, LV_CHART_AXIS_PRIMARY_X,
                           0, s_x_window_seconds);
    }
    lv_chart_set_all_value(s_traffic_chart, s_download_series, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(s_traffic_chart, s_upload_series, LV_CHART_POINT_NONE);
    s_redraw_animation.point_count = history->count;
    s_redraw_animation.revealed = 0;

    /* The newest sample is always point zero; older samples extend right. */
    uint32_t peak = 500;
    for (uint32_t i = 0; i < history->count; ++i) {
        uint32_t source = (history->index + TRAFFIC_HISTORY_SECONDS - 1 - i) %
                          TRAFFIC_HISTORY_SECONDS;
        s_redraw_animation.download[i] = history->download[source] > CHART_VALUE_LIMIT ?
                                         CHART_VALUE_LIMIT : history->download[source];
        s_redraw_animation.upload[i] = history->upload[source] > CHART_VALUE_LIMIT ?
                                       CHART_VALUE_LIMIT : history->upload[source];
        if (history->download[source] > peak) peak = history->download[source];
        if (history->upload[source] > peak) peak = history->upload[source];
    }
    uint32_t chart_max = peak + peak / 5;
    if (chart_max > CHART_VALUE_LIMIT) chart_max = CHART_VALUE_LIMIT;
    lv_chart_set_range(s_traffic_chart, LV_CHART_AXIS_SECONDARY_Y, 0, chart_max);

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, &s_redraw_animation);
    lv_anim_set_exec_cb(&animation, animate_waveform_redraw);
    lv_anim_set_values(&animation, 0, history->count);
    lv_anim_set_time(&animation, CHART_ANIMATION_MS);
    lv_anim_set_path_cb(&animation, lv_anim_path_linear);
    lv_anim_start(&animation);
}

void status_dashboard_request_page_toggle(void)
{
    atomic_store(&s_page_toggle_requested, true);
}

bool status_dashboard_process_ui_requests(void)
{
    if (!atomic_exchange(&s_page_toggle_requested, false)) return false;

    board_display_lock();
    int next_slot = -1;
    for (int i = s_manual_history_slot + 1; i < OPENWRT_MAX_INTERFACES; ++i) {
        if (s_interface_histories[i].active) {
            next_slot = i;
            break;
        }
    }
    s_manual_history_slot = next_slot;
    if (s_manual_history_slot >= 0) {
        lv_label_set_text(s_traffic_alert_title,
                          s_interface_histories[s_manual_history_slot].name);
        lv_label_set_text(s_traffic_alert_rate, "D -- Mbps");
        lv_label_set_text(s_traffic_alert_upload_rate, "U -- Mbps");
        lv_label_set_text(s_traffic_alert_stats, "AVG D/U --/--  MAX --/--");
        start_waveform_redraw(s_manual_history_slot);
        s_visible_history_slot = s_manual_history_slot;
        s_was_high_traffic = true;
        lv_obj_clear_flag(s_traffic_alert, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_traffic_alert);
    } else {
        lv_anim_del(&s_redraw_animation, NULL);
        s_visible_history_slot = -1;
        s_was_high_traffic = false;
        lv_obj_add_flag(s_traffic_alert, LV_OBJ_FLAG_HIDDEN);
    }
    board_display_unlock();
    return true;
}

static void chart_draw_event(lv_event_t *event)
{
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(event);
    if (!lv_obj_draw_part_check_type(dsc, &lv_chart_class,
                                     LV_CHART_DRAW_PART_TICK_LABEL) || !dsc->text) {
        return;
    }
    if (dsc->id == LV_CHART_AXIS_PRIMARY_X) {
        /* For a line chart LVGL reports the major tick ordinal (0..N-1),
         * not the configured X-axis value. Scale it to the history window. */
        long seconds = ((long)dsc->value * s_x_window_seconds) /
                       (X_AXIS_MAJOR_TICKS - 1);
        if (seconds == 0) lv_snprintf(dsc->text, dsc->text_length, "0s");
        else lv_snprintf(dsc->text, dsc->text_length, "-%lds", seconds);
    } else if (dsc->id == LV_CHART_AXIS_SECONDARY_Y) {
        long value = dsc->value;
        long rounded_mbps = value >= 0 ? (value + 50) / 100 : (value - 50) / 100;
        lv_snprintf(dsc->text, dsc->text_length, "%ld", rounded_mbps);
    }
}

static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h,
                           const char *title, lv_color_t accent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x17233A), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x263A5B), 0);
    lv_obj_set_style_pad_all(card, 6, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, accent, 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);
    return card;
}

static lv_obj_t *make_value(lv_obj_t *parent, int y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, "--");
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_style_text_color(label, lv_color_hex(0xF2F7FF), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

static void render_liquid_wave(liquid_card_t *liquid)
{
    static const int8_t wave_y[LIQUID_WAVE_PERIOD] = {
         0,  1,  2,  2,  3,  4,  4,  5,
         5,  6,  6,  6,  6,  6,  6,  6,
         5,  5,  4,  4,  3,  2,  2,  1,
         0, -1, -2, -2, -3, -4, -4, -5,
        -5, -6, -6, -6, -6, -6, -6, -6,
        -5, -5, -4, -4, -3, -2, -2, -1,
    };
    const lv_color_t background = lv_color_hex(0x17233A);
    int32_t fill_height = (liquid->height * liquid->level + 50) / 100;
    int32_t surface_y = liquid->height - fill_height;

    for (int32_t x = 0; x < liquid->width; ++x) {
        int32_t wave_surface = surface_y;
        if (liquid->level >= 100) {
            wave_surface = 0;
        } else if (liquid->level > 0) {
            wave_surface += wave_y[(x + s_liquid_wave_phase + liquid->phase_offset) %
                                   LIQUID_WAVE_PERIOD];
        }
        if (wave_surface < 0) wave_surface = 0;
        if (wave_surface > liquid->height) wave_surface = liquid->height;

        for (int32_t y = 0; y < liquid->height; ++y) {
            lv_color_t color = background;
            if (liquid->level > 0 && y >= wave_surface) color = liquid->water;
            if (liquid->level > 0 && y == wave_surface) color = liquid->crest;
            liquid->buffer[y * liquid->width + x] = color;
        }
    }
    lv_obj_invalidate(liquid->canvas);
}

static void liquid_level_anim_cb(void *context, int32_t percent)
{
    liquid_card_t *liquid = context;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (percent == liquid->level) return;
    liquid->level = percent;
    render_liquid_wave(liquid);

    lv_obj_set_style_text_color(liquid->title,
                                percent >= 75 ? lv_color_hex(0xFFFFFF) : liquid->accent, 0);
}

static void set_liquid_level(liquid_card_t *liquid, int32_t target)
{
    if (target < 0) target = 0;
    if (target > 100) target = 100;
    if (target == liquid->target) return;
    liquid->target = target;
    lv_anim_del(liquid, liquid_level_anim_cb);

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, liquid);
    lv_anim_set_exec_cb(&animation, liquid_level_anim_cb);
    lv_anim_set_values(&animation, liquid->level, target);
    lv_anim_set_time(&animation, LIQUID_LEVEL_ANIMATION_MS);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_start(&animation);
}

static void liquid_wave_anim_cb(void *context, int32_t phase)
{
    (void)context;
    phase %= LIQUID_WAVE_PERIOD;
    if (phase == s_liquid_wave_phase) return;
    s_liquid_wave_phase = phase;
    /* The opaque high-traffic page covers these cards, so avoid rendering
     * hidden background animations while it is visible. */
    if (s_traffic_alert && !lv_obj_has_flag(s_traffic_alert, LV_OBJ_FLAG_HIDDEN)) return;
    liquid_card_t *cards[] = {
        &s_cpu_liquid,
        &s_memory_liquid,
        &s_temperature_liquid,
        &s_download_liquid,
        &s_upload_liquid,
    };
    /* Update one card per phase.  Each card still advances about six times
     * per second, but LVGL never has to flush all five canvas areas at once. */
    render_liquid_wave(cards[s_liquid_render_slot]);
    s_liquid_render_slot = (s_liquid_render_slot + 1) % 5;
}

static void start_liquid_wave_animation(void)
{
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, &s_liquid_wave_phase);
    lv_anim_set_exec_cb(&animation, liquid_wave_anim_cb);
    lv_anim_set_values(&animation, 0, LIQUID_WAVE_PERIOD - 1);
    lv_anim_set_time(&animation, LIQUID_WAVE_ANIMATION_MS);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&animation, lv_anim_path_linear);
    lv_anim_start(&animation);
}

static lv_obj_t *make_liquid_card(lv_obj_t *parent, liquid_card_t *liquid,
                                  lv_color_t *buffer, int x, int y, int width, int height,
                                  const char *title, lv_color_t accent, lv_color_t water,
                                  lv_color_t crest, int value_y, lv_obj_t **value)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, width, height);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x17233A), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x263A5B), 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    liquid->buffer = buffer;
    liquid->width = width - 2;
    liquid->height = height - 2;
    liquid->level = -1;
    liquid->target = -1;
    liquid->accent = accent;
    liquid->water = water;
    liquid->crest = crest;
    liquid->canvas = lv_canvas_create(card);
    lv_canvas_set_buffer(liquid->canvas, liquid->buffer,
                         liquid->width, liquid->height,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(liquid->canvas, 1, 1);

    liquid->title = lv_label_create(card);
    lv_label_set_text(liquid->title, title);
    lv_obj_set_pos(liquid->title, 6, 6);
    lv_obj_set_style_text_color(liquid->title, accent, 0);
    *value = make_value(card, value_y);

    liquid_level_anim_cb(liquid, 0);
    return card;
}

esp_err_t status_dashboard_init(void)
{
    /* History is read once per sample/redraw, so keep it in PSRAM. The hot
     * Canvas and SPI DMA buffers remain in internal RAM for display speed. */
    s_interface_histories = heap_caps_calloc(OPENWRT_MAX_INTERFACES,
                                             sizeof(*s_interface_histories),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_interface_histories) {
        ESP_LOGE("dashboard", "Failed to allocate %u bytes of interface history in PSRAM",
                 (unsigned)(OPENWRT_MAX_INTERFACES * sizeof(*s_interface_histories)));
        return ESP_ERR_NO_MEM;
    }

    board_display_lock();
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x09111F), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    s_title = lv_label_create(screen);
    lv_label_set_text(s_title, "HOST --");
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_width(s_title, 180);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 7, 6);

    s_connection = lv_label_create(screen);
    lv_label_set_text(s_connection, "WAITING");
    lv_obj_set_style_text_color(s_connection, lv_color_hex(0xFFB84D), 0);
    lv_obj_align(s_connection, LV_ALIGN_TOP_RIGHT, -7, 6);

    make_liquid_card(screen, &s_cpu_liquid, s_cpu_canvas_buffer,
                     2, 28, SMALL_CARD_WIDTH, SMALL_CARD_HEIGHT, "CPU",
                     lv_color_hex(0xA8E66A), lv_color_hex(0x527A2A),
                     lv_color_hex(0xD4F5A3), 22, &s_cpu);
    s_memory_liquid.phase_offset = 10;
    make_liquid_card(screen, &s_memory_liquid, s_memory_canvas_buffer,
                     82, 28, SMALL_CARD_WIDTH, SMALL_CARD_HEIGHT, "MEM",
                     lv_color_hex(0x6798FF), lv_color_hex(0x345CC4),
                     lv_color_hex(0x82AEFF), 22, &s_memory);
    s_temperature_liquid.phase_offset = 20;
    make_liquid_card(screen, &s_temperature_liquid, s_temperature_canvas_buffer,
                     162, 28, SMALL_CARD_WIDTH, SMALL_CARD_HEIGHT, "TEMP",
                     lv_color_hex(0xFF8A65), lv_color_hex(0xA94834),
                     lv_color_hex(0xFFAA8B), 22, &s_temperature);
    lv_obj_t *uptime_card = make_card(screen, 242, 28, 76, 55, "UPTIME", lv_color_hex(0xC792EA));
    s_uptime = make_value(uptime_card, 18);

    s_download_liquid.phase_offset = 30;
    lv_obj_t *download_card = make_liquid_card(
        screen, &s_download_liquid, s_download_canvas_buffer,
        2, 87, WAN_CARD_WIDTH, WAN_CARD_HEIGHT, "DOWNLOAD",
        lv_color_hex(0x44D7B6), lv_color_hex(0x167A68),
        lv_color_hex(0x7DF2D6), 25, &s_download_speed);
    s_download_total = lv_label_create(download_card);
    lv_label_set_text(s_download_total, "TOTAL --");
    lv_obj_set_width(s_download_total, lv_pct(100));
    lv_obj_set_style_text_align(s_download_total, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_download_total, lv_color_hex(0xF2F7FF), 0);
    lv_obj_align(s_download_total, LV_ALIGN_BOTTOM_MID, 0, -6);

    s_upload_liquid.phase_offset = 40;
    lv_obj_t *upload_card = make_liquid_card(
        screen, &s_upload_liquid, s_upload_canvas_buffer,
        162, 87, WAN_CARD_WIDTH, WAN_CARD_HEIGHT, "UPLOAD",
        lv_color_hex(0xFFB84D), lv_color_hex(0xA96D20),
        lv_color_hex(0xFFD080), 25, &s_upload_speed);
    s_upload_total = lv_label_create(upload_card);
    lv_label_set_text(s_upload_total, "TOTAL --");
    lv_obj_set_width(s_upload_total, lv_pct(100));
    lv_obj_set_style_text_align(s_upload_total, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_upload_total, lv_color_hex(0xF2F7FF), 0);
    lv_obj_align(s_upload_total, LV_ALIGN_BOTTOM_MID, 0, -6);

    s_traffic_alert = lv_obj_create(screen);
    lv_obj_set_size(s_traffic_alert, 320, 172);
    lv_obj_set_pos(s_traffic_alert, 0, 0);
    lv_obj_set_style_radius(s_traffic_alert, 0, 0);
    lv_obj_set_style_border_width(s_traffic_alert, 0, 0);
    lv_obj_set_style_pad_all(s_traffic_alert, 0, 0);
    lv_obj_set_style_bg_opa(s_traffic_alert, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_traffic_alert, lv_color_hex(0x071321), 0);
    lv_obj_clear_flag(s_traffic_alert, LV_OBJ_FLAG_SCROLLABLE);

    s_traffic_alert_title = lv_label_create(s_traffic_alert);
    lv_label_set_text(s_traffic_alert_title, "HIGH DOWNLOAD");
    lv_obj_set_style_text_color(s_traffic_alert_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_traffic_alert_title, LV_ALIGN_TOP_MID, 0, 4);

    s_traffic_alert_rate = lv_label_create(s_traffic_alert);
    lv_label_set_text(s_traffic_alert_rate, "D 0.00 Mbps");
    lv_obj_set_style_text_color(s_traffic_alert_rate, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(s_traffic_alert_rate, 49, 21);

    lv_obj_t *download_dot = lv_obj_create(s_traffic_alert);
    lv_obj_set_size(download_dot, 7, 7);
    lv_obj_set_pos(download_dot, 37, 27);
    lv_obj_set_style_radius(download_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(download_dot, 0, 0);
    lv_obj_set_style_pad_all(download_dot, 0, 0);
    lv_obj_set_style_bg_color(download_dot, lv_color_hex(0x44D7B6), 0);
    lv_obj_clear_flag(download_dot, LV_OBJ_FLAG_SCROLLABLE);

    s_traffic_alert_upload_rate = lv_label_create(s_traffic_alert);
    lv_label_set_text(s_traffic_alert_upload_rate, "U 0.00 Mbps");
    lv_obj_set_style_text_color(s_traffic_alert_upload_rate, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(s_traffic_alert_upload_rate, 190, 21);

    lv_obj_t *upload_dot = lv_obj_create(s_traffic_alert);
    lv_obj_set_size(upload_dot, 7, 7);
    lv_obj_set_pos(upload_dot, 178, 27);
    lv_obj_set_style_radius(upload_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(upload_dot, 0, 0);
    lv_obj_set_style_pad_all(upload_dot, 0, 0);
    lv_obj_set_style_bg_color(upload_dot, lv_color_hex(0xFFB84D), 0);
    lv_obj_clear_flag(upload_dot, LV_OBJ_FLAG_SCROLLABLE);

    s_traffic_alert_stats = lv_label_create(s_traffic_alert);
    lv_label_set_text(s_traffic_alert_stats, "AVG D/U 0.00/0.00  MAX 0.00/0.00");
    lv_obj_set_style_text_color(s_traffic_alert_stats, lv_color_hex(0xB9C8DA), 0);
    lv_obj_set_style_text_font(s_traffic_alert_stats, &lv_font_montserrat_12, 0);
    lv_obj_align(s_traffic_alert_stats, LV_ALIGN_TOP_MID, 0, 39);

    s_traffic_chart = lv_chart_create(s_traffic_alert);
    /* Reserve the upper 50 px for title/current values and leave room around
     * the plot for visible Y and X tick labels. */
    lv_obj_set_size(s_traffic_chart, 264, 76);
    lv_obj_set_pos(s_traffic_chart, 8, 69);
    lv_chart_set_type(s_traffic_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(s_traffic_chart, LV_CHART_UPDATE_MODE_CIRCULAR);
    lv_chart_set_point_count(s_traffic_chart, TRAFFIC_DEFAULT_WINDOW_SECONDS);
    lv_chart_set_range(s_traffic_chart, LV_CHART_AXIS_SECONDARY_Y, 0, 500);
    lv_chart_set_range(s_traffic_chart, LV_CHART_AXIS_PRIMARY_X,
                       0, TRAFFIC_DEFAULT_WINDOW_SECONDS);
    lv_chart_set_div_line_count(s_traffic_chart, 4, 6);
    lv_chart_set_axis_tick(s_traffic_chart, LV_CHART_AXIS_SECONDARY_Y,
                           5, 3, 4, 2, true, 44);
    lv_chart_set_axis_tick(s_traffic_chart, LV_CHART_AXIS_PRIMARY_X,
                           5, 3, X_AXIS_MAJOR_TICKS, 2, true, 18);
    lv_obj_add_event_cb(s_traffic_chart, chart_draw_event,
                        LV_EVENT_DRAW_PART_BEGIN, NULL);
    lv_obj_set_style_bg_color(s_traffic_chart, lv_color_hex(0x0B1D30), 0);
    lv_obj_set_style_bg_opa(s_traffic_chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_traffic_chart, lv_color_hex(0x29415E), 0);
    lv_obj_set_style_line_color(s_traffic_chart, lv_color_hex(0x203750), LV_PART_MAIN);
    lv_obj_set_style_size(s_traffic_chart, 0, LV_PART_INDICATOR);
    s_download_series = lv_chart_add_series(s_traffic_chart, lv_color_hex(0x44D7B6),
                                            LV_CHART_AXIS_SECONDARY_Y);
    s_upload_series = lv_chart_add_series(s_traffic_chart, lv_color_hex(0xFFB84D),
                                          LV_CHART_AXIS_SECONDARY_Y);
    lv_obj_add_flag(s_traffic_alert, LV_OBJ_FLAG_HIDDEN);
    start_liquid_wave_animation();
    board_display_unlock();
    ESP_LOGI("dashboard", "Interface history: %u bytes in PSRAM; internal free: %u, PSRAM free: %u bytes",
             (unsigned)(OPENWRT_MAX_INTERFACES * sizeof(*s_interface_histories)),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}

static void format_rate(uint64_t bytes_per_second, char *output, size_t size)
{
    if (bytes_per_second >= 1024ULL * 1024ULL) {
        uint64_t whole = bytes_per_second / (1024ULL * 1024ULL);
        uint64_t fraction = ((bytes_per_second % (1024ULL * 1024ULL)) * 100) /
                            (1024ULL * 1024ULL);
        snprintf(output, size, "%llu.%02llu MB/s", (unsigned long long)whole,
                 (unsigned long long)fraction);
    } else if (bytes_per_second >= 1024ULL) {
        uint64_t whole = bytes_per_second / 1024ULL;
        uint64_t fraction = ((bytes_per_second % 1024ULL) * 10) / 1024ULL;
        snprintf(output, size, "%llu.%llu KB/s", (unsigned long long)whole,
                 (unsigned long long)fraction);
    } else {
        snprintf(output, size, "%llu B/s", (unsigned long long)bytes_per_second);
    }
}

static void format_scaled_mbps(uint64_t hundredths, char *output, size_t size)
{
    snprintf(output, size, "%llu.%02llu",
             (unsigned long long)(hundredths / 100ULL),
             (unsigned long long)(hundredths % 100ULL));
}

static void update_traffic_speed_labels(
    int history_slot, const openwrt_interface_status_t *interface)
{
    if (history_slot < 0 || history_slot >= OPENWRT_MAX_INTERFACES) return;
    traffic_history_t *history = &s_interface_histories[history_slot];
    uint64_t sum_down = 0, sum_up = 0;
    uint32_t max_down = 0, max_up = 0;
    for (uint32_t i = 0; i < history->count; ++i) {
        sum_down += history->download[i];
        sum_up += history->upload[i];
        if (history->download[i] > max_down) max_down = history->download[i];
        if (history->upload[i] > max_up) max_up = history->upload[i];
    }
    uint64_t avg_down = history->count ? sum_down / history->count : 0;
    uint64_t avg_up = history->count ? sum_up / history->count : 0;
    uint64_t current_down = interface ?
                                (interface->download_bps + CHART_SCALE_DIVISOR / 2) /
                                    CHART_SCALE_DIVISOR : 0;
    uint64_t current_up = interface ?
                              (interface->upload_bps + CHART_SCALE_DIVISOR / 2) /
                                  CHART_SCALE_DIVISOR : 0;
    char d_now[20], u_now[20], d_avg[20], u_avg[20], d_max[20], u_max[20];
    char line[128];
    format_scaled_mbps(current_down, d_now, sizeof(d_now));
    format_scaled_mbps(current_up, u_now, sizeof(u_now));
    format_scaled_mbps(avg_down, d_avg, sizeof(d_avg));
    format_scaled_mbps(avg_up, u_avg, sizeof(u_avg));
    format_scaled_mbps(max_down, d_max, sizeof(d_max));
    format_scaled_mbps(max_up, u_max, sizeof(u_max));
    snprintf(line, sizeof(line), "D %s Mbps", d_now);
    lv_label_set_text(s_traffic_alert_rate, line);
    snprintf(line, sizeof(line), "U %s Mbps", u_now);
    lv_label_set_text(s_traffic_alert_upload_rate, line);
    snprintf(line, sizeof(line), "AVG D/U %s/%s  MAX %s/%s",
             d_avg, u_avg, d_max, u_max);
    lv_label_set_text(s_traffic_alert_stats, line);
}

static void format_total(uint64_t bytes, char *output, size_t size)
{
    const char *unit;
    uint64_t divisor;
    if (bytes >= 1024ULL * 1024ULL * 1024ULL * 1024ULL) {
        divisor = 1024ULL * 1024ULL * 1024ULL * 1024ULL; unit = "TB";
    } else if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        divisor = 1024ULL * 1024ULL * 1024ULL; unit = "GB";
    } else if (bytes >= 1024ULL * 1024ULL) {
        divisor = 1024ULL * 1024ULL; unit = "MB";
    } else if (bytes >= 1024ULL) {
        divisor = 1024ULL; unit = "KB";
    } else {
        snprintf(output, size, "TOTAL %llu B", (unsigned long long)bytes);
        return;
    }
    uint64_t whole = bytes / divisor;
    uint64_t fraction = ((bytes % divisor) * 100) / divisor;
    snprintf(output, size, "TOTAL %llu.%02llu %s", (unsigned long long)whole,
             (unsigned long long)fraction, unit);
}

static void format_tenths(float value, const char *suffix, char *output, size_t size)
{
    int scaled = value >= 0 ? (int)(value * 10.0f + 0.5f) : (int)(value * 10.0f - 0.5f);
    int absolute = scaled < 0 ? -scaled : scaled;
    snprintf(output, size, "%s%d.%d%s", scaled < 0 ? "-" : "",
             absolute / 10, absolute % 10, suffix);
}

static void format_uptime(uint64_t seconds, char *output, size_t size)
{
    uint64_t days = seconds / 86400;
    uint64_t hours = (seconds % 86400) / 3600;
    if (days) snprintf(output, size, "%llud %lluh", (unsigned long long)days,
                       (unsigned long long)hours);
    else snprintf(output, size, "%02llu:%02llu", (unsigned long long)hours,
                  (unsigned long long)((seconds % 3600) / 60));
}

static uint32_t chart_value(uint64_t bytes_per_second)
{
    uint64_t value = bytes_per_second / CHART_SCALE_DIVISOR;
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

static int32_t rate_level(uint64_t bytes_per_second, uint64_t full_scale_bps)
{
    if (bytes_per_second >= full_scale_bps) return 100;
    return (int32_t)((bytes_per_second * 100ULL + full_scale_bps / 2ULL) /
                     full_scale_bps);
}

static const openwrt_interface_status_t *default_page_interface(
    const openwrt_status_t *status)
{
    if (status->interface_count == 0) return NULL;
    if (s_default_interface_index >= status->interface_count) {
        s_default_interface_index = 0;
        s_default_interface_switch_us = esp_timer_get_time();
    }
    int64_t now = esp_timer_get_time();
    if (s_default_interface_switch_us == 0) s_default_interface_switch_us = now;
    if (now - s_default_interface_switch_us >= DEFAULT_INTERFACE_ROTATION_US) {
        s_default_interface_index = (s_default_interface_index + 1) % status->interface_count;
        s_default_interface_switch_us = now;
    }
    return &status->interfaces[s_default_interface_index];
}

static int find_history_slot(const char *name, bool create)
{
    int free_slot = -1;
    for (int i = 0; i < OPENWRT_MAX_INTERFACES; ++i) {
        if (s_interface_histories[i].name[0] &&
            strcmp(s_interface_histories[i].name, name) == 0) return i;
        if (free_slot < 0 && !s_interface_histories[i].name[0]) free_slot = i;
    }
    if (!create || free_slot < 0) return -1;
    strlcpy(s_interface_histories[free_slot].name, name,
            sizeof(s_interface_histories[free_slot].name));
    return free_slot;
}

static const openwrt_interface_status_t *status_interface_for_slot(
    const openwrt_status_t *status, int slot)
{
    if (slot < 0 || slot >= OPENWRT_MAX_INTERFACES) return NULL;
    for (uint8_t i = 0; i < status->interface_count; ++i) {
        if (strcmp(status->interfaces[i].name, s_interface_histories[slot].name) == 0) {
            return &status->interfaces[i];
        }
    }
    return NULL;
}

static bool append_traffic_samples(const openwrt_status_t *status)
{
    if (!status->sample_id || status->sample_id == s_last_sample_id) return false;
    s_last_sample_id = status->sample_id;
    for (int i = 0; i < OPENWRT_MAX_INTERFACES; ++i) {
        s_interface_histories[i].active = false;
    }
    s_wan_history_slot = -1;
    for (uint8_t i = 0; i < status->interface_count; ++i) {
        const openwrt_interface_status_t *interface = &status->interfaces[i];
        int slot = find_history_slot(interface->name, true);
        if (slot < 0) continue;
        traffic_history_t *history = &s_interface_histories[slot];
        history->active = true;
        history->download[history->index] = chart_value(interface->download_bps);
        history->upload[history->index] = chart_value(interface->upload_bps);
        history->index = (history->index + 1) % TRAFFIC_HISTORY_SECONDS;
        if (history->count < TRAFFIC_HISTORY_SECONDS) history->count++;
        if ((int)i == status->wan_interface_index) s_wan_history_slot = slot;
    }
    if (s_manual_history_slot >= 0 &&
        !s_interface_histories[s_manual_history_slot].active) {
        s_manual_history_slot = -1;
    }
    return true;
}

void status_dashboard_update(void)
{
    static openwrt_status_t status;
    memset(&status, 0, sizeof(status));
    openwrt_status_get(&status);
    char text[64];

    board_display_lock();
    if (!status.valid) {
        lv_anim_del(&s_redraw_animation, NULL);
        if (s_manual_history_slot >= 0) {
            lv_label_set_text(s_traffic_alert_title,
                              s_interface_histories[s_manual_history_slot].name);
            lv_label_set_text(s_traffic_alert_rate,
                              status.message[0] ? status.message : "NO DATA");
            lv_label_set_text(s_traffic_alert_upload_rate, "");
            lv_label_set_text(s_traffic_alert_stats, "AVG D/U --/--  MAX --/--");
            lv_obj_clear_flag(s_traffic_alert, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_traffic_alert);
        } else {
            s_was_high_traffic = false;
            lv_obj_add_flag(s_traffic_alert, LV_OBJ_FLAG_HIDDEN);
        }
        lv_label_set_text(s_connection, status.message[0] ? status.message : "OFFLINE");
        lv_obj_set_style_text_color(s_connection, lv_color_hex(0xFFB84D), 0);
        board_display_unlock();
        return;
    }

    const openwrt_interface_status_t *default_interface = default_page_interface(&status);
    lv_label_set_text(s_connection, default_interface ? default_interface->name : "NO IFACE");
    lv_label_set_text(s_title, status.hostname[0] ? status.hostname : "OpenWrt");
    lv_obj_set_style_text_color(s_connection, lv_color_hex(0x44D7B6), 0);
    format_tenths(status.cpu_percent, "%", text, sizeof(text));
    lv_label_set_text(s_cpu, text);
    set_liquid_level(&s_cpu_liquid, (int32_t)(status.cpu_percent + 0.5f));
    format_tenths(status.memory_percent, "%", text, sizeof(text));
    lv_label_set_text(s_memory, text);
    set_liquid_level(&s_memory_liquid, (int32_t)(status.memory_percent + 0.5f));
    format_tenths(status.temperature_c, " C", text, sizeof(text));
    lv_label_set_text(s_temperature, text);
    set_liquid_level(&s_temperature_liquid, (int32_t)(status.temperature_c + 0.5f));
    format_uptime(status.uptime_seconds, text, sizeof(text));
    lv_label_set_text(s_uptime, text);
    uint64_t default_download_bps = default_interface ? default_interface->download_bps : 0;
    uint64_t default_upload_bps = default_interface ? default_interface->upload_bps : 0;
    uint64_t default_download_bytes = default_interface ? default_interface->download_bytes : 0;
    uint64_t default_upload_bytes = default_interface ? default_interface->upload_bytes : 0;
    format_rate(default_download_bps, text, sizeof(text));
    lv_label_set_text(s_download_speed, text);
    set_liquid_level(&s_download_liquid,
                     rate_level(default_download_bps, DOWNLOAD_FULL_SCALE_BPS));
    format_total(default_download_bytes, text, sizeof(text));
    lv_label_set_text(s_download_total, text);
    format_rate(default_upload_bps, text, sizeof(text));
    lv_label_set_text(s_upload_speed, text);
    set_liquid_level(&s_upload_liquid,
                     rate_level(default_upload_bps, UPLOAD_FULL_SCALE_BPS));
    format_total(default_upload_bytes, text, sizeof(text));
    lv_label_set_text(s_upload_total, text);

    /* Keep a continuous one-sample-per-second history.  The traffic threshold
     * only controls whether the waveform page is visible; it no longer
     * controls collection, so a spike can show the preceding normal traffic. */
    bool new_traffic_sample = append_traffic_samples(&status);
    bool high_download = FULLSCREEN_TRAFFIC_ENABLED &&
                         s_wan_history_slot >= 0 &&
                         status.wan_download_bps >= HIGH_TRAFFIC_THRESHOLD_BPS;
    bool high_upload = FULLSCREEN_TRAFFIC_ENABLED &&
                       s_wan_history_slot >= 0 &&
                       status.wan_upload_bps >= HIGH_TRAFFIC_THRESHOLD_BPS;
    int display_slot = s_manual_history_slot >= 0 ? s_manual_history_slot :
                       ((high_download || high_upload) ? s_wan_history_slot : -1);
    if (display_slot >= 0) {
        if (new_traffic_sample || !s_was_high_traffic ||
            s_visible_history_slot != display_slot) {
            start_waveform_redraw(display_slot);
        }
        s_visible_history_slot = display_slot;
        s_was_high_traffic = true;
        const openwrt_interface_status_t *shown =
            status_interface_for_slot(&status, display_slot);
        if (s_manual_history_slot >= 0) {
            lv_label_set_text(s_traffic_alert_title,
                              s_interface_histories[display_slot].name);
        } else if (high_download && high_upload) {
            lv_label_set_text(s_traffic_alert_title, "WAN HIGH DOWN + UP");
        } else if (high_download) {
            lv_label_set_text(s_traffic_alert_title, "WAN HIGH DOWNLOAD");
        } else {
            lv_label_set_text(s_traffic_alert_title, "WAN HIGH UPLOAD");
        }
        update_traffic_speed_labels(display_slot, shown);
        lv_obj_clear_flag(s_traffic_alert, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_traffic_alert);
    } else {
        if (s_was_high_traffic) lv_anim_del(&s_redraw_animation, NULL);
        s_was_high_traffic = false;
        s_visible_history_slot = -1;
        lv_obj_add_flag(s_traffic_alert, LV_OBJ_FLAG_HIDDEN);
    }
    board_display_unlock();
}
