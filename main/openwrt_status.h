#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define OPENWRT_MAX_INTERFACES 16
#define OPENWRT_INTERFACE_NAME_LEN 24

typedef struct {
    char name[OPENWRT_INTERFACE_NAME_LEN];
    uint64_t download_bps;
    uint64_t upload_bps;
    uint64_t download_bytes;
    uint64_t upload_bytes;
} openwrt_interface_status_t;

typedef struct {
    bool valid;
    uint32_t sample_id;
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
    char wan_device[24];
    char message[48];
    uint8_t interface_count;
    int8_t wan_interface_index;
    openwrt_interface_status_t interfaces[OPENWRT_MAX_INTERFACES];
} openwrt_status_t;

esp_err_t openwrt_status_start(void);
void openwrt_status_get(openwrt_status_t *status);
