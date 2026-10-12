/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 piaozhiye <piaozhiye@gmail.com>
 */
/*
 * mini_drm.h - minimal DRM/KMS double-buffer backend (raw ioctls, no
 * libdrm userspace dependency; only the kernel uapi headers are used).
 *
 * Two dumb buffers are page-flipped with a blocking DRM_IOCTL_MODE_PAGE_FLIP
 * (waits for vblank), which guarantees the scanout never sees a mid-write
 * frame - the cure for the garbage bands the single-buffered fbdev path
 * showed on this panel.
 */

#ifndef MINI_DRM_H
#define MINI_DRM_H

#include <stdint.h>

typedef struct {
    int fd;
    uint32_t conn_id, enc_id, crtc_id;
    uint32_t width, height, pitch;
    struct {
        uint32_t fb_id, handle;
        uint32_t *map;
        uint64_t size;
    } bo[2];
    int front, back;
} mini_drm_t;

/* open card0, take master, find the connected CRTC and its mode, create
 * and map two dumb framebuffers, show bo[0]. Returns 0 on success. */
int mini_drm_init(mini_drm_t *d);

/* pixel buffer of the back buffer (width x height, 32bpp XRGB) */
uint32_t *mini_drm_back_buffer(const mini_drm_t *d);

/* flip the back buffer to the screen (blocks until vblank), then swaps */
int mini_drm_flip(mini_drm_t *d);

/* dump the current back buffer to a file (debug aid, SIGUSR1 hook) */
int mini_drm_dump(const mini_drm_t *d, const char *path);

#endif
