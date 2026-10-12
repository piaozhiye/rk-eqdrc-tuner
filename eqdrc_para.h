/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 piaozhiye <piaozhiye@gmail.com>
 */
/*
 * eqdrc_para.h - EQDRC parameter bin (544 floats) layout and file I/O.
 *
 * Layout of the 2176-byte (544-float) parameter bin consumed by the
 * EQDRC effect library (see RK_AudioEffect_Interface.h); every index
 * is a float offset into the file.
 */

#ifndef EQDRC_PARA_H
#define EQDRC_PARA_H

#define EQDRC_PARA_NUM      544
#define EQDRC_PARA_BYTES    (EQDRC_PARA_NUM * sizeof(float))

/* ---- header ---- */
#define P_FS                0    /* sample rate */
#define P_BIT               1    /* bits per sample */
#define P_LINK              2    /* 1: both channels use channel 0 params */
#define P_CHANUM            3    /* channel count */
#define P_VERSION           4

/* ---- module switches ---- */
#define P_SW_AUTOGAIN       10
#define P_SW_BASS           11
#define P_SW_EXCITER        12
#define P_SW_DEESSER        13
#define P_SW_EQ10           14
#define P_SW_MBDRC          15
#define P_SW_AGC            16
#define P_SW_MAXI           17

/* ---- per-channel blocks ---- */
#define P_PREGAIN(ch)       (20 + (ch) * 5)     /* preGain_dB */

#define P_AUTOGAIN          30   /* global (first channel): 5 floats */
#define AG_TARGET_I_DB      0
#define AG_ATTACK_TIME      1
#define AG_RELEASE_TIME     2
#define AG_R128_WINS        3
#define AG_R128_STEPS       4

#define P_BASS(ch)          (60 + (ch) * 10)    /* 6 floats used */
#define BASS_AMOUNT_DB      0
#define BASS_DRIVE          1
#define BASS_BLEND          2
#define BASS_FREQ           3
#define BASS_FLOOR_ACTIVE   4
#define BASS_FLOOR_FREQ     5

#define P_EXCITER(ch)       (80 + (ch) * 5)     /* 4 floats used */
#define EXC_EVEN_HARM       0
#define EXC_DIST_AMOUNT     1
#define EXC_CUTOFF          2
#define EXC_MIX             3

#define P_DEESSER(ch)       (90 + (ch) * 5)     /* 2 floats used */
#define DE_F0               0
#define DE_THRESHOLD        1

/* EQ10: 10 bands per channel, 5 floats per band */
#define P_EQ(ch, band)      (100 + (ch) * 50 + (band) * 5)
#define EQ_ENABLE           0
#define EQ_TYPE             1
#define EQ_FC               2
#define EQ_Q                3
#define EQ_BOOST            4

/* filter type enum (common/filter.h) */
enum {
    EQ_FILT_PARAMETRIC = 0,
    EQ_FILT_LOWSHELF,
    EQ_FILT_HIGHSHELF,
    EQ_FILT_GENERALLP,
    EQ_FILT_GENERALHP,
    EQ_FILT_GENERALBP,
    EQ_FILT_BUTTERLP,
    EQ_FILT_BUTTERHP,
    EQ_FILT_NUM
};

/* MultiBandDRC: 100 floats per channel.
 * [0] = band count; four 20-float band slots; [90..99] backup. */
#define P_MBDRC(ch)         (200 + (ch) * 100)
#define MB_BAND_COUNT       0
/* bin band slot -> base offset inside the channel block
 * (MultiBandDRC_GetPara switch: band0=LOW, 1=HIGH, 2=MED1, 3=MED2) */
#define MB_BASE_LOW         10
#define MB_BASE_HIGH        70
#define MB_BASE_MED1        30
#define MB_BASE_MED2        50
#define MB_BAND_OFFSET      20
#define MB_PARAM_NUM        12

#define MB_FREQ_START       0
#define MB_FREQ_END         1
#define MB_GAIN_DB          2
#define MB_DRC_ENABLE       3
#define MB_COMPRESS_START   4
#define MB_EXPAND_END       5
#define MB_NOISE_THRESHOLD  6
#define MB_MAX_GAIN         7
#define MB_MAX_PEAK         8
#define MB_ATTACK_TIME      9
#define MB_RELEASE_TIME     10
#define MB_HOLD_TIME        11

/* Maximizer section: 20 floats per channel (3 Maximizer + 8 AGC used) */
#define P_MAXI(ch)          (400 + (ch) * 20)
#define MX_THRESHOLD        0
#define MX_CEILING          1
#define MX_RELEASE          2
#define MXM_AGC_COMP_START  3
#define MXM_AGC_EXPAND_END  4
#define MXM_AGC_NOISE_THR   5
#define MXM_AGC_MAX_GAIN    6
#define MXM_AGC_MAX_PEAK    7
#define MXM_AGC_ATTACK      8
#define MXM_AGC_RELEASE     9
#define MXM_AGC_HOLD        10

/* max frequency/coef parameter slots per module block, for UI ranges */

/* bin file I/O: load reads exactly EQDRC_PARA_NUM floats; save writes the
 * full array through "<path>.new" + rename so the ALSA plugin's inotify
 * (IN_MOVED_TO) picks the change up atomically. */
int eqdrc_para_load(const char *path, float *para);
int eqdrc_para_save(const char *path, const float *para);

#endif
