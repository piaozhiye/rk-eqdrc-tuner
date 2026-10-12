/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 piaozhiye <piaozhiye@gmail.com>
 */
/*
 * lv_port_pc.c - PC (SDL) backend for eqdrc_tuner simulation.
 *
 * Same-facing API as the board port layer so ui_main.c builds unchanged
 * with -DEQDRC_PC_SIM: lv_port_init() brings up LVGL on an SDL window
 * with mouse-driven pointer input. The UI runs at the panel's native
 * 1080x1920 logical size; the window shows it half-scale (fits 1080p
 * desktops) and rescales on resize.
 *
 * Keys: ESC/Q quit, S dumps the framebuffer to eqdrc_tuner_dump.ppm.
 */

#include <lvgl/lvgl.h>
#include <lvgl/lv_conf.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <SDL.h>

#include "lv_port_init.h"

#define SW 1080
#define SH 1920

static SDL_Window *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture *s_tex;
static volatile int s_quit;
static volatile int s_dump;

static int s_mouse_x, s_mouse_y;
static int s_mouse_pr;

/* SIGUSR1 maps here on the board; on PC the S key dumps instead */
void lv_port_disp_dump(void)
{
    s_dump = 1;
}

static void pc_pump(void);

static void pump_timer_cb(lv_timer_t *t)
{
    (void)t;
    pc_pump();
    if (s_quit) {
        SDL_Quit();
        exit(0);
    }
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                     lv_color_t *color_p)
{
    void *pix;
    int pitch;

    (void)area;
    if (SDL_LockTexture(s_tex, NULL, &pix, &pitch) == 0) {
        const uint32_t *src = (const uint32_t *)color_p;

        if (pitch == drv->hor_res * 4) {
            memcpy(pix, src, (size_t)drv->hor_res * drv->ver_res * 4);
        } else {
            for (int y = 0; y < drv->ver_res; y++)
                memcpy((uint8_t *)pix + (size_t)y * pitch,
                       src + (size_t)y * drv->hor_res, drv->hor_res * 4);
        }
        SDL_UnlockTexture(s_tex);
    }
    lv_disp_flush_ready(drv);
}

static void mouse_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    data->point.x = s_mouse_x;
    data->point.y = s_mouse_y;
    data->state = s_mouse_pr ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}

void lv_port_init(void)
{
    lv_init();

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "lv_port_pc: SDL_Init: %s\n", SDL_GetError());
        return;
    }
    s_win = SDL_CreateWindow("eqdrc_tuner (PC sim)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SW / 2, SH / 2,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    s_ren = SDL_CreateRenderer(s_win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(s_ren, SW, SH);
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, SW, SH);

    static lv_color_t buf[SW * SH];
    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, SW * SH);

    static lv_disp_drv_t drv;
    lv_disp_drv_init(&drv);
    drv.hor_res = SW;
    drv.ver_res = SH;
    drv.flush_cb = flush_cb;
    drv.draw_buf = &draw_buf;
    drv.full_refresh = 1;
    lv_disp_drv_register(&drv);

    static lv_indev_drv_t indev;
    lv_indev_drv_init(&indev);
    indev.type = LV_INDEV_TYPE_POINTER;
    indev.read_cb = mouse_read_cb;
    lv_indev_drv_register(&indev);

    lv_timer_create(pump_timer_cb, 20, NULL);
}

static void pc_pump(void)
{
    SDL_Event ev;

    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            s_quit = 1;
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            if (ev.button.button == SDL_BUTTON_LEFT)
                s_mouse_pr = ev.button.state == SDL_PRESSED;
            break;
        case SDL_KEYDOWN:
            if (ev.key.keysym.sym == SDLK_ESCAPE ||
                ev.key.keysym.sym == SDLK_q)
                s_quit = 1;
            if (ev.key.keysym.sym == SDLK_s)
                s_dump = 1;
            break;
        default:
            break;
        }
    }

    /* map window coords -> logical 1080x1920 */
    {
        int ww = 0, wh = 0;

        SDL_GetWindowSize(s_win, &ww, &wh);
        if (ww > 0 && wh > 0) {
            int mx, my;

            SDL_GetMouseState(&mx, &my);
            s_mouse_x = mx * SW / ww;
            s_mouse_y = my * SH / wh;
        }
    }

    SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
    SDL_RenderPresent(s_ren);

    if (s_dump) {
        s_dump = 0;
        void *pix;
        int pitch;

        if (SDL_LockTexture(s_tex, NULL, &pix, &pitch) == 0) {
            FILE *f = fopen("eqdrc_tuner_dump.ppm", "wb");

            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", SW, SH);
                for (int y = 0; y < SH; y++) {
                    const uint8_t *row = (const uint8_t *)pix +
                                         (size_t)y * pitch;
                    for (int x = 0; x < SW; x++) {
                        uint8_t rgb[3] = { row[x * 4 + 2], row[x * 4 + 1],
                                           row[x * 4 + 0] };
                        fwrite(rgb, 1, 3, f);
                    }
                }
                fclose(f);
                printf("lv_port_pc: dumped eqdrc_tuner_dump.ppm\n");
            }
            SDL_UnlockTexture(s_tex);
        }
    }
}
