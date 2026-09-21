#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t status_dashboard_init(void);
void status_dashboard_update(void);
void status_dashboard_request_page_toggle(void);
bool status_dashboard_process_ui_requests(void);
