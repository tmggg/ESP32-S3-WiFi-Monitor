#pragma once

#include <stdbool.h>

void status_dashboard_init(void);
void status_dashboard_update(void);
void status_dashboard_request_page_toggle(void);
bool status_dashboard_process_ui_requests(void);
