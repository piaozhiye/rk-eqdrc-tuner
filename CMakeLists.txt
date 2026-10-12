# SPDX-License-Identifier: MIT
# Copyright (c) 2026 piaozhiye <piaozhiye@gmail.com>
cmake_minimum_required(VERSION 3.10)
project(eqdrc_tuner LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -O2 -DUSE_EVDEV=1 -DLV_DRV_NO_CONF")

set(APP_DIR ${CMAKE_CURRENT_SOURCE_DIR})

find_library(LVGL_LIB lvgl PATHS ${APP_DIR}/lib/aarch64 ${APP_DIR}/lib NO_DEFAULT_PATH REQUIRED)
find_library(EVDEV_LIB evdev REQUIRED)
find_library(FREETYPE_LIB freetype REQUIRED)

add_executable(eqdrc_tuner
    ui_main.c
    eqdrc_para.c
    evdev.c
    key.c
    lv_port_disp.c
    lv_port_indev.c
    lv_port_init.c
    mini_drm.c
    sys/cpu.c
    sys/soc.c
    sys/timestamp.c
    sys/trace.c
    common/kalman_filter.c
    common/ui_scaler.c
)

target_include_directories(eqdrc_tuner PRIVATE
    ${APP_DIR}
    ${APP_DIR}/sys
    ${APP_DIR}/common
    ${APP_DIR}/include
    ${APP_DIR}/include/lvgl
    ${APP_DIR}/include/lvgl/lv_drivers
    ${APP_DIR}/include/libdrm

)

target_link_libraries(eqdrc_tuner
    ${LVGL_LIB}
    ${EVDEV_LIB}
    ${FREETYPE_LIB}
    pthread m rt dl
)
