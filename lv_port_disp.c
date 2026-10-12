/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 piaozhiye <piaozhiye@gmail.com>
 */
#include <errno.h>
#include <stdlib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <lvgl/lvgl.h>

#include "mini_drm.h"

/*
 * Display backends sharing the same LVGL-facing logic:
 *
 *  - DRM double buffer (preferred). rot=0 copies the flushed area straight
 *    into the back dumb buffer (native-portrait panels); rot=90/270 writes
 *    it through the 8x8 tile transposer (landscape-driven portrait glass).
 *    A blocking page flip on vblank presents complete frames only.
 *
 *  - fbdev single buffer (fallback): same two paths, may tear.
 */

static mini_drm_t drm;
static int use_drm;

/* ---- fbdev fallback state ---- */
static int fb_fd = -1;
static char *fb_mem = NULL;
static struct fb_var_screeninfo fb_vinfo;
static struct fb_fix_screeninfo fb_finfo;
static int g_rotation = 0;

static void tile_transpose_write(uint32_t *dst, int dst_stride_px,
                                 const lv_area_t *area, lv_color_t *color_p,
                                 int h)
{
    /*
     * Rotated blit as 8x8 tiles: destination rows stay 32-byte contiguous
     * inside a tile (cache friendly). The source is the area's row-major
     * compact buffer, so each tile must index it at (y-y1)*aw + (x-x1).
     */
    int aw = area->x2 - area->x1 + 1;

    for (int ty = area->y1; ty <= area->y2; ty += 8) {
        int ymax = ty + 7 < area->y2 ? ty + 7 : area->y2;
        for (int tx = area->x1; tx <= area->x2; tx += 8) {
            int xmax = tx + 7 < area->x2 ? tx + 7 : area->x2;
            const uint32_t *s = (const uint32_t *)color_p +
                                (long)(ty - area->y1) * aw + (tx - area->x1);
            int nx = xmax - tx + 1, ny = ymax - ty + 1;
            uint32_t tmp[8][8];

            for (int j = 0; j < ny; j++)
                for (int i = 0; i < nx; i++)
                    tmp[i][j] = s[(long)j * aw + i];
            for (int i = 0; i < nx; i++) {
                uint32_t *row = dst + (long)(tx + i) * dst_stride_px;
                int dcol = h - 1 - ty;
                for (int j = 0; j < ny; j++)
                    row[dcol - j] = tmp[i][j];
            }
        }
    }
}

static void area_direct_write(uint32_t *dst, int dst_stride_px,
                              const lv_area_t *area, lv_color_t *color_p)
{
    int w = area->x2 - area->x1 + 1;

    for (int y = area->y1; y <= area->y2; y++)
        memcpy(dst + (long)y * dst_stride_px + area->x1,
               color_p + (long)(y - area->y1) * w, w * sizeof(uint32_t));
}

static void drm_flush(lv_disp_drv_t *drv, const lv_area_t *area,
                      lv_color_t *color_p)
{
    uint32_t *back = mini_drm_back_buffer(&drm);

    if (g_rotation == 0)
        area_direct_write(back, drm.pitch / 4, area, color_p);
    else
        tile_transpose_write(back, drm.pitch / 4, area, color_p, drv->ver_res);
    mini_drm_flip(&drm);
    lv_disp_flush_ready(drv);
}

static void fb_flush(lv_disp_drv_t *drv, const lv_area_t *area,
                     lv_color_t *color_p)
{
    int arg = 0;

    ioctl(fb_fd, FBIO_WAITFORVSYNC, &arg);
    if (g_rotation == 0)
        area_direct_write((uint32_t *)fb_mem, fb_finfo.line_length / 4,
                          area, color_p);
    else
        tile_transpose_write((uint32_t *)fb_mem, fb_finfo.line_length / 4,
                             area, color_p, drv->ver_res);
    lv_disp_flush_ready(drv);
}

void lv_port_disp_dump(void)
{
    if (use_drm)
        mini_drm_dump(&drm, "/tmp/drm_dump.raw");
    else if (fb_mem)
        printf("lv_port_disp: fbdev mode, dump unsupported\n");
}

void lv_port_disp_init(lv_coord_t hor_res, lv_coord_t ver_res, int rot)
{
    (void)hor_res;
    (void)ver_res;
    g_rotation = rot;

    lv_coord_t w, h;

    if (mini_drm_init(&drm) == 0) {
        use_drm = 1;
        w = drm.width;
        h = drm.height;
        printf("lv_port_disp: DRM %dx%d\n", w, h);
    } else {
        fb_fd = open("/dev/fb0", O_RDWR);
        if (fb_fd < 0) {
            fprintf(stderr, "lv_port_disp: no DRM display and no fb0\n");
            exit(1);
        }
        if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &fb_vinfo) < 0 ||
            ioctl(fb_fd, FBIOGET_FSCREENINFO, &fb_finfo) < 0) {
            perror("fb ioctls");
            exit(1);          /* no display registered -> NULL deref later */
        }
        fb_mem = mmap(NULL, fb_finfo.smem_len, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fb_fd, 0);
        if (fb_mem == MAP_FAILED) {
            perror("mmap");
            exit(1);
        }
        w = fb_vinfo.xres;
        h = fb_vinfo.yres;
        printf("lv_port_disp: fbdev %dx%d\n", w, h);
    }

    if (g_rotation == 90 || g_rotation == 270) {
        lv_coord_t tmp = w;

        w = h;
        h = tmp;
    }

    static lv_color_t *buf = NULL;
    buf = malloc(w * h * sizeof(lv_color_t));

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, w * h);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = w;
    disp_drv.ver_res = h;
    disp_drv.flush_cb = use_drm ? drm_flush : fb_flush;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.full_refresh = 1;
    lv_disp_drv_register(&disp_drv);

    printf("lv_port_disp: rot=%d -> LVGL %dx%d %dbpp\n", rot, w, h, 32);
}
