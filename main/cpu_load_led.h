#pragma once

#include "esp_err.h"

esp_err_t cpu_load_led_start(void);
void cpu_load_led_set_openwrt_load(float load_percent);
void cpu_load_led_set_unavailable(void);

