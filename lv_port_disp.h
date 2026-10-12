/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 piaozhiye <piaozhiye@gmail.com>
 */
/**
 * @file lv_port_disp.h
 *
 */

#ifndef LV_PORT_DISP_H
#define LV_PORT_DISP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"

void lv_port_disp_init(lv_coord_t hor_res, lv_coord_t ver_res, int rot);
void lv_port_disp_dump(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
