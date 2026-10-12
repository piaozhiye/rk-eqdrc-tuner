/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 piaozhiye <piaozhiye@gmail.com>
 */
#ifndef LV_SYSTICK_H
#define LV_SYSTICK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <time.h>

/* matches the Rockchip lv_conf.h LV_TICK_CUSTOM hook */
static inline uint32_t lv_systick(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u +
                      (uint64_t)ts.tv_nsec / 1000000u);
}

#ifdef __cplusplus
}
#endif

#endif
