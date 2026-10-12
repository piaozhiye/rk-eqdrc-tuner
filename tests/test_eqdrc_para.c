/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 piaozhiye <piaozhiye@gmail.com>
 *
 * Host-side tests for the parameter bin I/O and layout. The offset
 * table pins the README layout as a compile-time spec; the runtime
 * tests cover save/load round-trip (including unused tail slots),
 * short-read atomicity and save failure cleanliness.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "eqdrc_para.h"

/* ---- layout pin: these MUST match the README offset table ---- */
_Static_assert(EQDRC_PARA_NUM == 544, "544 floats per bin");
_Static_assert(P_EQ(1, 9) + EQ_BOOST == 199, "ch1 last EQ slot");
_Static_assert(P_MAXI(1) + MXM_AGC_HOLD == 430, "ch1 AGC hold");
_Static_assert(P_MBDRC(1) + MB_BASE_HIGH + MB_HOLD_TIME == 381,
               "ch1 highest used MBDRC field (HIGH slot tail)");
_Static_assert(P_MBDRC(0) + MB_BASE_LOW == 210, "LOW slot");
_Static_assert(P_MBDRC(0) + MB_BASE_MED1 == 230, "MED1 slot");
_Static_assert(P_MBDRC(0) + MB_BASE_MED2 == 250, "MED2 slot");
_Static_assert(P_MBDRC(0) + MB_BASE_HIGH == 270, "HIGH slot");
_Static_assert(P_BASS(1) + BASS_FLOOR_FREQ == 75, "bass ch1 tail");
_Static_assert(P_DEESSER(1) + DE_THRESHOLD == 96, "deesser ch1");

#define TEST_BIN "/tmp/eqt_test_para.bin"

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        failures++; \
    } \
} while (0)

int main(void)
{
    float para[EQDRC_PARA_NUM], back[EQDRC_PARA_NUM];

    /* deterministic pattern covering every slot incl. unused tail */
    for (int i = 0; i < EQDRC_PARA_NUM; i++)
        para[i] = (float)(i * 7 - 1000);
    para[P_FS] = 48000.0f;
    para[P_SW_EQ10] = 1.0f;

    CHECK(eqdrc_para_save(TEST_BIN, para) == 0, "save ok");
    {
        struct stat st;

        CHECK(stat(TEST_BIN ".new", &st) != 0, "no .new residue");
        CHECK(stat(TEST_BIN, &st) == 0 && st.st_size == EQDRC_PARA_BYTES,
              "exact 2176-byte file");
    }
    memset(back, 0, sizeof(back));
    CHECK(eqdrc_para_load(TEST_BIN, back) == 0, "load ok");
    CHECK(memcmp(para, back, sizeof(para)) == 0,
          "round-trip byte-exact incl. unused slots");

    /* short file: caller buffer must stay untouched */
    {
        float guard[EQDRC_PARA_NUM];
        FILE *f = fopen(TEST_BIN, "wb");

        CHECK(f != NULL, "open short file for write");
        fwrite(para, sizeof(float), 100, f);   /* 100 of 544 */
        fclose(f);
        for (int i = 0; i < EQDRC_PARA_NUM; i++)
            guard[i] = 12345.0f;
        CHECK(eqdrc_para_load(TEST_BIN, guard) == -1, "short read fails");
        for (int i = 0; i < EQDRC_PARA_NUM; i++)
            if (guard[i] != 12345.0f) {
                CHECK(0, "short read polluted caller buffer");
                break;
            }
    }

    /* trailing junk (wrong total size): also rejected */
    {
        float guard[EQDRC_PARA_NUM];
        FILE *f = fopen(TEST_BIN, "wb");

        fwrite(para, sizeof(float), EQDRC_PARA_NUM, f);
        fwrite(para, sizeof(float), 3, f);
        fclose(f);
        CHECK(eqdrc_para_load(TEST_BIN, guard) == -1, "oversize rejected");
    }

    /* missing file */
    {
        float guard[EQDRC_PARA_NUM];

        CHECK(eqdrc_para_load("/tmp/eqt_test_noexist.bin", guard) == -1,
              "missing file fails");
    }

    unlink(TEST_BIN);
    if (failures) {
        fprintf(stderr, "test_eqdrc_para: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_eqdrc_para: PASS (%d floats, offsets pinned)\n",
           EQDRC_PARA_NUM);
    return 0;
}
