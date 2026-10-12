/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 piaozhiye <piaozhiye@gmail.com>
 */
/*
 * mini_drm.c - see mini_drm.h. Implemented with raw ioctls against the
 * kernel uapi headers in include/libdrm (drm.h / drm_mode.h), so nothing
 * needs to be linked against libdrm at build or run time.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "libdrm/drm.h"
#include "libdrm/drm_mode.h"

#include "mini_drm.h"

#define CARD "/dev/dri/card0"

static int drm_ioctl(int fd, unsigned long req, void *arg)
{
    int r;

    do {
        r = ioctl(fd, req, arg);
    } while (r < 0 && (errno == EINTR || errno == EAGAIN));
    return r;
}

static int pick_connector(int fd, struct drm_mode_card_res *res,
                          uint32_t *conn_id, uint32_t *enc_id,
                          struct drm_mode_modeinfo *mode)
{
    uint32_t *conn_ids = (uint32_t *)(uintptr_t)res->connector_id_ptr;

    for (int i = 0; i < res->count_connectors; i++) {
        struct drm_mode_get_connector conn;
        uint32_t encoders[8];

        memset(&conn, 0, sizeof(conn));
        conn.connector_id = conn_ids[i];
        conn.encoders_ptr = (uint64_t)(uintptr_t)encoders;
        conn.count_encoders = 8;
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
            continue;
        /* status: 1=connected 2=disconnected 3=unknown; DPI panels often
         * report unknown, so accept anything that has modes */
        printf("mini_drm: connector %u status %u modes %d\n",
               conn.connector_id, conn.connection, conn.count_modes);
        if (conn.count_modes < 1)
            continue;

        /* two-pass: the ioctl needs mode/prop arrays to read fully */
        uint32_t props[32];
        uint64_t prop_values[32];
        struct drm_mode_modeinfo modes[8];

        memset(&conn, 0, sizeof(conn));
        conn.connector_id = conn_ids[i];
        conn.encoders_ptr = (uint64_t)(uintptr_t)encoders;
        conn.count_encoders = 8;
        conn.props_ptr = (uint64_t)(uintptr_t)props;
        conn.prop_values_ptr = (uint64_t)(uintptr_t)prop_values;
        conn.count_props = 32;
        conn.modes_ptr = (uint64_t)(uintptr_t)modes;
        conn.count_modes = 8;
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0 ||
            conn.count_modes < 1)
            continue;

        struct drm_mode_modeinfo *best = &modes[0];

        for (int m = 0; m < conn.count_modes; m++)
            if (modes[m].type & DRM_MODE_TYPE_PREFERRED) {
                best = &modes[m];
                break;
            }
        *conn_id = conn.connector_id;
        *enc_id = conn.encoder_id;
        /*
         * After a compositor releases the connector, encoder_id reads 0;
         * walk the connector's supported encoders and take the first one
         * that can drive a crtc, otherwise the crtc fallback picks the
         * wrong pipe and SETCRTC fails with EINVAL.
         */
        if (!*enc_id) {
            for (int e = 0; e < (int)conn.count_encoders && e < 8; e++) {
                struct drm_mode_get_encoder enc;

                memset(&enc, 0, sizeof(enc));
                enc.encoder_id = encoders[e];
                if (drm_ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) == 0 &&
                    enc.possible_crtcs) {
                    *enc_id = encoders[e];
                    break;
                }
            }
        }
        *mode = *best;
        return 0;
    }
    return -1;
}

static int make_bo(int fd, mini_drm_t *d, int idx)
{
    struct drm_mode_create_dumb create;
    struct drm_mode_map_dumb map;
    struct drm_mode_fb_cmd fb;
    struct drm_mode_destroy_dumb destroy;

    memset(&create, 0, sizeof(create));
    create.width = d->width;
    create.height = d->height;
    create.bpp = 32;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
        perror("CREATE_DUMB");
        return -1;
    }
    d->bo[idx].handle = create.handle;
    d->bo[idx].size = create.size;
    d->pitch = create.pitch;

    memset(&map, 0, sizeof(map));
    map.handle = create.handle;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        perror("MAP_DUMB");
        return -1;
    }
    d->bo[idx].map = mmap(NULL, create.size, PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd, map.offset);
    if (d->bo[idx].map == MAP_FAILED) {
        perror("mmap dumb");
        return -1;
    }
    memset(d->bo[idx].map, 0, create.size);

    memset(&fb, 0, sizeof(fb));
    fb.width = d->width;
    fb.height = d->height;
    fb.pitch = create.pitch;
    fb.depth = 24;
    fb.bpp = 32;
    fb.handle = create.handle;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0) {
        perror("ADDFB");
        return -1;
    }
    d->bo[idx].fb_id = fb.fb_id;
    (void)destroy;
    return 0;
}

