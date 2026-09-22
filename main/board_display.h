#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t board_display_init(void);
void board_display_lock(void);
void board_display_unlock(void);
void board_display_handle(void);
void board_display_set_brightness(uint8_t percent);

typedef struct {
    uint32_t refreshes;
    uint32_t flushes;
    uint32_t dma_completed;
    uint32_t pixels;
} board_display_perf_t;

void board_display_take_perf(board_display_perf_t *perf);
