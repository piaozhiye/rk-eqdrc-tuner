/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 piaozhiye <piaozhiye@gmail.com>
 */
/* eqdrc_para.c - bin file I/O for the EQDRC tuner UI */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "eqdrc_para.h"

int eqdrc_para_load(const char *path, float *para)
{
    /* stage into a local buffer first: a short/corrupt file must leave
     * the caller's array untouched (a half-loaded array would be handed
     * straight to eqdrc_para_save by the auto-apply timer otherwise) */
    static float staged[EQDRC_PARA_NUM];
    FILE *f = fopen(path, "rb");

    if (!f) {
        fprintf(stderr, "eqdrc_para: open %s: %s\n", path, strerror(errno));
        return -1;
    }
    size_t n = fread(staged, sizeof(float), EQDRC_PARA_NUM, f);
    int short_read = n < (size_t)EQDRC_PARA_NUM ||
                     fgetc(f) != EOF;      /* trailing junk = wrong size */
    fclose(f);
    if (short_read) {
        fprintf(stderr, "eqdrc_para: %s: read %zu floats, want %d\n",
                path, n, EQDRC_PARA_NUM);
        return -1;
    }
    memcpy(para, staged, sizeof(staged));
    return 0;
}

int eqdrc_para_save(const char *path, const float *para)
{
    char tmp[512];

    snprintf(tmp, sizeof(tmp), "%s.new", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "eqdrc_para: create %s: %s\n", tmp, strerror(errno));
        return -1;
    }
    if (fwrite(para, sizeof(float), EQDRC_PARA_NUM, f) != EQDRC_PARA_NUM) {
        fprintf(stderr, "eqdrc_para: write %s failed\n", tmp);
        fclose(f);
        unlink(tmp);
        return -1;
    }
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fprintf(stderr, "eqdrc_para: flush %s: %s\n", tmp, strerror(errno));
        fclose(f);
        unlink(tmp);
        return -1;
    }
    fclose(f);

    /* rename triggers the plugin's IN_MOVED_TO hot reload */
    if (rename(tmp, path) != 0) {
        fprintf(stderr, "eqdrc_para: rename %s -> %s: %s\n",
                tmp, path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}
