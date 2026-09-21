#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t provisioning_portal_start(void);
void provisioning_portal_stop(void);
bool provisioning_portal_is_running(void);
