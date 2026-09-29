#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "openwrt_status.h"

esp_err_t clock_screensaver_init(void);
void clock_screensaver_update(const openwrt_status_t *status);

/* The caller must hold the display lock. */
void clock_screensaver_show(void);
/* The caller must hold the display lock. */
void clock_screensaver_hide(void);
bool clock_screensaver_is_visible(void);
