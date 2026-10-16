#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "openwrt_status.h"

#define TRAFFIC_HISTORY_SECONDS 120
#define TRAFFIC_HISTORY_CAPACITY (TRAFFIC_HISTORY_SECONDS + 1)
#define TRAFFIC_SECOND_US 1000000LL

typedef struct {
    uint64_t download;
    uint64_t upload;
    int64_t started_us;
    int64_t received_us;
} traffic_sample_t;

typedef struct {
    bool active;
    char name[OPENWRT_INTERFACE_NAME_LEN];
    uint64_t ifindex;
    int64_t last_seen_us;
    traffic_sample_t samples[TRAFFIC_HISTORY_CAPACITY];
} traffic_history_t;

typedef struct {
    uint64_t average;
    uint64_t maximum;
    bool valid;
} traffic_stats_t;

/* Fixed second buckets bound storage even if an endpoint responds very quickly.
 * Timestamps, rather than sample count, determine expiry and horizontal position. */
static inline void traffic_history_append(traffic_history_t *history,
                                         const openwrt_status_t *status,
                                         const openwrt_interface_status_t *interface)
{
    int64_t end_us = status->protocol_v2 ? status->sample_local_us : status->received_us;
    if (end_us <= 0) return;
    int64_t start_us = status->request_started_us;
    if (status->protocol_v2) {
        uint64_t interval = interface->interval_ms;
        if (interval > TRAFFIC_HISTORY_SECONDS * 1000ULL)
            interval = TRAFFIC_HISTORY_SECONDS * 1000ULL;
        start_us = end_us - (int64_t)interval * 1000;
    }
    uint32_t bucket = (uint64_t)(end_us / TRAFFIC_SECOND_US) %
                      TRAFFIC_HISTORY_CAPACITY;
    history->samples[bucket] = (traffic_sample_t) {
        .download = interface->download_bps,
        .upload = interface->upload_bps,
        .started_us = start_us,
        .received_us = end_us,
    };
    history->last_seen_us = status->received_us;
}

static inline bool traffic_sample_in_window(const traffic_sample_t *sample, int64_t now_us)
{
    return sample->received_us > 0 && sample->received_us <= now_us &&
           now_us - sample->received_us <= TRAFFIC_HISTORY_SECONDS * TRAFFIC_SECOND_US;
}

/* A reported rate describes (started_us, received_us], not just its endpoint.
 * Include the endpoint bucket even for sub-second intervals. Other positions
 * must fall inside measured coverage: missing intervals are never interpolated. */
static inline int64_t traffic_sample_first_second(const traffic_sample_t *sample)
{
    int64_t first = sample->started_us < 0 ? 0 :
                    sample->started_us / TRAFFIC_SECOND_US + 1;
    int64_t last = sample->received_us / TRAFFIC_SECOND_US;
    return first < last ? first : last;
}

static inline traffic_stats_t traffic_history_stats(const traffic_history_t *history,
                                                     int64_t now_us, bool upload)
{
    traffic_stats_t stats = {0};
    double weighted_sum = 0;
    uint64_t duration_us = 0;
    int64_t cutoff = now_us - TRAFFIC_HISTORY_SECONDS * TRAFFIC_SECOND_US;
    for (uint32_t i = 0; i < TRAFFIC_HISTORY_CAPACITY; ++i) {
        const traffic_sample_t *sample = &history->samples[i];
        uint64_t rate = upload ? sample->upload : sample->download;
        if (!traffic_sample_in_window(sample, now_us) ||
            rate == OPENWRT_TRAFFIC_UNAVAILABLE) continue;
        if (rate > stats.maximum) stats.maximum = rate;
        int64_t start = sample->started_us > cutoff ? sample->started_us : cutoff;
        if (start < sample->received_us) {
            uint64_t weight = (uint64_t)(sample->received_us - start);
            weighted_sum += (double)rate * (double)weight;
            duration_us += weight;
        }
        stats.valid = true;
    }
    /* v2 uses the actual per-interface server interval; legacy endpoints use
     * observed HTTP windows. Missing/invalid fields contribute neither zero nor time. */
    stats.average = duration_us ? (uint64_t)(weighted_sum / (double)duration_us + 0.5) :
                                 stats.maximum;
    if (stats.average > stats.maximum) stats.average = stats.maximum;
    return stats;
}

/* Preserve the true peak; only the chart's coordinates are scaled. */
static inline uint64_t traffic_chart_scale(uint64_t peak)
{
    const uint64_t minimum = 625000ULL; /* 5 Mbps */
    if (peak < minimum) peak = minimum;
    return peak + peak / 5;
}

static inline int32_t traffic_chart_coordinate(uint64_t rate, uint64_t scale)
{
    if (rate == OPENWRT_TRAFFIC_UNAVAILABLE) return -1;
    return (int32_t)((double)rate * 30000.0 / (double)scale + 0.5);
}