int mini_drm_init(mini_drm_t *d)
{
    int fd = open(CARD, O_RDWR | O_CLOEXEC);

    setvbuf(stdout, NULL, _IONBF, 0);   /* debug prints reach the log file */
    memset(d, 0, sizeof(*d));
    if (fd < 0) {
        perror("open " CARD);
        return -1;
    }
    d->fd = fd;
    if (drm_ioctl(fd, DRM_IOCTL_SET_MASTER, 0) < 0)
        perror("SET_MASTER (continuing anyway)");

    struct drm_mode_card_res res;
    uint32_t conn[8], crtc[8], enc[8];

    memset(&res, 0, sizeof(res));
    res.connector_id_ptr = (uint64_t)(uintptr_t)conn;
    res.count_connectors = 8;
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtc;
    res.count_crtcs = 8;
    res.encoder_id_ptr = (uint64_t)(uintptr_t)enc;
    res.count_encoders = 8;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        perror("GETRESOURCES");
        close(fd);
        return -1;
    }
    printf("mini_drm: res crtcs %d connectors %d encoders %d fbs %d\n",
           res.count_crtcs, res.count_connectors, res.count_encoders,
           res.count_fbs);

    struct drm_mode_modeinfo mode;

    if (pick_connector(fd, &res, &d->conn_id, &d->enc_id, &mode) < 0) {
        fprintf(stderr, "mini_drm: no connected display\n");
        close(fd);
        return -1;
    }
    d->width = mode.hdisplay;
    d->height = mode.vdisplay;

    struct drm_mode_get_encoder get_enc;
    uint32_t crtc_id = 0;

    memset(&get_enc, 0, sizeof(get_enc));
    get_enc.encoder_id = d->enc_id;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &get_enc) < 0)
        memset(&get_enc, 0, sizeof(get_enc));
    if (get_enc.crtc_id) {
        crtc_id = get_enc.crtc_id;
    } else if (get_enc.encoder_id && get_enc.possible_crtcs) {
        /* encoder not bound (compositor released it): pick the first
         * crtc this encoder can drive, not blindly crtc[0] */
        for (int i = 0; i < 32 && i < (int)res.count_crtcs; i++)
            if (get_enc.possible_crtcs & (1u << i)) {
                crtc_id = crtc[i];
                break;
            }
    }
    if (!crtc_id)
        crtc_id = crtc[0];
    d->crtc_id = crtc_id;

    if (make_bo(fd, d, 0) < 0 || make_bo(fd, d, 1) < 0) {
        close(fd);
        return -1;
    }

    struct drm_mode_crtc set;

    memset(&set, 0, sizeof(set));
    set.crtc_id = d->crtc_id;
    set.fb_id = d->bo[0].fb_id;
    set.set_connectors_ptr = (uint64_t)(uintptr_t)&d->conn_id;
    set.count_connectors = 1;
    set.mode_valid = 1;
    set.mode = mode;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set) < 0) {
        perror("SETCRTC");
        close(fd);
        return -1;
    }

    printf("mini_drm: %ux%u conn %u crtc %u fb[%u,%u] pitch %u\n",
           d->width, d->height, d->conn_id, d->crtc_id,
           d->bo[0].fb_id, d->bo[1].fb_id, d->pitch);
    d->front = 0;
    d->back = 1;
    return 0;
}

uint32_t *mini_drm_back_buffer(const mini_drm_t *d)
{
    return d->bo[d->back].map;
}

int mini_drm_dump(const mini_drm_t *d, const char *path)
{
    FILE *f = fopen(path, "w");

    if (!f)
        return -1;
    size_t n = fwrite(d->bo[d->back].map, 1, d->bo[d->back].size, f);
    fclose(f);
    printf("mini_drm: dumped %zu bytes to %s\n", n, path);
    return n > 0 ? 0 : -1;
}

int mini_drm_flip(mini_drm_t *d)
{
    struct drm_mode_crtc_page_flip flip;
    int r;

    memset(&flip, 0, sizeof(flip));
    flip.crtc_id = d->crtc_id;
    flip.fb_id = d->bo[d->back].fb_id;
    flip.flags = 0;
    /*
     * This driver completes flips asynchronously at vblank; a new flip
     * while one is pending returns EBUSY. Retry until the previous flip
     * lands instead of dropping the frame (a dropped flip froze the
     * panel on a stale buffer and looked like corruption).
     */
    for (int i = 0; i < 40; i++) {
        r = ioctl(d->fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip);
        if (r == 0) {
            d->front = d->back;
            d->back ^= 1;
            return 0;
        }
        if (errno != EBUSY) {
            perror("PAGE_FLIP");
            return -1;
        }
        usleep(500);
    }
    fprintf(stderr, "mini_drm: flip stuck busy\n");
    return -1;
}
