#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "openwrt_status.h"

esp_err_t test_gif_configure(const openwrt_status_t *status);
bool test_gif_is_ready(void);

/* Caller must hold the display lock. */
esp_err_t test_gif_show(void);
/* Caller must hold the display lock. */
void test_gif_hide(void);
bool test_gif_is_visible(void);
