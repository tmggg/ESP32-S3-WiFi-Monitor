#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t status_dashboard_init(void);
void status_dashboard_update(void);
void status_dashboard_request_page_toggle(void);
bool status_dashboard_process_ui_requests(void);
void status_dashboard_animate_frame(void);

typedef struct {
    bool traffic_page_visible;
    uint32_t liquid_updates[5]; /* CPU, MEM, TEMP, DOWNLOAD, UPLOAD */
} status_dashboard_perf_t;

void status_dashboard_take_perf(status_dashboard_perf_t *perf);
