#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define OPENWRT_MAX_INTERFACES 16
#define OPENWRT_INTERFACE_NAME_LEN 24
/* Reserved sentinel: invalid or absent traffic fields are never real zeroes. */
#define OPENWRT_TRAFFIC_UNAVAILABLE UINT64_MAX
#define OPENWRT_TRAFFIC_MAX_JSON_INTEGER 9007199254740991ULL
#define OPENWRT_STALE_AFTER_MS 3000ULL
#define OPENWRT_IDENTITY_LEN 65

typedef struct {
    char name[OPENWRT_INTERFACE_NAME_LEN];
    uint64_t ifindex;
    uint64_t interval_ms;
    bool download_bytes_valid;
    bool upload_bytes_valid;
    uint64_t download_bps;
    uint64_t upload_bps;
    uint64_t download_bytes;
    uint64_t upload_bytes;
} openwrt_interface_status_t;

typedef struct {
    bool valid;
    uint64_t sample_id;
    uint64_t history_generation;
    bool protocol_v2;
    bool stale;
    char boot_id[OPENWRT_IDENTITY_LEN];
    char collector_id[OPENWRT_IDENTITY_LEN];
    uint64_t sample_seq;
    uint64_t sample_end_ms;
    uint64_t interval_ms;
    uint64_t age_ms;
    int64_t sample_local_us;
    int64_t request_started_us;
    int64_t received_us;
    float cpu_percent;
    float memory_percent;
    float temperature_c;
    uint64_t uptime_seconds;
    uint64_t download_bps;
    uint64_t upload_bps;
    uint64_t wan_download_bps;
    uint64_t wan_upload_bps;
    uint64_t download_bytes;
    uint64_t upload_bytes;
    char hostname[64];
    char system_date[16];
    char system_time[16];
    uint32_t screensaver_timeout;
    char screensaver_type[16];
    char gif_url[256];
    char gif_version[65];
    uint32_t gif_size;
    char wan_device[24];
    char message[48];
    uint8_t interface_count;
    int8_t wan_interface_index;
    openwrt_interface_status_t interfaces[OPENWRT_MAX_INTERFACES];
} openwrt_status_t;

esp_err_t openwrt_status_start(void);
void openwrt_status_get(openwrt_status_t *status);
