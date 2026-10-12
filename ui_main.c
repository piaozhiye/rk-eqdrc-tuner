/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 piaozhiye <piaozhiye@gmail.com>
 */
/*
 * eqdrc_tuner - on-device LVGL UI for live EQDRC parameter tuning.
 *
 * Edits the 544-float parameter bin in place and writes it back through an
 * atomic rename; the ALSA pcm_eqdrc plugin watches the file with inotify and
 * hot-reloads it inside the running audio stream, so slider moves take
 * effect while audio plays - no restart, no adb push.
 *
 * Board: RK3576 + 1080x1920 DSI panel (goodix touch), buildroot.
 * Porting layer: mini_drm DRM double-buffer backend, libevdev touch, lv_port_*.
 *
 * Usage: eqdrc_tuner [-p /path/to/para.bin] [-r 0|90|180|270]
 *   SIGUSR1 dumps the framebuffer to /tmp/drm_dump.raw (screenshot aid).
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <lvgl/lvgl.h>
#include <lvgl/lv_conf.h>

#include "eqdrc_para.h"

#ifdef EQDRC_PC_SIM
#include "lv_port_init.h"
void lv_port_disp_dump(void);        /* defined in lv_port_pc.c */
#else
#include "lv_port_disp.h"
#include "lv_port_indev.h"
#endif

#define DEFAULT_PARA_PATH \
    "/usr/share/rkaudio/rkaudio_effect_eqdrc_48000hz_2ch.bin"
#define SLOT_DIR_DEF    "/data/eqdrc_tuner"
#define SLOT_NUM        3
#define NSLOTS          (SLOT_NUM + 1)      /* slots + active file */

#define SW              1080
#define SH              1920
/* the glass crops the first/last rows of the panel; keep content inside */
#define SAFE_TOP        40
#define SAFE_BOT        40
#define HEADER_H        84
#define FOOTER_H        72
#define TAB_H           96

/* neon accents per tab */
#define ACCENT_EQ     0x00E5FF   /* cyan   */
#define ACCENT_DRC    0xFF2ED2   /* magenta */
#define ACCENT_AGC    0x3DFFA8   /* mint   */
#define ACCENT_SYS    0xFFC24D   /* amber  */
#define ROW_H           96
#define NAME_W          250
#define SLIDE_W         520
#define VAL_W           230

#define EQ_GAIN_MIN     (-18.0f)
#define EQ_GAIN_MAX     18.0f
#define FC_MIN          20.0f
#define FC_MAX          20000.0f
#define Q_MIN           0.1f
#define Q_MAX           16.0f

static const char *mb_band_names[4] = { "LOW", "HIGH", "MED1", "MED2" };
static const int  mb_band_base[4] = {
    MB_BASE_LOW, MB_BASE_HIGH, MB_BASE_MED1, MB_BASE_MED2
};

/* ---------------- global state ---------------- */

static float g_para[EQDRC_PARA_NUM];
static char g_para_path[512];
static time_t g_base_mtime;             /* mtime matching our cache */
static int g_link;
static int g_ch;                        /* selected channel */
static int g_eq_band;                   /* selected EQ band 0..9 */
static int g_drc_band;                  /* selected MBDRC band 0..3 */
static int g_auto_apply = 1;
static int g_pending;
static int g_write_cnt;
static uint32_t g_last_edit_tick;
static volatile int g_quit;

static lv_obj_t *g_path_label, *g_status_label;
static lv_obj_t *g_auto_sw;
static lv_timer_t *g_apply_timer, *g_poll_timer;

static void para_edited(void);
static void apply_timer_cb(lv_timer_t *t);
static void ui_sync_all(void);
static void band_gain_slider_cb(lv_event_t *e);
static void status_set(const char *fmt, ...);

/* ---------------- generic slider row ---------------- */

typedef struct {
    int idx;                /* float index into g_para */
    float min, max;
    uint8_t log_scale;      /* slider position 0..1000, exponential map */
    uint8_t dec;            /* decimals shown */
    uint32_t accent;        /* neon accent, 0 = default cyan */
    const char *unit;
    lv_obj_t *slider;
    lv_obj_t *lbl;
} srow_t;

static float pos2val(const srow_t *r, int pos)
{
    if (r->log_scale)
        return r->min * powf(r->max / r->min, (float)pos / 1000.0f);
    return (float)pos / powf(10.0f, r->dec);
}

/* single source for the EQ filter-type roller (used by both styles) */
static const char eq_filt_options[] =
    "Parametric\nLowShelf\nHighShelf\nGeneralLP\n"
    "GeneralHP\nGeneralBP\nButterLP\nButterHP";

static int val2pos(const srow_t *r, float val)
{
    if (r->log_scale) {
        if (val < r->min) val = r->min;
        if (val > r->max) val = r->max;
        return (int)(1000.0f * logf(val / r->min) / logf(r->max / r->min));
    }
    /* bin values outside the slider range are legal DSP values (e.g.
     * freq start = 0); park the knob at the end rather than letting
     * lv_slider clamp it invisibly - the label keeps showing the true
     * value with an out-of-range marker */
    if (val < r->min) val = r->min;
    if (val > r->max) val = r->max;
    return (int)lroundf(val * powf(10.0f, r->dec));
}

static void srow_update_label(const srow_t *r)
{
    float v = g_para[r->idx];
    const char *mark = (v < r->min || v > r->max) ? " *" : "";

    if (r->dec == 0)
        lv_label_set_text_fmt(r->lbl, "%.0f%s %s", v, mark,
                              r->unit ? r->unit : "");
    else
        lv_label_set_text_fmt(r->lbl, "%.*f%s %s", r->dec, v, mark,
                              r->unit ? r->unit : "");
}

static void srow_sync(srow_t *r)
{
    lv_slider_set_value(r->slider, val2pos(r, g_para[r->idx]), LV_ANIM_OFF);
    srow_update_label(r);
}

static void slider_event_cb(lv_event_t *e)
{
    srow_t *r = (srow_t *)lv_event_get_user_data(e);
    float v = pos2val(r, lv_slider_get_value(r->slider));

    g_para[r->idx] = v;
    srow_update_label(r);
    para_edited();
}

/* ---------------- neon styling helpers ---------------- */

static lv_color_t accent_mix(uint32_t accent, uint8_t opa)
{
    return lv_color_mix(lv_color_hex(accent), lv_color_hex(0x10141A), opa);
}

static void neon_slider_style(lv_obj_t *sl, uint32_t accent)
{
    lv_obj_set_style_bg_color(sl, lv_color_hex(0x1B2230), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, 0x80, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(accent), LV_PART_INDICATOR);
    lv_obj_set_style_shadow_color(sl, lv_color_hex(accent), LV_PART_INDICATOR);
    lv_obj_set_style_shadow_width(sl, 14, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_opa(sl, 0x50, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0xEAF7FF), LV_PART_KNOB);
    lv_obj_set_style_border_color(sl, lv_color_hex(accent), LV_PART_KNOB);
    lv_obj_set_style_border_width(sl, 2, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(sl, lv_color_hex(accent), LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sl, 22, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(sl, 0x80, LV_PART_KNOB);
}

static void neon_switch_style(lv_obj_t *sw, uint32_t accent)
{
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x232A36), 0);
    lv_obj_set_style_bg_color(sw, accent_mix(accent, 0xB0),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_shadow_color(sw, lv_color_hex(accent),
                                  LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_shadow_width(sw, 16,
                                  LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_shadow_opa(sw, 0x60,
                                LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0xF2FBFF), LV_PART_KNOB);
}

static void neon_btnmatrix_style(lv_obj_t *bm, uint32_t accent)
{
    lv_obj_set_style_bg_opa(bm, 0, 0);
    lv_obj_set_style_border_width(bm, 0, 0);
    lv_obj_set_style_pad_all(bm, 4, 0);
    lv_obj_set_style_bg_color(bm, lv_color_hex(0x1A2029), LV_PART_ITEMS);
    lv_obj_set_style_text_color(bm, lv_color_hex(0x93A7BC), LV_PART_ITEMS);
    lv_obj_set_style_text_font(bm, &lv_font_montserrat_26, LV_PART_ITEMS);
    lv_obj_set_style_text_font(bm, &lv_font_montserrat_26,
                               LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_radius(bm, 12, LV_PART_ITEMS);
    lv_obj_set_style_border_width(bm, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(bm, accent_mix(accent, 0x50),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(bm, lv_color_hex(0xFFFFFF),
                                LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(bm, lv_color_hex(accent),
                                  LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(bm, 2, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_shadow_color(bm, lv_color_hex(accent),
                                  LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_shadow_width(bm, 20, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_shadow_opa(bm, 0x70, LV_PART_ITEMS | LV_STATE_CHECKED);
}

/* one horizontal row: name label + slider + value label */
static void srow_create(lv_obj_t *parent, srow_t *r, const char *name,
                        int x, int y, int slide_w)
{
    lv_obj_t *lbl = lv_label_create(parent);

    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_pos(lbl, x, y + 22);
    lv_obj_set_size(lbl, NAME_W - 20, 52);

    r->slider = lv_slider_create(parent);
    if (r->log_scale) {
        lv_slider_set_range(r->slider, 0, 1000);
    } else {
        int sc = (int)powf(10.0f, r->dec);
        lv_slider_set_range(r->slider, (int)(r->min * sc), (int)(r->max * sc));
    }
    lv_obj_set_pos(r->slider, x + NAME_W, y + 12);
    lv_obj_set_size(r->slider, slide_w, 50);
    lv_obj_add_event_cb(r->slider, slider_event_cb, LV_EVENT_VALUE_CHANGED, r);
    /* fat knob/pad for touch */
    lv_obj_set_style_pad_all(r->slider, 12, LV_PART_KNOB);
    neon_slider_style(r->slider, r->accent ? r->accent : ACCENT_EQ);

    r->lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(r->lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(r->lbl,
                                lv_color_hex(r->accent ? r->accent : ACCENT_EQ),
                                0);
    lv_obj_set_pos(r->lbl, x + NAME_W + slide_w + 24, y + 26);
    lv_obj_set_size(r->lbl, VAL_W, 44);
}

/* ---------------- module switch rows ---------------- */

typedef struct {
    int idx;
    lv_obj_t *sw;
    lv_obj_t *lbl;
} mswitch_t;

static void switch_event_cb(lv_event_t *e)
{
    mswitch_t *m = (mswitch_t *)lv_event_get_user_data(e);

    g_para[m->idx] = lv_obj_has_state(m->sw, LV_STATE_CHECKED) ? 1.0f : 0.0f;
    para_edited();
}

static void mswitch_sync(mswitch_t *m)
{
    if (g_para[m->idx] > 0.5f)
        lv_obj_add_state(m->sw, LV_STATE_CHECKED);
    else
        lv_obj_clear_state(m->sw, LV_STATE_CHECKED);
}

static void mswitch_create(lv_obj_t *parent, mswitch_t *m, const char *name,
                           int x, int y, int w, uint32_t accent)
{
    m->lbl = lv_label_create(parent);
    lv_label_set_text(m->lbl, name);
    lv_obj_set_style_text_font(m->lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(m->lbl, lv_color_hex(accent), 0);
    lv_obj_set_pos(m->lbl, x, y + 12);

    m->sw = lv_switch_create(parent);
    lv_obj_set_pos(m->sw, x + w - 110, y);
    lv_obj_set_size(m->sw, 100, 56);
    lv_obj_add_event_cb(m->sw, switch_event_cb, LV_EVENT_VALUE_CHANGED, m);
    neon_switch_style(m->sw, accent);
}

/* ---------------- apply / reload plumbing ---------------- */

/*
 * Library-side invariants (README "参数 bin 布局"): enforce on every
 * edit regardless of UI style, so the shipped style-1 sliders cannot
 * produce an invalid bin either. Returns the number of clamped fields
 * so the caller can resync the widgets.
 */
static int para_enforce_constraints(void)
{
    int clamped = 0;

    for (int ch = 0; ch < 2; ch++) {
        /* MBDRC: every band slot must satisfy
         * noise < expand < compress and compress + max_gain < max_peak */
        for (int slot = 0; slot < 4; slot++) {
            int b = P_MBDRC(ch) + mb_band_base[slot];
            float *noise = &g_para[b + MB_NOISE_THRESHOLD];
            float *exp = &g_para[b + MB_EXPAND_END];
            float *comp = &g_para[b + MB_COMPRESS_START];
            float *gain = &g_para[b + MB_MAX_GAIN];
            float *peak = &g_para[b + MB_MAX_PEAK];

            if (*exp < *noise + 2.0f) { *exp = *noise + 2.0f; clamped++; }
            if (*comp < *exp + 2.0f) { *comp = *exp + 2.0f; clamped++; }
            float gmax = *peak - *comp;

            if (gmax < 0) gmax = 0;
            if (*gain > gmax) { *gain = gmax; clamped++; }
        }
        /* AGC block shares the comp/expand/noise tail layout */
        {
            int a = P_MAXI(ch) + MXM_AGC_COMP_START;
            float *comp = &g_para[a];
            float *exp = &g_para[a + 1];
            float *noise = &g_para[a + 2];

            if (*exp < *noise + 2.0f) { *exp = *noise + 2.0f; clamped++; }
            if (*comp < *exp + 2.0f) { *comp = *exp + 2.0f; clamped++; }
        }
        /* Maximizer: max_threshold < ceiling */
        {
            float *thr = &g_para[P_MAXI(ch) + MX_THRESHOLD];
            float *ceilv = &g_para[P_MAXI(ch) + MX_CEILING];

            if (*thr > *ceilv - 1.0f) { *thr = *ceilv - 1.0f; clamped++; }
        }
    }
    return clamped;
}

void para_edited(void)
{
    if (para_enforce_constraints() > 0)
        ui_sync_all();      /* cross-field clamps: reflect in widgets */
    g_pending = 1;
    g_last_edit_tick = lv_tick_get();
    if (!g_apply_timer)
        g_apply_timer = lv_timer_create(apply_timer_cb, 80, NULL);
    status_set("editing... (pending write)");
}

static const char *now_hms(void)
{
    static char buf[16];
    time_t t = time(NULL);
    struct tm tm;

    localtime_r(&t, &tm);
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min,
             tm.tm_sec);
    return buf;
}

static void do_apply(void)
{
    if (eqdrc_para_save(g_para_path, g_para) == 0) {
        struct stat st;

        g_write_cnt++;
        if (stat(g_para_path, &st) == 0)
            g_base_mtime = st.st_mtime;
        g_pending = 0;
        status_set("#%d written %s (active while playing)", g_write_cnt,
                   now_hms());
    } else {
        status_set("WRITE FAILED: %s", g_para_path);
    }
}

static void apply_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (g_pending && lv_tick_elaps(g_last_edit_tick) > 300) {
        if (g_auto_apply)
            do_apply();
        else
            status_set("edited (auto-apply off)");
    }
}

/* UI rebuild from cache: implemented at the bottom (widgets live there) */
static void ui_sync_all(void);

static void poll_timer_cb(lv_timer_t *t)
{
    (void)t;
    struct stat st;

    if (stat(g_para_path, &st) != 0 || st.st_size != EQDRC_PARA_BYTES)
        return;
    if (st.st_mtime == g_base_mtime)
        return;
    /* file changed behind our back (adb push / other tool): adopt it */
    if (eqdrc_para_load(g_para_path, g_para) == 0) {
        g_base_mtime = st.st_mtime;
        g_link = (int)g_para[P_LINK];
        ui_sync_all();
        status_set("external change adopted %s", now_hms());
    }
}

static void apply_btn_cb(lv_event_t *e)
{
    (void)e;
    do_apply();
}

static void auto_sw_cb(lv_event_t *e)
{
    (void)e;
    g_auto_apply = lv_obj_has_state(g_auto_sw, LV_STATE_CHECKED);
    status_set(g_auto_apply ? "auto-apply on" : "auto-apply off");
}

static void exit_confirm_cb(lv_event_t *e)
{
    lv_obj_t *mb = lv_event_get_current_target(e);
    const char *txt = lv_msgbox_get_active_btn_text(mb);

    if (!txt)
        return;
    if (strcmp(txt, "Reboot") == 0) {
        sync();
        if (reboot(RB_AUTOBOOT) != 0)
            status_set("reboot failed: %s", strerror(errno));
        return;                 /* dialog stays; user may retry or Quit */
    }
    g_quit = 1;
    lv_msgbox_close(mb);
}

static void exit_btn_cb(lv_event_t *e)
{
    (void)e;
    static const char *btns[] = { "Reboot", "Quit", "" };

    lv_obj_t *mb = lv_msgbox_create(NULL, NULL,
            "Exit hands the display back to the system.\n"
            "On panels that only wake on cold boot the screen stays "
            "dark - Reboot returns to the tuner.", btns, false);

    lv_obj_set_style_bg_color(mb, lv_color_hex(0x141a24), 0);
    lv_obj_center(mb);
    lv_obj_set_style_text_font(lv_msgbox_get_text(mb),
                               &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_font(lv_msgbox_get_btns(mb),
                               &lv_font_montserrat_22, LV_PART_ITEMS);
    lv_obj_add_event_cb(mb, exit_confirm_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* ---------------- EQ frequency response chart ---------------- */

static lv_obj_t *g_eq_chart;            /* canvas with the glow curve */
#define EQC_W   (SW - 16)
#define EQC_H   500
static uint32_t g_eq_cvbuf[EQC_W * EQC_H];
static lv_point_t g_curve[EQC_W];
static lv_obj_t *g_tabview;
static lv_timer_t *g_sweep_timer;
static int g_style2;
static lv_obj_t *g_tab_pages[4];
static char g_slot_dir[300];          /* preset/slot dir (EQT_SLOT_DIR) */

#define DRCV_W  (SW - 16)
#define DRCV_H  480

typedef struct {
    lv_obj_t *cv;
    uint32_t buf[DRCV_W * DRCV_H];
    int base;               /* absolute g_para index of the comp field */
    int drag;               /* 0 none / 1 comp / 2 expand / 3 noise */
    int ch_ref;             /* channel the base was built for */
} drcview_t;

/* two independent instances: MBDRC (per band) and AGC (per channel).
 * sharing one buffer used to paint the AGC curve over the MBDRC page */
static drcview_t g_drcv_m, g_drcv_a;

static void eq_tab_build(lv_obj_t *tab);
static void eq_tab_build2(lv_obj_t *tab);
static void drc_tab_build(lv_obj_t *tab);
static void drc_tab_build2(lv_obj_t *tab);
static void dyn_tab_build(lv_obj_t *tab);
static void dyn_tab_build2(lv_obj_t *tab);
static void ui_sync_all(void);

/* rebuild all tab contents in the other style, persist the choice */
static void style_set(int style2)
{
    g_style2 = style2;
    for (int i = 0; i < 3; i++)
        if (g_tab_pages[i])
            lv_obj_clean(g_tab_pages[i]);
    /* the classic pages carry no DRC canvases - drop the stale lv_obj_t
     * pointers so a later drcv_redraw can never hit freed memory */
    g_drcv_m.cv = NULL;
    g_drcv_a.cv = NULL;
    if (g_style2) {
        eq_tab_build2(g_tab_pages[0]);
        drc_tab_build2(g_tab_pages[1]);
        dyn_tab_build2(g_tab_pages[2]);
    } else {
        eq_tab_build(g_tab_pages[0]);
        drc_tab_build(g_tab_pages[1]);
        dyn_tab_build(g_tab_pages[2]);
    }
    ui_sync_all();
    {
        char path[512];

        snprintf(path, sizeof(path), "%s/style.txt", g_slot_dir);
        FILE *f = fopen(path, "w");

        if (f) {
            fputs(g_style2 ? "2" : "1", f);
            fclose(f);
        }
    }
    status_set(g_style2 ? "style: Studio Deck" : "style: classic (form)");
}

static void style_toggle_cb(lv_event_t *e)
{
    (void)e;
    style_set(!g_style2);
}

/* the tuner is a pure parameter editor; it attaches to no other audio
 * process and shows no simulated live audio */
static int g_drag_band = -1;

static inline uint32_t c32(int r, int g, int b)
{
    return 0xff000000u | ((uint32_t)(r & 0xff) << 16) |
           ((uint32_t)(g & 0xff) << 8) | (uint32_t)(b & 0xff);
}

static inline uint32_t mix32(uint32_t a, uint32_t b, int t /* 0..255 */)
{
    if (t < 0) t = 0;
    if (t > 255) t = 255;
    int ar = (a >> 16) & 0xff, ag = (a >> 8) & 0xff, ab = a & 0xff;
    int br = (b >> 16) & 0xff, bg = (b >> 8) & 0xff, bb = b & 0xff;
    return c32(ar + ((br - ar) * t >> 8),
               ag + ((bg - ag) * t >> 8),
               ab + ((bb - ab) * t >> 8));
}

/* biquad magnitude (dB) at frequency f for one EQ band */
static float band_response_db(const float *b5, float f, float fs)
{
    int type = (int)b5[EQ_TYPE];
    float fc = b5[EQ_FC], Q = b5[EQ_Q], boost = b5[EQ_BOOST];
    float A = powf(10.0f, boost / 40.0f);
    float w0 = 2.0f * (float)M_PI * fc / fs;
    float cw = cosf(w0), sw = sinf(w0);
    float alpha = sw / (2.0f * Q);
    float b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    float sqA = sqrtf(A);

    switch (type) {
    case EQ_FILT_PARAMETRIC:
        b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
        break;
    case EQ_FILT_LOWSHELF:
        alpha = sw / 2 * sqrtf(2.0f);
        b0 = A * ((A + 1) - (A - 1) * cw + 2 * sqA * alpha);
        b1 = 2 * A * ((A - 1) - (A + 1) * cw);
        b2 = A * ((A + 1) - (A - 1) * cw - 2 * sqA * alpha);
        a0 = (A + 1) + (A - 1) * cw + 2 * sqA * alpha;
        a1 = -2 * ((A - 1) + (A + 1) * cw);
        a2 = (A + 1) + (A - 1) * cw - 2 * sqA * alpha;
        break;
    case EQ_FILT_HIGHSHELF:
        alpha = sw / 2 * sqrtf(2.0f);
        b0 = A * ((A + 1) + (A - 1) * cw + 2 * sqA * alpha);
        b1 = -2 * A * ((A - 1) + (A + 1) * cw);
        b2 = A * ((A + 1) + (A - 1) * cw - 2 * sqA * alpha);
        a0 = (A + 1) - (A - 1) * cw + 2 * sqA * alpha;
        a1 = 2 * ((A - 1) - (A + 1) * cw);
        a2 = (A + 1) - (A - 1) * cw - 2 * sqA * alpha;
        break;
    case EQ_FILT_GENERALLP:
    case EQ_FILT_BUTTERLP:
        if (type == EQ_FILT_BUTTERLP) { Q = 0.7071f; alpha = sw / (2 * Q); }
        b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case EQ_FILT_GENERALHP:
    case EQ_FILT_BUTTERHP:
        if (type == EQ_FILT_BUTTERHP) { Q = 0.7071f; alpha = sw / (2 * Q); }
        b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case EQ_FILT_GENERALBP:
        b0 = alpha; b1 = 0; b2 = -alpha;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    default:
        return 0;
    }

    /* |H(e^jw)| with H = (b0 + b1 z^-1 + b2 z^-2) / (a0 + ...) */
    float w = 2.0f * (float)M_PI * f / fs;
    float cw1 = cosf(w), sw1 = sinf(w);
    float cw2 = cosf(2 * w), sw2 = sinf(2 * w);
    float nr = b0 + b1 * cw1 + b2 * cw2, ni = -(b1 * sw1 + b2 * sw2);
    float dr = a0 + a1 * cw1 + a2 * cw2, di = -(a1 * sw1 + a2 * sw2);
    float num = sqrtf(nr * nr + ni * ni), den = sqrtf(dr * dr + di * di);

    if (den < 1e-9f) den = 1e-9f;
    return 20.0f * log10f(num / den + 1e-9f);
}

static int eq_gain_to_y(float db)
{
    int y = EQC_H / 2 - (int)lroundf(db * (EQC_H / 2 - 16) / 24.0f);

    if (y < 4) y = 4;
    if (y > EQC_H - 5) y = EQC_H - 5;
    return y;
}

static float eq_x_to_fc(int x)
{
    if (x < 0) x = 0;
    if (x > EQC_W - 1) x = EQC_W - 1;
    return FC_MIN * powf(FC_MAX / FC_MIN, (float)x / (EQC_W - 1));
}

static float eq_y_to_gain(int y)
{
    return (EQC_H / 2 - y) * 24.0f / (EQC_H / 2 - 16);
}

static int eq_fc_to_x(float fc)
{
    float t;

    if (fc < FC_MIN) fc = FC_MIN;
    if (fc > FC_MAX) fc = FC_MAX;
    t = logf(fc / FC_MIN) / logf(FC_MAX / FC_MIN);

    return (int)(t * (EQC_W - 1) + 0.5f);
}

/* redraw the whole neon response canvas: wash, grid, glow curve,
 * per-band markers, sweep line, axis captions */
static void eq_chart_refresh(void)
{
    float fs = g_para[P_FS] > 4000 ? g_para[P_FS] : 48000.0f;
    uint32_t accent = c32(0x00, 0xE5, 0xFF);
    uint32_t white = c32(235, 250, 255);

    for (int x = 0; x < EQC_W; x++) {
        float f = FC_MIN * powf(FC_MAX / FC_MIN, (float)x / (EQC_W - 1));
        float total = 0;

        for (int b = 0; b < 10; b++) {
            const float *p = &g_para[P_EQ(g_ch, b)];

            if (p[EQ_ENABLE] > 0.5f)
                total += band_response_db(p, f, fs);
        }
        if (total > 24) total = 24;
        if (total < -24) total = -24;
        int y = EQC_H / 2 - (int)lroundf(total * (EQC_H / 2 - 16) / 24.0f);

        if (y < 4) y = 4;
        if (y > EQC_H - 5) y = EQC_H - 5;
        g_curve[x].x = x;
        g_curve[x].y = y;
    }

    /* vertical wash background */
    for (int y = 0; y < EQC_H; y++) {
        uint32_t c = mix32(c32(8, 11, 18), c32(14, 19, 31),
                           y * 255 / EQC_H);
        uint32_t *row = &g_eq_cvbuf[y * EQC_W];

        for (int x = 0; x < EQC_W; x++)
            row[x] = c;
    }
    /* grid: decade verticals, 0 dB and +-12 dB horizontals */
    {
        const int gf[3] = { 100, 1000, 10000 };

        for (int i = 0; i < 3; i++) {
            int gx = eq_fc_to_x(gf[i]);

            for (int y = 0; y < EQC_H; y++)
                g_eq_cvbuf[y * EQC_W + gx] =
                    mix32(g_eq_cvbuf[y * EQC_W + gx], c32(160, 190, 230), 26);
        }
        for (int d = -12; d <= 12; d += 12) {
            int gy = EQC_H / 2 - d * (EQC_H / 2 - 16) / 24;

            for (int x = 0; x < EQC_W; x++)
                g_eq_cvbuf[gy * EQC_W + x] =
                    mix32(g_eq_cvbuf[gy * EQC_W + x], c32(160, 190, 230),
                          d == 0 ? 34 : 18);
        }
    }
    /* soft fill between curve and 0 dB line */
    for (int x = 0; x < EQC_W; x++) {
        int y = g_curve[x].y, y0 = EQC_H / 2;
        int step = y < y0 ? -1 : 1;

        for (int yy = y0; yy != y; yy += step) {
            int dist = yy > y0 ? yy - y0 : y0 - yy;
            int t = 30 - dist * 30 / ((EQC_H / 2) + 1);

            g_eq_cvbuf[yy * EQC_W + x] =
                mix32(g_eq_cvbuf[yy * EQC_W + x], accent, t + 8);
        }
    }
    /* neon glow: wide dim halo, mid, bright core */
    {
        static const int pass_w[3] = { 6, 3, 1 };
        static const int pass_t[3] = { 34, 90, 235 };

        for (int pi = 0; pi < 3; pi++) {
            int w = pass_w[pi], t = pass_t[pi];

            for (int x = 0; x < EQC_W; x++)
                for (int dy = -w; dy <= w; dy++) {
                    int yy = g_curve[x].y + dy;

                    if (yy < 0 || yy >= EQC_H) continue;
                    g_eq_cvbuf[yy * EQC_W + x] =
                        mix32(g_eq_cvbuf[yy * EQC_W + x], accent,
                              t * (w + 1 - (dy < 0 ? -dy : dy)) / (w + 1));
                }
        }
        for (int x = 0; x < EQC_W; x++) {
            int yy = g_curve[x].y;

            g_eq_cvbuf[yy * EQC_W + x] = mix32(g_eq_cvbuf[yy * EQC_W + x],
                                               white, 200);
        }
    }
    /* band nodes: glow dot per enabled band, radar ring on selection */
    for (int b = 0; b < 10; b++) {
        const float *bp = &g_para[P_EQ(g_ch, b)];

        if (bp[EQ_ENABLE] < 0.5f) continue;
        int x = eq_fc_to_x(bp[EQ_FC]);
        int y = eq_gain_to_y(bp[EQ_BOOST]);
        uint32_t bc = mix32(accent, white, 120);

        for (int dy = -7; dy <= 7; dy++)
            for (int dx = -7; dx <= 7; dx++) {
                int d2 = dx * dx + dy * dy;

                if (d2 > 49) continue;
                int xx = x + dx, yy = y + dy;

                if (xx < 0 || xx >= EQC_W || yy < 0 || yy >= EQC_H)
                    continue;
                int t = d2 <= 9 ? 255 : 120 - (d2 - 9) * 8;

                if (t <= 0) continue;
                g_eq_cvbuf[yy * EQC_W + xx] =
                    mix32(g_eq_cvbuf[yy * EQC_W + xx], bc, t);
            }
        if (b == g_eq_band) {
            /* expanding radar ring, restarts every 900 ms */
            int rr = 12 + (int)((lv_tick_get() % 900) * 16 / 900);
            int tt = 200 - (rr - 12) * 11;

            if (tt < 0) tt = 0;
            int err = 0, dx = rr, dy = 0;

            while (dx >= dy) {
                int pts[8][2] = {
                    {x + dx, y + dy}, {x + dy, y + dx},
                    {x - dx, y + dy}, {x - dy, y + dx},
                    {x + dx, y - dy}, {x + dy, y - dx},
                    {x - dx, y - dy}, {x - dy, y - dx},
                };
                for (int k = 0; k < 8; k++) {
                    int xx = pts[k][0], yy = pts[k][1];

                    if (xx < 0 || xx >= EQC_W || yy < 0 || yy >= EQC_H)
                        continue;
                    g_eq_cvbuf[yy * EQC_W + xx] =
                        mix32(g_eq_cvbuf[yy * EQC_W + xx], accent, tt);
                }
                dy++;
                err += 1 + 2 * dy;
                if (err >= 2 * dx + 1) { dx--; err -= 2 * dx + 1; }
            }
        }
    }

    lv_obj_invalidate(g_eq_chart);

    /* selected band HUD chip floating above its node */
    {
        const float *sp = &g_para[P_EQ(g_ch, g_eq_band)];
        int nx = eq_fc_to_x(sp[EQ_FC]);
        int ny = eq_gain_to_y(sp[EQ_BOOST]);
        char chip[48];

        snprintf(chip, sizeof(chip), "B%d  %.0f Hz  %+.1f dB  Q%.2f",
                 g_eq_band, sp[EQ_FC], sp[EQ_BOOST], sp[EQ_Q]);
        lv_draw_label_dsc_t cd;

        lv_draw_label_dsc_init(&cd);
        cd.font = &lv_font_montserrat_24;
        cd.color = lv_color_hex(0xffffff);
        int cw = (int)strlen(chip) * 15 + 40, ch = 52;
        int cx = nx - cw / 2, cy = ny - ch - 26;

        if (cx < 4) cx = 4;
        if (cx + cw > EQC_W - 4) cx = EQC_W - 4 - cw;
        if (cy < 4) cy = ny + 26;
        if (cy + ch > EQC_H - 4) cy = EQC_H - 4 - ch;
        for (int y = cy; y < cy + ch; y++)
            for (int x = cx; x < cx + cw; x++) {
                uint32_t *px = &g_eq_cvbuf[y * EQC_W + x];
                uint8_t edge = (x == cx || x == cx + cw - 1 ||
                                y == cy || y == cy + ch - 1);

                *px = mix32(*px, accent, edge ? 220 : 42);
            }
        lv_canvas_draw_text(g_eq_chart, cx + 18, cy + 9, cw - 30, &cd,
                            chip);
    }

    /* axis captions on top of the drawing */
    static const char *cap[3] = { "100", "1k", "10k" };
    const int gf[3] = { 100, 1000, 10000 };
    lv_draw_label_dsc_t ld;

    lv_draw_label_dsc_init(&ld);
    ld.font = &lv_font_montserrat_16;
    ld.color = lv_color_hex(0x6E82A0);
    for (int i = 0; i < 3; i++) {
        int gx = eq_fc_to_x(gf[i]);
        int tx = gx + 6;

        if (tx > EQC_W - 60) tx = EQC_W - 60;
        lv_canvas_draw_text(g_eq_chart, tx, EQC_H - 26, 58, &ld, cap[i]);
    }
    lv_canvas_draw_text(g_eq_chart, 8, 6, 60, &ld, "+12");
    lv_canvas_draw_text(g_eq_chart, 8, EQC_H / 2 - 24, 60, &ld, "0");
    lv_canvas_draw_text(g_eq_chart, 8, EQC_H - 46, 60, &ld, "-12");
}



/* ---------------- EQ tab ---------------- */

static mswitch_t g_eq_sw;
static lv_obj_t *g_eq_ch_bm, *g_eq_band_bm;
static lv_obj_t *g_eq_gain_lbl[10];
static lv_obj_t *g_eq_gain_slider[10];
static srow_t g_eq_fc_row, g_eq_q_row, g_eq_gain_row;
static lv_obj_t *g_eq_type_roller;
static lv_obj_t *g_eq_enable_sw;

static char band_btn_texts[10][32];     /* DRC band buttons */
static char g_eq_band_btn_text[10][12]; /* EQ band buttons: live fc */

static void eq_enable_event_cb(lv_event_t *e)
{
    (void)e;
    g_para[P_EQ(g_ch, g_eq_band) + EQ_ENABLE] =
        lv_obj_has_state(g_eq_enable_sw, LV_STATE_CHECKED) ? 1.0f : 0.0f;
    para_edited();
    eq_chart_refresh();
}

static void eq_type_roller_cb(lv_event_t *e)
{
    (void)e;
    g_para[P_EQ(g_ch, g_eq_band) + EQ_TYPE] =
        (float)lv_roller_get_selected(g_eq_type_roller);
    para_edited();
    eq_chart_refresh();
}

static void eq_gain_slider_cb(lv_event_t *e)
{
    srow_t *r = (srow_t *)lv_event_get_user_data(e);
    float v = pos2val(r, lv_slider_get_value(r->slider));

    g_para[P_EQ(g_ch, g_eq_band) + EQ_BOOST] = v;
    srow_update_label(r);
    if (g_eq_gain_lbl[g_eq_band])
        lv_label_set_text_fmt(g_eq_gain_lbl[g_eq_band], "%+.1f", v);
    para_edited();
    eq_chart_refresh();
}

static void eq_fc_slider_cb(lv_event_t *e)
{
    srow_t *r = (srow_t *)lv_event_get_user_data(e);
    float v = pos2val(r, lv_slider_get_value(r->slider));

    g_para[P_EQ(g_ch, g_eq_band) + EQ_FC] = v;
    srow_update_label(r);
    lv_label_set_text_fmt(g_eq_gain_lbl[g_eq_band], "%.0f", v);
    para_edited();
    eq_chart_refresh();
}

static void eq_ch_bm_cb(lv_event_t *e)
{
    lv_obj_t *bm = lv_event_get_target(e);
    uint32_t id = lv_btnmatrix_get_selected_btn(bm);

    if (g_link && id != 0) {
        status_set("LINK=1: channel 1 follows channel 0");
        lv_btnmatrix_set_selected_btn(bm, 0);
        return;
    }
    g_ch = (int)id;
    ui_sync_all();
}

static void eq_band_bm_cb(lv_event_t *e)
{
    lv_obj_t *bm = lv_event_get_target(e);

    g_eq_band = (int)lv_btnmatrix_get_selected_btn(bm);
    ui_sync_all();
}

static void chart_pointer_cb(lv_event_t *e);

static void eq_tab_build(lv_obj_t *tab)
{
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    int y = 0;

    /* row 0: module switch + channel matrix */
    mswitch_create(tab, &g_eq_sw, "EQ10", 20, y + 8, 260, ACCENT_EQ);
    g_eq_sw.idx = P_SW_EQ10;

    static const char *ch_map[] = {"CH0", "CH1", ""};
    g_eq_ch_bm = lv_btnmatrix_create(tab);
    lv_btnmatrix_set_map(g_eq_ch_bm, ch_map);
    lv_obj_set_pos(g_eq_ch_bm, 560, y);
    lv_obj_set_size(g_eq_ch_bm, 480, 76);
    lv_obj_add_event_cb(g_eq_ch_bm, eq_ch_bm_cb, LV_EVENT_VALUE_CHANGED, NULL);
    neon_btnmatrix_style(g_eq_ch_bm, ACCENT_EQ);
    y += 88;

    /* response curve canvas (neon glow, custom renderer) */
    g_eq_chart = lv_canvas_create(tab);
    lv_canvas_set_buffer(g_eq_chart, g_eq_cvbuf, EQC_W, EQC_H,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(g_eq_chart, 8, y);
    lv_obj_set_size(g_eq_chart, EQC_W, EQC_H);
    lv_obj_set_style_radius(g_eq_chart, 14, 0);
    lv_obj_set_style_clip_corner(g_eq_chart, true, 0);
    lv_obj_set_style_border_color(g_eq_chart, lv_color_hex(0x233040), 0);
    lv_obj_set_style_border_width(g_eq_chart, 1, 0);
    lv_obj_add_flag(g_eq_chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_eq_chart, chart_pointer_cb, LV_EVENT_PRESSED,
                        NULL);
    lv_obj_add_event_cb(g_eq_chart, chart_pointer_cb, LV_EVENT_PRESSING,
                        NULL);
    lv_obj_add_event_cb(g_eq_chart, chart_pointer_cb, LV_EVENT_RELEASED,
                        NULL);
    lv_obj_add_event_cb(g_eq_chart, chart_pointer_cb,
                        LV_EVENT_PRESS_LOST, NULL);
    y += EQC_H + 12;

    /* 10 band columns: fc button + vertical gain slider + value */
    int col_w = (SW - 16) / 10;

    for (int b = 0; b < 10; b++) {
        int x0 = 8 + b * col_w;

        lv_obj_t *bl = lv_label_create(tab);
        lv_obj_set_style_text_font(bl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_align(bl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(bl, x0, y + 6);
        lv_obj_set_size(bl, col_w, 48);
        g_eq_gain_lbl[b] = bl;

        lv_obj_t *s = lv_slider_create(tab);
        lv_slider_set_range(s, (int)(EQ_GAIN_MIN * 10), (int)(EQ_GAIN_MAX * 10));
        lv_obj_set_pos(s, x0 + col_w / 2 - 21, y + 58);
        lv_obj_set_size(s, 42, 364);
        lv_obj_set_style_pad_all(s, 7, LV_PART_KNOB);
        neon_slider_style(s, ACCENT_EQ);
        lv_obj_add_event_cb(s, band_gain_slider_cb, LV_EVENT_VALUE_CHANGED,
                            (void *)(uintptr_t)b);
        g_eq_gain_slider[b] = s;
    }
    y += 430;

    /* band selector row: buttons carry each band's live fc (Hz) */
    {
        static const char *bmap[11];

        for (int b = 0; b < 10; b++) {
            snprintf(g_eq_band_btn_text[b], sizeof(g_eq_band_btn_text[b]),
                     "%u", (unsigned)g_para[P_EQ(g_ch, b) + EQ_FC]);
            bmap[b] = g_eq_band_btn_text[b];
        }
        bmap[10] = "";
        g_eq_band_bm = lv_btnmatrix_create(tab);
        lv_btnmatrix_set_map(g_eq_band_bm, bmap);
        lv_obj_set_pos(g_eq_band_bm, 8, y);
        lv_obj_set_size(g_eq_band_bm, SW - 16, 64);
        lv_obj_add_event_cb(g_eq_band_bm, eq_band_bm_cb,
                            LV_EVENT_VALUE_CHANGED, NULL);
        neon_btnmatrix_style(g_eq_band_bm, ACCENT_EQ);
    }
    y += 74;

    /* selected band detail: enable + type + fc + Q */
    lv_obj_t *det = lv_obj_create(tab);
    lv_obj_set_pos(det, 8, y);
    lv_obj_set_size(det, SW - 16,
                    SH - SAFE_TOP - HEADER_H - FOOTER_H - SAFE_BOT - TAB_H -
                    y - 8);
    lv_obj_clear_flag(det, LV_OBJ_FLAG_SCROLLABLE);

    g_eq_enable_sw = lv_switch_create(det);
    lv_obj_set_pos(g_eq_enable_sw, 20, 16);
    lv_obj_set_size(g_eq_enable_sw, 100, 56);
    neon_switch_style(g_eq_enable_sw, ACCENT_EQ);
    lv_obj_add_event_cb(g_eq_enable_sw, eq_enable_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *en_lbl = lv_label_create(det);
    lv_label_set_text(en_lbl, "band enable");
    lv_obj_set_style_text_font(en_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(en_lbl, 140, 30);

    g_eq_type_roller = lv_roller_create(det);
    lv_roller_set_options(g_eq_type_roller,
                          eq_filt_options,
                          LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(g_eq_type_roller, 2);
    lv_obj_set_pos(g_eq_type_roller, 660, 10);
    lv_obj_set_size(g_eq_type_roller, 360, 110);
    lv_obj_set_style_text_font(g_eq_type_roller, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_font(g_eq_type_roller, &lv_font_montserrat_26,
                               LV_PART_SELECTED);
    lv_obj_set_style_bg_color(g_eq_type_roller, lv_color_hex(0x161B24), 0);
    lv_obj_set_style_bg_color(g_eq_type_roller, accent_mix(ACCENT_EQ, 0x60),
                              LV_PART_SELECTED);
    lv_obj_set_style_text_color(g_eq_type_roller, lv_color_hex(0xFFFFFF),
                                LV_PART_SELECTED);
    lv_obj_add_event_cb(g_eq_type_roller, eq_type_roller_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    g_eq_fc_row.accent = ACCENT_EQ;
    g_eq_fc_row.idx = P_EQ(g_ch, g_eq_band) + EQ_FC;
    g_eq_fc_row.min = FC_MIN; g_eq_fc_row.max = FC_MAX;
    g_eq_fc_row.log_scale = 1; g_eq_fc_row.dec = 0; g_eq_fc_row.unit = "Hz";
    srow_create(det, &g_eq_fc_row, "fc", 20, 120, 480);
    lv_obj_remove_event_cb(g_eq_fc_row.slider, slider_event_cb);
    lv_obj_add_event_cb(g_eq_fc_row.slider, eq_fc_slider_cb,
                        LV_EVENT_VALUE_CHANGED, &g_eq_fc_row);

    g_eq_q_row.accent = ACCENT_EQ;
    g_eq_q_row.idx = P_EQ(g_ch, g_eq_band) + EQ_Q;
    g_eq_q_row.min = Q_MIN; g_eq_q_row.max = Q_MAX;
    g_eq_q_row.log_scale = 1; g_eq_q_row.dec = 2; g_eq_q_row.unit = "";
    srow_create(det, &g_eq_q_row, "Q", 20, 216, 480);

    g_eq_gain_row.accent = ACCENT_EQ;
    g_eq_gain_row.idx = P_EQ(g_ch, g_eq_band) + EQ_BOOST;
    g_eq_gain_row.min = EQ_GAIN_MIN; g_eq_gain_row.max = EQ_GAIN_MAX;
    g_eq_gain_row.log_scale = 0; g_eq_gain_row.dec = 1; g_eq_gain_row.unit = "dB";
    srow_create(det, &g_eq_gain_row, "gain", 20, 312, 480);
    lv_obj_remove_event_cb(g_eq_gain_row.slider, slider_event_cb);
    lv_obj_add_event_cb(g_eq_gain_row.slider, eq_gain_slider_cb,
                        LV_EVENT_VALUE_CHANGED, &g_eq_gain_row);
}

/* vertical per-band gain slider callback (band index in user_data) */
static void band_gain_slider_cb(lv_event_t *e)
{
    int b = (int)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *s = lv_event_get_target(e);
    float v = lv_slider_get_value(s) / 10.0f;

    g_para[P_EQ(g_ch, b) + EQ_BOOST] = v;
    if (b == g_eq_band) {
        lv_slider_set_value(g_eq_gain_row.slider, (int)(v * 10), LV_ANIM_OFF);
        srow_update_label(&g_eq_gain_row);
    }
    lv_label_set_text_fmt(g_eq_gain_lbl[b], "%.0f\n%+.1f",
                          g_para[P_EQ(g_ch, b) + EQ_FC], v);
    para_edited();
    eq_chart_refresh();
}

static void eq_tab_sync(void)
{
    mswitch_sync(&g_eq_sw);

    const float (*p_all)[5] = (const float (*)[5])&g_para[P_EQ(g_ch, 0)];

    for (int b = 0; b < 10; b++) {
        const float *p = &g_para[P_EQ(g_ch, b)];

        lv_slider_set_value(g_eq_gain_slider[b], (int)(p[EQ_BOOST] * 10),
                            LV_ANIM_OFF);
        lv_label_set_text_fmt(g_eq_gain_lbl[b], "%.0f\n%+.1f", p[EQ_FC],
                              p[EQ_BOOST]);
        lv_obj_set_style_text_color(g_eq_gain_lbl[b],
                                    p[EQ_ENABLE] > 0.5f ?
                                    lv_color_hex(0xffffff) :
                                    lv_color_hex(0x666677), 0);
    }
    {
        static const char *bmap[11];

        for (int b = 0; b < 10; b++) {
            snprintf(g_eq_band_btn_text[b], sizeof(g_eq_band_btn_text[b]),
                     "%u", (unsigned)p_all[b][EQ_FC]);
            bmap[b] = g_eq_band_btn_text[b];
        }
        bmap[10] = "";
        lv_btnmatrix_set_map(g_eq_band_bm, bmap);
    }
    g_eq_fc_row.idx = P_EQ(g_ch, g_eq_band) + EQ_FC;
    g_eq_q_row.idx = P_EQ(g_ch, g_eq_band) + EQ_Q;
    g_eq_gain_row.idx = P_EQ(g_ch, g_eq_band) + EQ_BOOST;

    const float *p = &g_para[P_EQ(g_ch, g_eq_band)];

    lv_btnmatrix_set_selected_btn(g_eq_band_bm, (uint32_t)g_eq_band);
    lv_btnmatrix_set_selected_btn(g_eq_ch_bm, (uint32_t)g_ch);
    if (g_link)
        lv_btnmatrix_set_btn_ctrl(g_eq_ch_bm, 1, LV_BTNMATRIX_CTRL_DISABLED);
    else
        lv_btnmatrix_clear_btn_ctrl(g_eq_ch_bm, 1, LV_BTNMATRIX_CTRL_DISABLED);

    if (p[EQ_ENABLE] > 0.5f)
        lv_obj_add_state(g_eq_enable_sw, LV_STATE_CHECKED);
    else
        lv_obj_clear_state(g_eq_enable_sw, LV_STATE_CHECKED);
    lv_roller_set_selected(g_eq_type_roller, (uint16_t)p[EQ_TYPE],
                               LV_ANIM_OFF);
    srow_sync(&g_eq_fc_row);
    srow_sync(&g_eq_q_row);
    srow_sync(&g_eq_gain_row);
    eq_chart_refresh();
}

/* light-weight sync while dragging a node: only the bits that show the
 * dragged band (full eq_tab_sync would restyle every slider per frame) */
static void eq_band_card_update(int b);
static void knob_sync_all(void);

static void eq_drag_ui_sync(void)
{
    int b = g_eq_band;
    const float *p = &g_para[P_EQ(g_ch, b)];
    char txt[12];

    if (g_style2) {
        eq_band_card_update(b);
        return;
    }

    lv_label_set_text_fmt(g_eq_gain_lbl[b], "%.0f\n%+.1f",
                          p[EQ_FC], p[EQ_BOOST]);
    (void)txt;
    {
        static const char *bmap[11];

        snprintf(g_eq_band_btn_text[b], sizeof(g_eq_band_btn_text[b]),
                 "%.0f", p[EQ_FC]);
        for (int i = 0; i < 10; i++)
            bmap[i] = g_eq_band_btn_text[i];
        bmap[10] = "";
        lv_btnmatrix_set_map(g_eq_band_bm, bmap);
    }

    g_eq_fc_row.idx = P_EQ(g_ch, b) + EQ_FC;
    g_eq_gain_row.idx = P_EQ(g_ch, b) + EQ_BOOST;
    srow_sync(&g_eq_fc_row);
    srow_sync(&g_eq_gain_row);
}

static void eq_drag_update(int x, int y)
{
    int b = g_eq_band;
    float *p = &g_para[P_EQ(g_ch, b)];
    float fc = eq_x_to_fc(x);
    float gain = eq_y_to_gain(y);

    if (fc < FC_MIN) fc = FC_MIN;
    if (fc > FC_MAX) fc = FC_MAX;
    if (gain < EQ_GAIN_MIN) gain = EQ_GAIN_MIN;
    if (gain > EQ_GAIN_MAX) gain = EQ_GAIN_MAX;
    p[EQ_FC] = fc;
    p[EQ_BOOST] = gain;
    eq_drag_ui_sync();
    eq_chart_refresh();
    para_edited();
}

static void chart_pointer_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();

    if (!indev)
        return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t a;
    lv_obj_get_coords(g_eq_chart, &a);
    int x = p.x - a.x1, y = p.y - a.y1;

    if (x < -20 || x > EQC_W + 20 || y < -20 || y > EQC_H + 20)
        return;

    if (code == LV_EVENT_PRESSED) {
        int best = -1, best_d = 52 * 52;

        for (int b = 0; b < 10; b++) {
            const float *bp = &g_para[P_EQ(g_ch, b)];

            if (bp[EQ_ENABLE] < 0.5f) continue;
            int dx = eq_fc_to_x(bp[EQ_FC]) - x;
            int dy = eq_gain_to_y(bp[EQ_BOOST]) - y;
            int d = dx * dx + dy * dy;

            if (d < best_d) { best_d = d; best = b; }
        }
        if (best >= 0) {
            int prev = g_eq_band;

            g_eq_band = best;
            g_drag_band = best;
            if (prev != best)
                eq_tab_sync();      /* full detail panel swap */
            eq_drag_update(x, y);
        }
    } else if (code == LV_EVENT_PRESSING && g_drag_band >= 0) {
        eq_drag_update(x, y);
    } else if (code == LV_EVENT_RELEASED ||
               code == LV_EVENT_PRESS_LOST) {
        g_drag_band = -1;
    }
}

static void ui_tick_timer_cb(lv_timer_t *t)
{
    (void)t;
    /* redraw the EQ canvas for the selected node's radar-ring animation */
    if (!g_tabview || lv_tabview_get_tab_act(g_tabview) != 0)
        return;
    eq_chart_refresh();
}

/* ---------------- DRC tab ---------------- */

static mswitch_t g_mbdrc_sw;
static lv_obj_t *g_drc_ch_bm, *g_drc_band_bm, *g_drc_count_bm;
static srow_t g_drc_rows[11];
static mswitch_t g_drc_en_sw;

/* row spec: name, field offset (relative to band base), min, max, log, dec, unit */
typedef struct {
    const char *name; int fld; float min, max; int log; int dec;
    const char *unit;
} row_spec_t;

static const row_spec_t drc_spec[11] = {
    { "freq start", MB_FREQ_START,      20, 20000, 1, 0, "Hz"  },
    { "freq end",   MB_FREQ_END,        20, 24000, 1, 0, "Hz"  },
    { "band gain",  MB_GAIN_DB,         -12,   12, 0, 1, "dB"  },
    { "compress",   MB_COMPRESS_START,  -60,    0, 0, 1, "dB"  },
    { "expand end", MB_EXPAND_END,      -90,  -10, 0, 1, "dB"  },
    { "noise thr",  MB_NOISE_THRESHOLD, -90,  -10, 0, 1, "dB"  },
    { "max gain",   MB_MAX_GAIN,          0,   30, 0, 1, "dB"  },
    { "max peak",   MB_MAX_PEAK,        -12,    0, 0, 1, "dB"  },
    { "attack",     MB_ATTACK_TIME,       1,  500, 1, 0, "ms"  },
    { "release",    MB_RELEASE_TIME,      5, 2000, 1, 0, "ms"  },
    { "hold",       MB_HOLD_TIME,         0,  500, 0, 0, "ms"  },
};

/* style-2 slider rows map into drc_spec (freq start/end, max gain, hold);
 * pinned at compile time - an index typo here is a silent wrong-param
 * sync, far worse than a build break */
static const int drc2_map[4] = { 0, 1, 6, 10 };
_Static_assert(sizeof(drc_spec) / sizeof(drc_spec[0]) == 11, "drc_spec size");

static void drc_band_count_cb(lv_event_t *e)
{
    (void)e;
    g_para[P_MBDRC(g_ch) + MB_BAND_COUNT] =
        (float)(lv_btnmatrix_get_selected_btn(g_drc_count_bm) + 2);
    para_edited();
    ui_sync_all();
}

static void drc_band_bm_cb(lv_event_t *e)
{
    lv_obj_t *bm = lv_event_get_target(e);
    int id = (int)lv_btnmatrix_get_selected_btn(bm);
    int nb = (int)g_para[P_MBDRC(g_ch) + MB_BAND_COUNT];

    if (id >= nb) {
        status_set("band %s inactive (band count = %d)", mb_band_names[id], nb);
        lv_btnmatrix_set_selected_btn(bm, g_drc_band);
        return;
    }
    g_drc_band = id;
    ui_sync_all();
}

static void drc_en_cb(lv_event_t *e)
{
    (void)e;
    g_para[P_MBDRC(g_ch) + mb_band_base[g_drc_band] + MB_DRC_ENABLE] =
        lv_obj_has_state(g_drc_en_sw.sw, LV_STATE_CHECKED) ? 1.0f : 0.0f;
    para_edited();
}

static void drc_tab_build(lv_obj_t *tab)
{
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    int y = 0;

    mswitch_create(tab, &g_mbdrc_sw, "MBDRC", 20, y + 8, 300, ACCENT_DRC);
    g_mbdrc_sw.idx = P_SW_MBDRC;

    static const char *cnt_map[] = {"2 band", "3 band", "4 band", ""};
    g_drc_count_bm = lv_btnmatrix_create(tab);
    lv_btnmatrix_set_map(g_drc_count_bm, cnt_map);
    lv_obj_set_pos(g_drc_count_bm, 400, y);
    lv_obj_set_size(g_drc_count_bm, 330, 76);
    lv_obj_add_event_cb(g_drc_count_bm, drc_band_count_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    neon_btnmatrix_style(g_drc_count_bm, ACCENT_DRC);

    static const char *ch_map2[] = {"CH0", "CH1", ""};
    g_drc_ch_bm = lv_btnmatrix_create(tab);
    lv_btnmatrix_set_map(g_drc_ch_bm, ch_map2);
    lv_obj_set_pos(g_drc_ch_bm, 760, y);
    lv_obj_set_size(g_drc_ch_bm, 280, 76);
    lv_obj_add_event_cb(g_drc_ch_bm, eq_ch_bm_cb, LV_EVENT_VALUE_CHANGED, NULL);
    neon_btnmatrix_style(g_drc_ch_bm, ACCENT_DRC);
    y += 88;

    /* band selector: labels filled at sync (freq ranges shown) */
    static const char *band_map[5];
    for (int i = 0; i < 4; i++) {
        snprintf(band_btn_texts[i], sizeof(band_btn_texts[i]), "%s",
                 mb_band_names[i]);
        band_map[i] = band_btn_texts[i];
    }
    band_map[4] = "";
    g_drc_band_bm = lv_btnmatrix_create(tab);
    lv_btnmatrix_set_map(g_drc_band_bm, band_map);
    lv_obj_set_pos(g_drc_band_bm, 8, y);
    lv_obj_set_size(g_drc_band_bm, SW - 16, 84);
    lv_obj_add_event_cb(g_drc_band_bm, drc_band_bm_cb, LV_EVENT_VALUE_CHANGED,
                        NULL);
    neon_btnmatrix_style(g_drc_band_bm, ACCENT_DRC);
    y += 96;

    /* drc_enable switch inline in the first column header row */
    mswitch_create(tab, &g_drc_en_sw, "band DRC en", 640, y + 4, 420,
                   ACCENT_DRC);
    lv_obj_add_event_cb(g_drc_en_sw.sw, drc_en_cb, LV_EVENT_VALUE_CHANGED,
                        NULL);
    y += 72;

    for (int i = 0; i < 11; i++) {
        g_drc_rows[i].accent = ACCENT_DRC;
        g_drc_rows[i].min = drc_spec[i].min;
        g_drc_rows[i].max = drc_spec[i].max;
        g_drc_rows[i].log_scale = (uint8_t)drc_spec[i].log;
        g_drc_rows[i].dec = (uint8_t)drc_spec[i].dec;
        g_drc_rows[i].unit = drc_spec[i].unit;
        srow_create(tab, &g_drc_rows[i], drc_spec[i].name, 20, y, SLIDE_W);
        y += ROW_H;
    }
}

static void drc_tab_sync(void)
{
    int base = P_MBDRC(g_ch);
    int nb = (int)g_para[base + MB_BAND_COUNT];

    mswitch_sync(&g_mbdrc_sw);

    /* band buttons show frequency ranges; inactive ones disabled */
    static const char *band_map[5];
    for (int i = 0; i < 4; i++) {
        const float *bp = &g_para[base + mb_band_base[i]];

        snprintf(band_btn_texts[i], sizeof(band_btn_texts[i]),
                 "%s\n%.0f-%.0f", mb_band_names[i], bp[MB_FREQ_START],
                 bp[MB_FREQ_END]);
        band_map[i] = band_btn_texts[i];
        lv_btnmatrix_set_btn_ctrl(g_drc_band_bm, i, LV_BTNMATRIX_CTRL_DISABLED);
    }
    band_map[4] = "";
    lv_btnmatrix_set_map(g_drc_band_bm, band_map);
    if (g_drc_band >= nb)
        g_drc_band = nb > 0 ? nb - 1 : 0;
    for (int i = 0; i < nb && i < 4; i++)
        lv_btnmatrix_clear_btn_ctrl(g_drc_band_bm, i,
                                    LV_BTNMATRIX_CTRL_DISABLED);
    lv_btnmatrix_set_selected_btn(g_drc_band_bm, g_drc_band);
    lv_btnmatrix_set_selected_btn(g_drc_count_bm,
                                  nb >= 2 && nb <= 4 ? nb - 2 : 0);
    lv_btnmatrix_set_selected_btn(g_drc_ch_bm, (uint32_t)g_ch);
    if (g_link)
        lv_btnmatrix_set_btn_ctrl(g_drc_ch_bm, 1, LV_BTNMATRIX_CTRL_DISABLED);
    else
        lv_btnmatrix_clear_btn_ctrl(g_drc_ch_bm, 1, LV_BTNMATRIX_CTRL_DISABLED);

    int bb = base + mb_band_base[g_drc_band];

    if (g_para[bb + MB_DRC_ENABLE] > 0.5f)
        lv_obj_add_state(g_drc_en_sw.sw, LV_STATE_CHECKED);
    else
        lv_obj_clear_state(g_drc_en_sw.sw, LV_STATE_CHECKED);

    for (int i = 0; i < 11; i++) {
        g_drc_rows[i].idx = bb + drc_spec[i].fld;
        srow_sync(&g_drc_rows[i]);
    }
}

/* ---------------- AGC / Maximizer tab ---------------- */

static mswitch_t g_maxi_sw, g_agc_sw;
static lv_obj_t *g_dyn_ch_bm;
static srow_t g_maxi_rows[3], g_agc_rows[8];

static const row_spec_t maxi_spec[3] = {
    { "max thr",  MX_THRESHOLD, -24, 0, 0, 1, "dB" },
    { "ceiling",  MX_CEILING,   -10, 0, 0, 1, "dB" },
    { "release",  MX_RELEASE,     1, 500, 1, 0, "ms" },
};

static const row_spec_t agc_spec[8] = {
    { "compress",   MXM_AGC_COMP_START, -60,  0, 0, 1, "dB" },
    { "expand end", MXM_AGC_EXPAND_END, -90, -10, 0, 1, "dB" },
    { "noise thr",  MXM_AGC_NOISE_THR,  -90, -10, 0, 1, "dB" },
    { "max gain",   MXM_AGC_MAX_GAIN,     0, 30, 0, 1, "dB" },
    { "max peak",   MXM_AGC_MAX_PEAK,   -12,  0, 0, 1, "dB" },
    { "attack",     MXM_AGC_ATTACK,       1, 500, 1, 0, "ms" },
    { "release",    MXM_AGC_RELEASE,      5, 2000, 1, 0, "ms" },
    { "hold",       MXM_AGC_HOLD,         0, 500, 0, 0, "ms" },
};

/* style-2 AGC rows map into agc_spec (attack release hold maxgain maxpeak) */
static const int dyn2_map[5] = { 5, 6, 7, 3, 4 };
_Static_assert(sizeof(agc_spec) / sizeof(agc_spec[0]) == 8, "agc_spec size");

/* C cannot static-assert struct members; verify the spec-index maps at
 * startup instead - a drifted map silently syncs the wrong parameter,
 * so refuse to run rather than corrupt a tuning session */
static int spec_maps_valid(void)
{
    static const int drc2_fld[4] = { MB_FREQ_START, MB_FREQ_END,
                                     MB_MAX_GAIN, MB_HOLD_TIME };
    static const int dyn2_fld[5] = { MXM_AGC_ATTACK, MXM_AGC_RELEASE,
                                     MXM_AGC_HOLD, MXM_AGC_MAX_GAIN,
                                     MXM_AGC_MAX_PEAK };

    for (int i = 0; i < 4; i++)
        if (drc_spec[drc2_map[i]].fld != drc2_fld[i])
            return 0;
    for (int i = 0; i < 5; i++)
        if (agc_spec[dyn2_map[i]].fld != dyn2_fld[i])
            return 0;
    return 1;
}

static void dyn_tab_build(lv_obj_t *tab)
{
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    int y = 0;

    mswitch_create(tab, &g_maxi_sw, "MAXIMIZER", 20, y + 8, 340, ACCENT_AGC);
    g_maxi_sw.idx = P_SW_MAXI;
    mswitch_create(tab, &g_agc_sw, "AGC", 520, y + 8, 200, ACCENT_AGC);
    g_agc_sw.idx = P_SW_AGC;

    static const char *ch_map3[] = {"CH0", "CH1", ""};
    g_dyn_ch_bm = lv_btnmatrix_create(tab);
    lv_btnmatrix_set_map(g_dyn_ch_bm, ch_map3);
    lv_obj_set_pos(g_dyn_ch_bm, 800, y);
    lv_obj_set_size(g_dyn_ch_bm, 240, 76);
    lv_obj_add_event_cb(g_dyn_ch_bm, eq_ch_bm_cb, LV_EVENT_VALUE_CHANGED, NULL);
    neon_btnmatrix_style(g_dyn_ch_bm, ACCENT_AGC);
    y += 96;

    lv_obj_t *t1 = lv_label_create(tab);
    lv_label_set_text(t1, "--- Maximizer ---");
    lv_obj_set_style_text_color(t1, lv_color_hex(ACCENT_AGC), 0);
    lv_obj_set_style_text_font(t1, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(t1, 20, y + 4);
    y += 44;

    for (int i = 0; i < 3; i++) {
        g_maxi_rows[i].accent = ACCENT_AGC;
        g_maxi_rows[i].min = maxi_spec[i].min;
        g_maxi_rows[i].max = maxi_spec[i].max;
        g_maxi_rows[i].log_scale = (uint8_t)maxi_spec[i].log;
        g_maxi_rows[i].dec = (uint8_t)maxi_spec[i].dec;
        g_maxi_rows[i].unit = maxi_spec[i].unit;
        srow_create(tab, &g_maxi_rows[i], maxi_spec[i].name, 20, y, SLIDE_W);
        y += ROW_H;
    }

    lv_obj_t *t2 = lv_label_create(tab);
    lv_label_set_text(t2, "--- AGC ---");
    lv_obj_set_style_text_color(t2, lv_color_hex(0x9dffe7), 0);
    lv_obj_set_style_text_font(t2, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(t2, 20, y + 4);
    y += 44;

    for (int i = 0; i < 8; i++) {
        g_agc_rows[i].accent = ACCENT_AGC;
        g_agc_rows[i].min = agc_spec[i].min;
        g_agc_rows[i].max = agc_spec[i].max;
        g_agc_rows[i].log_scale = (uint8_t)agc_spec[i].log;
        g_agc_rows[i].dec = (uint8_t)agc_spec[i].dec;
        g_agc_rows[i].unit = agc_spec[i].unit;
        srow_create(tab, &g_agc_rows[i], agc_spec[i].name, 20, y, SLIDE_W);
        y += ROW_H;
    }
}

static void dyn_tab_sync(void)
{
    int base = P_MAXI(g_ch);

    mswitch_sync(&g_maxi_sw);
    mswitch_sync(&g_agc_sw);
    lv_btnmatrix_set_selected_btn(g_dyn_ch_bm, (uint32_t)g_ch);
    if (g_link)
        lv_btnmatrix_set_btn_ctrl(g_dyn_ch_bm, 1, LV_BTNMATRIX_CTRL_DISABLED);
    else
        lv_btnmatrix_clear_btn_ctrl(g_dyn_ch_bm, 1, LV_BTNMATRIX_CTRL_DISABLED);
    for (int i = 0; i < 3; i++) {
        g_maxi_rows[i].idx = base + maxi_spec[i].fld;
        srow_sync(&g_maxi_rows[i]);
    }
    for (int i = 0; i < 8; i++) {
        g_agc_rows[i].idx = base + agc_spec[i].fld;
        srow_sync(&g_agc_rows[i]);
    }
}

/* ---------------- DRC transfer-curve view (MBDRC / AGC pages) ---------
 * x = input dB, y = output dB. Threshold handles (COMP / EXP / NOISE)
 * drag horizontally on the curve; ceiling follows max_peak. The widget
 * is retargeted to any parameter block whose fields sit at fixed offsets
 * (MBDRC band block and the AGC block share the same tail layout). */




static int drcv_db_to_x(float db)
{
    /* -90 .. +6 dB range mapped across the canvas with a small inset;
     * bin floats are untrusted, clamp before any buffer indexing */
    if (db < -90.0f) db = -90.0f;
    if (db > 6.0f) db = 6.0f;
    return 34 + (int)((db + 90.0f) / 96.0f * (DRCV_W - 52));
}

static float drcv_x_to_db(int x)
{
    float t = (float)(x - 34) / (DRCV_W - 52);

    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return -90.0f + t * 96.0f;
}

static int drcv_db_to_y(float db)
{
    if (db < -90.0f) db = -90.0f;
    if (db > 6.0f) db = 6.0f;
    return DRCV_H - 34 - (int)((db + 90.0f) / 96.0f * (DRCV_H - 52));
}

/* P[0]=comp P[1]=expand P[2]=noise P[3]=max_gain P[4]=max_peak */
static float drcv_out(float in, const float *P)
{
    float comp = P[0], expand = P[1], noise = P[2], maxp = P[4];

    if (in >= comp) {
        float out = comp + (in - comp) / 2.0f;

        if (out > maxp)
            out = maxp + (out - maxp) * 0.08f;
        if (out > 6.0f) out = 6.0f;
        if (out < -90.0f) out = -90.0f;
        return out;
    }
    if (in >= expand)
        return in;
    if (in >= noise) {
        float t = (expand - in) / (expand - noise + 1e-3f);

        return in - t * (expand - in) * 0.55f;
    }
    return noise + (in - noise) * 0.12f;
}

static void drcv_dot(uint32_t *buf, int x, int y, int r, uint32_t c, int t)
{
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy;

            if (d2 > r * r) continue;
            int xx = x + dx, yy = y + dy;

            if (xx < 0 || xx >= DRCV_W || yy < 0 || yy >= DRCV_H) continue;
            buf[yy * DRCV_W + xx] = mix32(buf[yy * DRCV_W + xx], c, t);
        }
}

static void drcv_redraw(drcview_t *dv)
{
    float P[5];
    uint32_t accent = c32(0xFF, 0x2E, 0xD2);
    uint32_t white = c32(235, 250, 255);
    drcview_t *g_drcv = dv;

    if (!g_drcv->cv) return;
    for (int i = 0; i < 5; i++)
        P[i] = g_para[g_drcv->base + i];

    for (int y = 0; y < DRCV_H; y++) {
        uint32_t c = mix32(c32(8, 11, 18), c32(14, 19, 31),
                           y * 255 / DRCV_H);
        uint32_t *row = &g_drcv->buf[y * DRCV_W];

        for (int x = 0; x < DRCV_W; x++)
            row[x] = c;
    }
    /* grid every 30 dB + unity line */
    for (int db = -90; db <= 0; db += 30) {
        int gx = drcv_db_to_x(db), gy = drcv_db_to_y(db);

        for (int y = 0; y < DRCV_H; y++)
            g_drcv->buf[y * DRCV_W + gx] =
                mix32(g_drcv->buf[y * DRCV_W + gx], c32(160, 190, 230), 22);
        for (int x = 0; x < DRCV_W; x++)
            g_drcv->buf[gy * DRCV_W + x] =
                mix32(g_drcv->buf[gy * DRCV_W + x], c32(160, 190, 230),
                      db == 0 ? 36 : 18);
    }
    /* ceiling: max_peak */
    {
        int cy = drcv_db_to_y(P[4]);

        for (int x = 0; x < DRCV_W; x++)
            g_drcv->buf[cy * DRCV_W + x] =
                mix32(g_drcv->buf[cy * DRCV_W + x], c32(255, 194, 77), 130);
    }
    /* transfer curve: glow passes + core */
    {
        static const int pw[3] = { 6, 3, 1 };
        static const int pt[3] = { 34, 90, 235 };
        int prev_y = drcv_db_to_y(drcv_out(-90, P));

        for (int x = 35; x < DRCV_W - 17; x++) {
            float in = drcv_x_to_db(x);
            int y = drcv_db_to_y(drcv_out(in, P));
            int y0 = prev_y, y1 = y;

            if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
            for (int yy = y0; yy <= y1; yy++)
                for (int pi = 0; pi < 3; pi++) {
                    int w = pw[pi];

                    for (int dy = -w; dy <= w; dy++) {
                        int yi = yy + dy;

                        if (yi < 0 || yi >= DRCV_H) continue;
                        g_drcv->buf[yi * DRCV_W + x] =
                            mix32(g_drcv->buf[yi * DRCV_W + x], accent,
                                  pt[pi] / (w + 1) * (w + 1 - (dy < 0 ?
                                  -dy : dy)));
                    }
                }
            g_drcv->buf[y * DRCV_W + x] =
                mix32(g_drcv->buf[y * DRCV_W + x], white, 200);
            prev_y = y;
        }
    }
    /* threshold handles: dashed vertical + diamond */
    for (int h = 0; h < 3; h++) {
        float db = P[h == 0 ? 0 : (h == 1 ? 1 : 2)];
        int x = drcv_db_to_x(db);
        uint32_t hc = h == 0 ? c32(255, 46, 210) :
                      h == 1 ? c32(0, 229, 255) : c32(255, 194, 77);

        for (int y = 6; y < DRCV_H - 6; y += 6) {
            g_drcv->buf[y * DRCV_W + x] =
                mix32(g_drcv->buf[y * DRCV_W + x], hc, 150);
            if (y + 1 < DRCV_H)
                g_drcv->buf[(y + 1) * DRCV_W + x] =
                    mix32(g_drcv->buf[(y + 1) * DRCV_W + x], hc, 60);
        }
        int y = drcv_db_to_y(drcv_out(db, P));

        drcv_dot(g_drcv->buf, x, y, 9, hc, 230);
        drcv_dot(g_drcv->buf, x, y, 4, white, 255);
    }

    lv_obj_invalidate(g_drcv->cv);

    lv_draw_label_dsc_t ld;
    char txt[48];

    lv_draw_label_dsc_init(&ld);
    ld.font = &lv_font_montserrat_18;
    ld.color = lv_color_hex(0x6e82a0);
    for (int db = -90; db <= 0; db += 30) {
        snprintf(txt, sizeof(txt), "%d", db);
        lv_canvas_draw_text(g_drcv->cv, 4, drcv_db_to_y(db) - 10, 40,
                            &ld, txt);
    }
    ld.font = &lv_font_montserrat_22;
    ld.color = lv_color_hex(0xffffff);
    snprintf(txt, sizeof(txt),
             "COMP %.0f  EXP %.0f  NOISE %.0f  PEAK %.0f dB",
             P[0], P[1], P[2], P[4]);
    lv_canvas_draw_text(g_drcv->cv, 40, 8, DRCV_W - 60, &ld, txt);
}

static void drcv_drag_update(drcview_t *dv, int x)
{
    static const int off[3] = { 0, 1, 2 };
    float *P[3];
    float v = drcv_x_to_db(x);
    int h = dv->drag;

    if (!h) return;
    for (int i = 0; i < 3; i++)
        P[i] = &g_para[dv->base + off[i]];

    *P[h - 1] = v;
    /* keep spec order: noise < expand < compress */
    if (h == 1) {                       /* compress */
        if (*P[0] < *P[1] + 2) *P[0] = *P[1] + 2;
    } else if (h == 2) {                /* expand */
        if (*P[1] > *P[0] - 2) *P[1] = *P[0] - 2;
        if (*P[1] < *P[2] + 2) *P[1] = *P[2] + 2;
    } else {                            /* noise */
        if (*P[2] > *P[1] - 2) *P[2] = *P[1] - 2;
    }
    drcv_redraw(dv);
    para_edited();
}

static void drcv_pointer_cb(lv_event_t *e)
{
    drcview_t *dv = (drcview_t *)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();

    if (!indev || !dv) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t a;
    lv_obj_get_coords(dv->cv, &a);
    int x = p.x - a.x1;

    if (code == LV_EVENT_PRESSED) {
        float P[3];

        for (int i = 0; i < 3; i++)
            P[i] = g_para[dv->base + i];
        int best = 0, best_d = 44 * 44;
        const float dbs[3] = { P[0], P[1], P[2] };

        for (int h = 0; h < 3; h++) {
            int dx = drcv_db_to_x(dbs[h]) - x;
            int d = dx * dx;

            if (d < best_d) { best_d = d; best = h + 1; }
        }
        dv->drag = best;
        if (best)
            drcv_drag_update(dv, x);
    } else if (code == LV_EVENT_PRESSING && dv->drag) {
        drcv_drag_update(dv, x);
    } else if (code == LV_EVENT_RELEASED ||
               code == LV_EVENT_PRESS_LOST) {
        dv->drag = 0;
    }
}

static void drcv_create(lv_obj_t *parent, int x, int y, drcview_t *dv)
{
    dv->cv = lv_canvas_create(parent);
    lv_canvas_set_buffer(dv->cv, dv->buf, DRCV_W, DRCV_H,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(dv->cv, x, y);
    lv_obj_set_size(dv->cv, DRCV_W, DRCV_H);
    lv_obj_set_style_radius(dv->cv, 14, 0);
    lv_obj_set_style_clip_corner(dv->cv, true, 0);
    lv_obj_set_style_border_color(dv->cv, lv_color_hex(0x233040), 0);
    lv_obj_set_style_border_width(dv->cv, 1, 0);
    lv_obj_add_flag(dv->cv, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(dv->cv, drcv_pointer_cb, LV_EVENT_PRESSED, dv);
    lv_obj_add_event_cb(dv->cv, drcv_pointer_cb, LV_EVENT_PRESSING, dv);
    lv_obj_add_event_cb(dv->cv, drcv_pointer_cb, LV_EVENT_RELEASED, dv);
    lv_obj_add_event_cb(dv->cv, drcv_pointer_cb, LV_EVENT_PRESS_LOST, dv);
}

/* ---------------- Style 2: Studio Deck (rotary knobs + band cards) --- */

#define KNOB_SZ     320
#define KNOB_R      124
#define CARD_W      96
#define CARD_H      340

typedef struct {
    const char *name;
    float min, max;
    int log_scale;
    uint8_t dec;
    const char *unit;
    uint32_t accent;
    float value;
    float add_step;         /* linear knobs: +/- per press */
    float mul_step;         /* log knobs: x/y per press (0 = linear) */
    int drag_y0;
    float drag_n0;
    lv_obj_t *cv;
    uint32_t buf[KNOB_SZ * KNOB_SZ];
    lv_obj_t *val_lbl;
    void (*changed)(float v);
} knob_t;

static knob_t g_knob_gain, g_knob_fc, g_knob_q;   /* EQ10 page */
static knob_t g_drc_knb[3];                        /* MBDRC page */
static knob_t g_dyn_knb[3];                        /* AGC/MAX page */
static lv_obj_t *g_band_card[10];
static lv_obj_t *g_card_bar[10];
static lv_obj_t *g_card_fc[10];
static lv_obj_t *g_card_gn[10];

static void knob_redraw(knob_t *k);
static void knob_drc_gain_changed(float v);
static void knob_drc_attack_changed(float v);
static void knob_drc_release_changed(float v);

static float knob_npos(knob_t *k, float v)
{
    float n;

    if (k->log_scale) {
        if (v < k->min) v = k->min;
        if (v > k->max) v = k->max;
        n = logf(v / k->min) / logf(k->max / k->min);
    } else {
        n = (v - k->min) / (k->max - k->min);
    }
    if (n < 0) n = 0;
    if (n > 1) n = 1;
    return n;
}

static float knob_val(knob_t *k, float n)
{
    if (k->log_scale)
        return k->min * powf(k->max / k->min, n);
    return k->min + n * (k->max - k->min);
}

/* arc plot: ang in degrees, 225deg=south-west sweeping clockwise to -45 */
static void knob_arc(uint32_t *buf, int cx, int cy, int r, int a0, int a1,
                     uint32_t color, int t, int thick)
{
    for (int a = a0; a >= a1; a--) {
        float rad = (float)a * (float)M_PI / 180.0f;
        int px = cx + (int)(cosf(rad) * r);
        int py = cy - (int)(sinf(rad) * r);

        for (int dy = -thick; dy <= thick; dy++)
            for (int dx = -thick; dx <= thick; dx++) {
                int xx = px + dx, yy = py + dy;

                if (xx < 0 || xx >= KNOB_SZ || yy < 0 || yy >= KNOB_SZ)
                    continue;
                buf[yy * KNOB_SZ + xx] =
                    mix32(buf[yy * KNOB_SZ + xx], color, t);
            }
    }
}

static void knob_redraw(knob_t *k)
{
    int cx = KNOB_SZ / 2, cy = KNOB_SZ / 2;
    uint32_t bg = 0xff16101c;   /* BGR order note below; use c32 */

    /* canvas TRUE_COLOR at 32bpp = XRGB; rebuild helpers give RGB layout */
    bg = c32(12, 15, 22);
    for (int y = 0; y < KNOB_SZ; y++) {
        uint32_t c = mix32(c32(10, 13, 20), bg, y * 255 / KNOB_SZ);
        uint32_t *row = &k->buf[y * KNOB_SZ];

        for (int x = 0; x < KNOB_SZ; x++)
            row[x] = c;
    }
    /* track */
    knob_arc(k->buf, cx, cy, KNOB_R, 225, -45, 0x1e2634, 210, 3);
    /* value arc + glow */
    float n = knob_npos(k, k->value);
    int aend = 225 - (int)(270.0f * n);
    uint32_t ac = k->accent;

    knob_arc(k->buf, cx, cy, KNOB_R, 225, aend, ac, 60, 6);
    knob_arc(k->buf, cx, cy, KNOB_R, 225, aend, ac, 230, 2);
    /* pointer dot at arc end */
    float rad = (float)aend * (float)M_PI / 180.0f;
    int pdx = (int)(cosf(rad) * KNOB_R), pdy = -(int)(sinf(rad) * KNOB_R);

    for (int dy = -7; dy <= 7; dy++)
        for (int dx = -7; dx <= 7; dx++) {
            if (dx * dx + dy * dy > 49) continue;
            int xx = cx + pdx + dx, yy = cy + pdy + dy;

            if (xx < 0 || xx >= KNOB_SZ || yy < 0 || yy >= KNOB_SZ)
                continue;
            k->buf[yy * KNOB_SZ + xx] = c32(240, 250, 255);
        }
    /* inner disc */
    for (int y = -70; y <= 70; y++)
        for (int x = -70; x <= 70; x++) {
            if (x * x + y * y > 70 * 70) continue;
            k->buf[(cy + y) * KNOB_SZ + (cx + x)] =
                mix32(c32(10, 13, 20), c32(22, 28, 40), y * 255 / 140 + 128);
        }
    lv_obj_invalidate(k->cv);

    /* center readout */
    lv_draw_label_dsc_t ld;
    char txt[24];

    lv_draw_label_dsc_init(&ld);
    ld.font = &lv_font_montserrat_36;
    ld.color = lv_color_hex(0xffffff);
    if (k->dec == 0)
        snprintf(txt, sizeof(txt), "%.0f", k->value);
    else
        snprintf(txt, sizeof(txt), "%.*f", k->dec, k->value);
    lv_canvas_draw_text(k->cv, cx - 70, cy - 26, 140, &ld, txt);
    ld.font = &lv_font_montserrat_18;
    ld.color = lv_color_hex(0x8fa3b8);
    lv_canvas_draw_text(k->cv, cx - 70, cy + 20, 140, &ld, k->unit);
}

static void knob_refresh_label(knob_t *k)
{
    if (k->dec == 0)
        lv_label_set_text_fmt(k->val_lbl, "%.0f %s", k->value,
                              k->unit ? k->unit : "");
    else
        lv_label_set_text_fmt(k->val_lbl, "%.*f %s", k->dec, k->value,
                              k->unit ? k->unit : "");
}

static void knob_event_cb(lv_event_t *e)
{
    knob_t *k = (knob_t *)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();

    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        k->drag_y0 = p.y;
        k->drag_n0 = knob_npos(k, k->value);
    } else if (code == LV_EVENT_PRESSING) {
        float n = k->drag_n0 + (float)(k->drag_y0 - p.y) / 420.0f;

        if (n < 0) n = 0;
        if (n > 1) n = 1;
        k->value = knob_val(k, n);
        knob_redraw(k);
        knob_refresh_label(k);
        if (k->changed)
            k->changed(k->value);
    }
}

static void knob_nudge(knob_t *k, int dir)
{
    float v = k->value;

    if (k->mul_step > 0) {
        v *= dir > 0 ? k->mul_step : 1.0f / k->mul_step;
    } else {
        v += dir * k->add_step;
    }
    if (v < k->min) v = k->min;
    if (v > k->max) v = k->max;
    k->value = v;
    knob_redraw(k);
    knob_refresh_label(k);
    if (k->changed)
        k->changed(v);
}

static void knob_minus_cb(lv_event_t *e)
{
    knob_nudge((knob_t *)lv_event_get_user_data(e), -1);
}

static void knob_plus_cb(lv_event_t *e)
{
    knob_nudge((knob_t *)lv_event_get_user_data(e), 1);
}

static void knob_create(lv_obj_t *parent, knob_t *k, int x, int y)
{
    k->cv = lv_canvas_create(parent);
    lv_canvas_set_buffer(k->cv, k->buf, KNOB_SZ, KNOB_SZ,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(k->cv, x, y);
    lv_obj_set_size(k->cv, KNOB_SZ, KNOB_SZ);
    lv_obj_add_flag(k->cv, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(k->cv, knob_event_cb, LV_EVENT_PRESSED, k);
    lv_obj_add_event_cb(k->cv, knob_event_cb, LV_EVENT_PRESSING, k);

    lv_obj_t *nm = lv_label_create(parent);
    lv_label_set_text(nm, k->name);
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(nm, lv_color_hex(k->accent), 0);
    lv_obj_set_pos(nm, x, y + KNOB_SZ + 4);
    lv_obj_set_width(nm, KNOB_SZ);
    lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_CENTER, 0);

    /* [-] value [+] stepper row: precise one-press adjustments */
    lv_obj_t *minus = lv_btn_create(parent);
    lv_obj_set_pos(minus, x + 18, y + KNOB_SZ + 34);
    lv_obj_set_size(minus, 74, 66);
    lv_obj_set_style_radius(minus, 14, 0);
    lv_obj_set_style_bg_color(minus, lv_color_hex(0x1A2029), 0);
    lv_obj_set_style_border_color(minus, lv_color_hex(k->accent), 0);
    lv_obj_set_style_border_width(minus, 2, 0);
    lv_obj_add_event_cb(minus, knob_minus_cb, LV_EVENT_CLICKED, k);
    lv_obj_t *ml = lv_label_create(minus);
    lv_label_set_text(ml, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_font(ml, &lv_font_montserrat_24, 0);
    lv_obj_center(ml);

    k->val_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(k->val_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(k->val_lbl, lv_color_hex(0xeaf7ff), 0);
    lv_obj_set_pos(k->val_lbl, x + 96, y + KNOB_SZ + 44);
    lv_obj_set_size(k->val_lbl, KNOB_SZ - 192, 56);
    lv_obj_set_style_text_align(k->val_lbl, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *plus = lv_btn_create(parent);
    lv_obj_set_pos(plus, x + KNOB_SZ - 92, y + KNOB_SZ + 34);
    lv_obj_set_size(plus, 74, 66);
    lv_obj_set_style_radius(plus, 14, 0);
    lv_obj_set_style_bg_color(plus, lv_color_hex(0x1A2029), 0);
    lv_obj_set_style_border_color(plus, lv_color_hex(k->accent), 0);
    lv_obj_set_style_border_width(plus, 2, 0);
    lv_obj_add_event_cb(plus, knob_plus_cb, LV_EVENT_CLICKED, k);
    lv_obj_t *pl = lv_label_create(plus);
    lv_label_set_text(pl, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_font(pl, &lv_font_montserrat_24, 0);
    lv_obj_center(pl);

    knob_redraw(k);
    knob_refresh_label(k);
}

/* ---- band cards ---- */

static void band_card_select(int b)
{
    for (int i = 0; i < 10; i++) {
        lv_obj_t *c = g_band_card[i];

        if (!c) continue;
        if (i == b) {
            lv_obj_set_style_border_color(c, lv_color_hex(ACCENT_EQ), 0);
            lv_obj_set_style_border_width(c, 3, 0);
            lv_obj_set_style_shadow_color(c, lv_color_hex(ACCENT_EQ), 0);
            lv_obj_set_style_shadow_width(c, 26, 0);
            lv_obj_set_style_shadow_opa(c, 0x80, 0);
        } else {
            lv_obj_set_style_border_color(c, lv_color_hex(0x232c3c), 0);
            lv_obj_set_style_border_width(c, 2, 0);
            lv_obj_set_style_shadow_width(c, 0, 0);
        }
    }
}

static void eq_band_card_update(int b)
{
    const float *p = &g_para[P_EQ(g_ch, b)];

    if (!g_band_card[b]) return;
    lv_bar_set_value(g_card_bar[b], p[EQ_BOOST], LV_ANIM_OFF);
    lv_label_set_text_fmt(g_card_fc[b], "%.0f", p[EQ_FC]);
    lv_label_set_text_fmt(g_card_gn[b], "%+.1f", p[EQ_BOOST]);
    lv_obj_set_style_bg_color(g_card_bar[b],
                              p[EQ_BOOST] >= 0 ? lv_color_hex(ACCENT_EQ) :
                              lv_color_hex(ACCENT_DRC), LV_PART_INDICATOR);
}

static void band_card_cb(lv_event_t *e)
{
    int b = (int)(uintptr_t)lv_event_get_user_data(e);

    if (g_eq_band == b) return;
    g_eq_band = b;
    band_card_select(b);
    if (g_style2) {
        knob_sync_all();
        eq_chart_refresh();
    }
}

/* knob write-throughs */
static void st2_changed_common(void)
{
    eq_band_card_update(g_eq_band);
    eq_chart_refresh();
    para_edited();
}

static void knob_gain_changed(float v)
{
    g_para[P_EQ(g_ch, g_eq_band) + EQ_BOOST] = v;
    st2_changed_common();
}

static void knob_fc_changed(float v)
{
    g_para[P_EQ(g_ch, g_eq_band) + EQ_FC] = v;
    st2_changed_common();
}

static void knob_q_changed(float v)
{
    g_para[P_EQ(g_ch, g_eq_band) + EQ_Q] = v;
    st2_changed_common();
}

static void knob_sync_all(void)
{
    const float *p = &g_para[P_EQ(g_ch, g_eq_band)];

    g_knob_gain.value = p[EQ_BOOST];
    g_knob_fc.value = p[EQ_FC];
    g_knob_q.value = p[EQ_Q];
    knob_redraw(&g_knob_gain);
    knob_redraw(&g_knob_fc);
    knob_redraw(&g_knob_q);
    knob_refresh_label(&g_knob_gain);
    knob_refresh_label(&g_knob_fc);
    knob_refresh_label(&g_knob_q);
}

static void band_cards_sync(void)
{
    for (int b = 0; b < 10; b++) {
        const float *p = &g_para[P_EQ(g_ch, b)];

        lv_bar_set_range(g_card_bar[b], (int)EQ_GAIN_MIN, (int)EQ_GAIN_MAX);
        lv_bar_set_value(g_card_bar[b], p[EQ_BOOST], LV_ANIM_OFF);
        lv_obj_set_style_bg_color(g_card_bar[b],
                                  p[EQ_BOOST] >= 0 ?
                                  lv_color_hex(ACCENT_EQ) :
                                  lv_color_hex(ACCENT_DRC),
                                  LV_PART_INDICATOR);
        lv_label_set_text_fmt(g_card_fc[b], "%.0f", p[EQ_FC]);
        lv_label_set_text_fmt(g_card_gn[b], "%+.1f", p[EQ_BOOST]);
        lv_obj_set_style_text_color(g_card_fc[b],
                                    p[EQ_ENABLE] > 0.5f ?
                                    lv_color_hex(0xffffff) :
                                    lv_color_hex(0x555f70), 0);
    }
    band_card_select(g_eq_band);
}

static void eq_tab_build2(lv_obj_t *tab)
{
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    int y = 0;

    mswitch_create(tab, &g_eq_sw, "EQ10", 20, y + 8, 260, ACCENT_EQ);
    g_eq_sw.idx = P_SW_EQ10;

    static const char *ch_map[] = {"CH0", "CH1", ""};
    g_eq_ch_bm = lv_btnmatrix_create(tab);
    lv_btnmatrix_set_map(g_eq_ch_bm, ch_map);
    lv_obj_set_pos(g_eq_ch_bm, 560, y);
    lv_obj_set_size(g_eq_ch_bm, 480, 76);
    lv_obj_add_event_cb(g_eq_ch_bm, eq_ch_bm_cb, LV_EVENT_VALUE_CHANGED,
                        NULL);
    neon_btnmatrix_style(g_eq_ch_bm, ACCENT_EQ);
    y += 88;

    /* hero canvas (same renderer / drag interaction as style 1) */
    g_eq_chart = lv_canvas_create(tab);
    lv_canvas_set_buffer(g_eq_chart, g_eq_cvbuf, EQC_W, EQC_H,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(g_eq_chart, 8, y);
    lv_obj_set_size(g_eq_chart, EQC_W, EQC_H);
    lv_obj_set_style_radius(g_eq_chart, 14, 0);
    lv_obj_set_style_clip_corner(g_eq_chart, true, 0);
    lv_obj_set_style_border_color(g_eq_chart, lv_color_hex(0x233040), 0);
    lv_obj_set_style_border_width(g_eq_chart, 1, 0);
    lv_obj_add_flag(g_eq_chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_eq_chart, chart_pointer_cb, LV_EVENT_PRESSED,
                        NULL);
    lv_obj_add_event_cb(g_eq_chart, chart_pointer_cb, LV_EVENT_PRESSING,
                        NULL);
    lv_obj_add_event_cb(g_eq_chart, chart_pointer_cb, LV_EVENT_RELEASED,
                        NULL);
    lv_obj_add_event_cb(g_eq_chart, chart_pointer_cb,
                        LV_EVENT_PRESS_LOST, NULL);
    y += EQC_H + 10;

    /* rotary cluster */
    g_knob_gain.name = "GAIN";   g_knob_gain.min = EQ_GAIN_MIN;
    g_knob_gain.max = EQ_GAIN_MAX; g_knob_gain.dec = 1;
    g_knob_gain.unit = "dB";     g_knob_gain.accent = ACCENT_EQ;
    g_knob_gain.add_step = 0.5f;      /* 0.5 dB per press */
    g_knob_gain.mul_step = 0;
    g_knob_gain.changed = knob_gain_changed;
    g_knob_fc.name = "FREQ";     g_knob_fc.min = FC_MIN;
    g_knob_fc.max = FC_MAX;      g_knob_fc.log_scale = 1;
    g_knob_fc.dec = 0;           g_knob_fc.unit = "Hz";
    g_knob_fc.accent = ACCENT_DRC;
    g_knob_fc.add_step = 0;
    g_knob_fc.mul_step = 1.0594631f;  /* semitone steps */
    g_knob_fc.changed = knob_fc_changed;
    g_knob_q.name = "Q (resonance)"; g_knob_q.min = Q_MIN;
    g_knob_q.max = Q_MAX;        g_knob_q.log_scale = 1;
    g_knob_q.dec = 2;            g_knob_q.unit = "";
    g_knob_q.accent = ACCENT_AGC;
    g_knob_q.add_step = 0;
    g_knob_q.mul_step = 1.10f;        /* ~10% per press */
    g_knob_q.changed = knob_q_changed;

    knob_create(tab, &g_knob_gain, 30, y);
    knob_create(tab, &g_knob_fc, (SW - KNOB_SZ) / 2, y);
    knob_create(tab, &g_knob_q, SW - KNOB_SZ - 30, y);
    y += KNOB_SZ + 112;

    /* selected band: enable switch + filter type */
    g_eq_enable_sw = lv_switch_create(tab);
    lv_obj_set_pos(g_eq_enable_sw, 30, y + 4);
    lv_obj_set_size(g_eq_enable_sw, 110, 62);
    neon_switch_style(g_eq_enable_sw, ACCENT_EQ);
    lv_obj_add_event_cb(g_eq_enable_sw, eq_enable_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *en_lbl = lv_label_create(tab);
    lv_label_set_text(en_lbl, "enable");
    lv_obj_set_style_text_font(en_lbl, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(en_lbl, lv_color_hex(0x93a7bc), 0);
    lv_obj_set_pos(en_lbl, 160, y + 18);

    lv_obj_t *ty_lbl = lv_label_create(tab);
    lv_label_set_text(ty_lbl, "filter type");
    lv_obj_set_style_text_font(ty_lbl, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(ty_lbl, lv_color_hex(0x93a7bc), 0);
    lv_obj_set_pos(ty_lbl, 620, y - 8);

    g_eq_type_roller = lv_roller_create(tab);
    lv_roller_set_options(g_eq_type_roller,
                          eq_filt_options,
                          LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(g_eq_type_roller, 2);
    lv_obj_set_pos(g_eq_type_roller, 620, y + 22);
    lv_obj_set_size(g_eq_type_roller, 420, 130);
    lv_obj_set_style_text_font(g_eq_type_roller, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_font(g_eq_type_roller, &lv_font_montserrat_26,
                               LV_PART_SELECTED);
    lv_obj_set_style_bg_color(g_eq_type_roller, lv_color_hex(0x161B24), 0);
    lv_obj_set_style_bg_color(g_eq_type_roller, accent_mix(ACCENT_EQ, 0x60),
                              LV_PART_SELECTED);
    lv_obj_set_style_text_color(g_eq_type_roller, lv_color_hex(0xffffff),
                                LV_PART_SELECTED);
    lv_obj_add_event_cb(g_eq_type_roller, eq_type_roller_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    y += 160;

    /* band cards */
    int gap = 8;
    int cw = (SW - 16 - 9 * gap) / 10;

    for (int b = 0; b < 10; b++) {
        int x0 = 8 + b * (cw + gap);

        lv_obj_t *card = lv_obj_create(tab);
        lv_obj_set_pos(card, x0, y);
        lv_obj_set_size(card, cw, CARD_H);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x141926), 0);
        lv_obj_set_style_radius(card, 16, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x232c3c), 0);
        lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_add_event_cb(card, band_card_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)b);
        g_band_card[b] = card;

        lv_obj_t *fc = lv_label_create(card);
        lv_obj_set_style_text_font(fc, &lv_font_montserrat_26, 0);
        lv_obj_set_style_text_align(fc, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(fc, 0, 6);
        lv_obj_set_width(fc, cw);
        g_card_fc[b] = fc;

        lv_obj_t *bar = lv_bar_create(card);
        lv_bar_set_range(bar, (int)EQ_GAIN_MIN, (int)EQ_GAIN_MAX);
        lv_obj_set_size(bar, 30, CARD_H - 150);
        lv_bar_set_mode(bar, LV_BAR_MODE_SYMMETRICAL);
        lv_obj_align(bar, LV_ALIGN_CENTER, 0, 22);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x0e1420),
                                  LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 8, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 8, LV_PART_INDICATOR);
        g_card_bar[b] = bar;

        lv_obj_t *gn = lv_label_create(card);
        lv_obj_set_style_text_font(gn, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_align(gn, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(gn, 0, CARD_H - 48);
        lv_obj_set_width(gn, cw);
        g_card_gn[b] = gn;

        lv_obj_t *bn = lv_label_create(card);
        lv_obj_set_style_text_font(bn, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(bn, lv_color_hex(0x5a6a80), 0);
        lv_obj_set_style_text_align(bn, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(bn, 0, 28);
        lv_obj_set_width(bn, cw);
        lv_label_set_text_fmt(bn, "B%d", b);
    }
    y += CARD_H + 16;

    band_cards_sync();
    knob_sync_all();
}

static void eq_tab_sync2(void)
{
    const float *p;

    mswitch_sync(&g_eq_sw);
    lv_btnmatrix_set_selected_btn(g_eq_ch_bm, (uint32_t)g_ch);
    if (g_link)
        lv_btnmatrix_set_btn_ctrl(g_eq_ch_bm, 1, LV_BTNMATRIX_CTRL_DISABLED);
    else
        lv_btnmatrix_clear_btn_ctrl(g_eq_ch_bm, 1,
                                    LV_BTNMATRIX_CTRL_DISABLED);

    p = &g_para[P_EQ(g_ch, g_eq_band)];
    if (p[EQ_ENABLE] > 0.5f)
        lv_obj_add_state(g_eq_enable_sw, LV_STATE_CHECKED);
    else
        lv_obj_clear_state(g_eq_enable_sw, LV_STATE_CHECKED);
    lv_roller_set_selected(g_eq_type_roller, (uint16_t)p[EQ_TYPE],
                           LV_ANIM_OFF);
    band_cards_sync();
    knob_sync_all();
    eq_chart_refresh();
}

/* ---------------- Style 2 MBDRC page ---------------- */

static srow_t g_drc2_rows[5];

static void drc_tab_build2(lv_obj_t *tab)
{
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    int y = 0;

    mswitch_create(tab, &g_mbdrc_sw, "MBDRC", 20, y + 8, 300, ACCENT_DRC);
    g_mbdrc_sw.idx = P_SW_MBDRC;

    static const char *cnt_map[] = {"2 band", "3 band", "4 band", ""};
    g_drc_count_bm = lv_btnmatrix_create(tab);
    lv_btnmatrix_set_map(g_drc_count_bm, cnt_map);
    lv_obj_set_pos(g_drc_count_bm, 400, y);
    lv_obj_set_size(g_drc_count_bm, 330, 76);
    lv_obj_add_event_cb(g_drc_count_bm, drc_band_count_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    neon_btnmatrix_style(g_drc_count_bm, ACCENT_DRC);

    static const char *ch_map2[] = {"CH0", "CH1", ""};
    g_drc_ch_bm = lv_btnmatrix_create(tab);
    lv_btnmatrix_set_map(g_drc_ch_bm, ch_map2);
    lv_obj_set_pos(g_drc_ch_bm, 760, y);
    lv_obj_set_size(g_drc_ch_bm, 280, 76);
    lv_obj_add_event_cb(g_drc_ch_bm, eq_ch_bm_cb, LV_EVENT_VALUE_CHANGED,
                        NULL);
    neon_btnmatrix_style(g_drc_ch_bm, ACCENT_DRC);
    y += 88;

    /* band selector (existing matrix, text filled at sync) */
    static const char *band_map[5];
    for (int i = 0; i < 4; i++) {
        snprintf(band_btn_texts[i], sizeof(band_btn_texts[i]), "%s",
                 mb_band_names[i]);
        band_map[i] = band_btn_texts[i];
    }
    band_map[4] = "";
    g_drc_band_bm = lv_btnmatrix_create(tab);
    lv_btnmatrix_set_map(g_drc_band_bm, band_map);
    lv_obj_set_pos(g_drc_band_bm, 8, y);
    lv_obj_set_size(g_drc_band_bm, SW - 16, 84);
    lv_obj_add_event_cb(g_drc_band_bm, drc_band_bm_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    neon_btnmatrix_style(g_drc_band_bm, ACCENT_DRC);
    y += 96;

    mswitch_create(tab, &g_drc_en_sw, "band DRC en", 640, y + 4, 420,
                   ACCENT_DRC);
    lv_obj_add_event_cb(g_drc_en_sw.sw, drc_en_cb, LV_EVENT_VALUE_CHANGED,
                        NULL);
    lv_obj_t *hint = lv_label_create(tab);
    lv_label_set_text(hint, "drag COMP / EXP / NOISE handles on the curve");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x5a6a80), 0);
    lv_obj_set_pos(hint, 24, y + 22);
    y += 72;

    /* threshold curve: comp/expand/noise/maxgain/maxpeak of this band */
    g_drcv_m.base = P_MBDRC(g_ch) + mb_band_base[g_drc_band] +
                    MB_COMPRESS_START;
    g_drcv_m.ch_ref = g_ch;
    drcv_create(tab, 8, y, &g_drcv_m);
    drcv_redraw(&g_drcv_m);
    y += DRCV_H + 10;

    /* knobs: band gain / attack / release */
    g_drc_knb[0].name = "GAIN";
    g_drc_knb[0].min = drc_spec[2].min;      /* band gain */
    g_drc_knb[0].max = drc_spec[2].max;
    g_drc_knb[0].log_scale = 0; g_drc_knb[0].dec = 1;
    g_drc_knb[0].unit = "dB"; g_drc_knb[0].accent = ACCENT_DRC;
    g_drc_knb[0].add_step = 0.5f; g_drc_knb[0].mul_step = 0;
    g_drc_knb[0].changed = knob_drc_gain_changed;
    g_drc_knb[1].name = "ATTACK";
    g_drc_knb[1].min = drc_spec[8].min;      /* attack */
    g_drc_knb[1].max = drc_spec[8].max;
    g_drc_knb[1].log_scale = 0; g_drc_knb[1].dec = 0;
    g_drc_knb[1].unit = "ms"; g_drc_knb[1].accent = ACCENT_AGC;
    g_drc_knb[1].add_step = 5; g_drc_knb[1].mul_step = 0;
    g_drc_knb[1].changed = knob_drc_attack_changed;
    g_drc_knb[2].name = "RELEASE";
    g_drc_knb[2].min = drc_spec[9].min;      /* release */
    g_drc_knb[2].max = drc_spec[9].max;
    g_drc_knb[2].log_scale = 0; g_drc_knb[2].dec = 0;
    g_drc_knb[2].unit = "ms"; g_drc_knb[2].accent = ACCENT_SYS;
    g_drc_knb[2].add_step = 10; g_drc_knb[2].mul_step = 0;
    g_drc_knb[2].changed = knob_drc_release_changed;
    knob_create(tab, &g_drc_knb[0], 30, y);
    knob_create(tab, &g_drc_knb[1], (SW - KNOB_SZ) / 2, y);
    knob_create(tab, &g_drc_knb[2], SW - KNOB_SZ - 30, y);
    y += KNOB_SZ + 112;

    /* remaining fields as slider rows: drc_spec is the single source
     * of ranges/names for BOTH styles (indices: fstart fend mgain hold) */
    for (int i = 0; i < 4; i++) {
        const row_spec_t *sp = &drc_spec[drc2_map[i]];

        g_drc2_rows[i].accent = ACCENT_DRC;
        g_drc2_rows[i].min = sp->min;
        g_drc2_rows[i].max = sp->max;
        g_drc2_rows[i].log_scale = (uint8_t)sp->log;
        g_drc2_rows[i].dec = (uint8_t)sp->dec;
        g_drc2_rows[i].unit = sp->unit;
        g_drc2_rows[i].idx = P_MBDRC(g_ch) + mb_band_base[g_drc_band] +
                             sp->fld;
        srow_create(tab, &g_drc2_rows[i], sp->name, 20, y, SLIDE_W);
        y += ROW_H;
    }
}

static void drc_tab_sync2(void)
{
    int base = P_MBDRC(g_ch);
    int nb = (int)g_para[base + MB_BAND_COUNT];

    mswitch_sync(&g_mbdrc_sw);

    static const char *band_map[5];
    for (int i = 0; i < 4; i++) {
        const float *bp = &g_para[base + mb_band_base[i]];

        snprintf(band_btn_texts[i], sizeof(band_btn_texts[i]),
                 "%s\n%.0f-%.0f", mb_band_names[i], bp[MB_FREQ_START],
                 bp[MB_FREQ_END]);
        band_map[i] = band_btn_texts[i];
        lv_btnmatrix_set_btn_ctrl(g_drc_band_bm, i,
                                  LV_BTNMATRIX_CTRL_DISABLED);
    }
    band_map[4] = "";
    lv_btnmatrix_set_map(g_drc_band_bm, band_map);
    if (g_drc_band >= nb)
        g_drc_band = nb > 0 ? nb - 1 : 0;
    for (int i = 0; i < nb && i < 4; i++)
        lv_btnmatrix_clear_btn_ctrl(g_drc_band_bm, i,
                                    LV_BTNMATRIX_CTRL_DISABLED);
    lv_btnmatrix_set_selected_btn(g_drc_band_bm, (uint32_t)g_drc_band);
    lv_btnmatrix_set_selected_btn(g_drc_count_bm,
                                  nb >= 2 && nb <= 4 ? nb - 2 : 0);
    lv_btnmatrix_set_selected_btn(g_drc_ch_bm, (uint32_t)g_ch);
    if (g_link)
        lv_btnmatrix_set_btn_ctrl(g_drc_ch_bm, 1,
                                  LV_BTNMATRIX_CTRL_DISABLED);
    else
        lv_btnmatrix_clear_btn_ctrl(g_drc_ch_bm, 1,
                                    LV_BTNMATRIX_CTRL_DISABLED);

    int bb = base + mb_band_base[g_drc_band];

    if (g_para[bb + MB_DRC_ENABLE] > 0.5f)
        lv_obj_add_state(g_drc_en_sw.sw, LV_STATE_CHECKED);
    else
        lv_obj_clear_state(g_drc_en_sw.sw, LV_STATE_CHECKED);

    g_drcv_m.base = bb + MB_COMPRESS_START;
    g_drcv_m.ch_ref = g_ch;
    drcv_redraw(&g_drcv_m);

    /* knobs show this band's gain / attack / release */
    g_drc_knb[0].value = g_para[bb + MB_GAIN_DB];
    g_drc_knb[1].value = g_para[bb + MB_ATTACK_TIME];
    g_drc_knb[2].value = g_para[bb + MB_RELEASE_TIME];
    for (int i = 0; i < 3; i++) {
        knob_redraw(&g_drc_knb[i]);
        knob_refresh_label(&g_drc_knb[i]);
    }

    for (int i = 0; i < 4; i++) {
        g_drc2_rows[i].idx = bb + drc_spec[drc2_map[i]].fld;
        srow_sync(&g_drc2_rows[i]);
    }
}

/* knob write-throughs for the DRC page (reuse fc/q knob slots) */
static void knob_drc_gain_changed(float v)
{
    g_para[P_MBDRC(g_ch) + mb_band_base[g_drc_band] + MB_GAIN_DB] = v;
    g_drcv_m.base = P_MBDRC(g_ch) + mb_band_base[g_drc_band] +
                    MB_COMPRESS_START;
    drcv_redraw(&g_drcv_m);
    para_edited();
}

static void knob_drc_attack_changed(float v)
{
    g_para[P_MBDRC(g_ch) + mb_band_base[g_drc_band] + MB_ATTACK_TIME] = v;
    para_edited();
}

static void knob_drc_release_changed(float v)
{
    g_para[P_MBDRC(g_ch) + mb_band_base[g_drc_band] + MB_RELEASE_TIME] = v;
    para_edited();
}

/* ---------------- Style 2 AGC/MAX page ---------------- */

static srow_t g_dyn2_rows[5];

static void knob_maxi_thr_changed(float v)
{
    g_para[P_MAXI(g_ch) + MX_THRESHOLD] = v;
    g_drcv_a.base = P_MAXI(g_ch) + MXM_AGC_COMP_START;
    drcv_redraw(&g_drcv_a);
    para_edited();
}

static void knob_maxi_ceil_changed(float v)
{
    g_para[P_MAXI(g_ch) + MX_CEILING] = v;
    g_drcv_a.base = P_MAXI(g_ch) + MXM_AGC_COMP_START;
    drcv_redraw(&g_drcv_a);
    para_edited();
}

static void knob_maxi_rel_changed(float v)
{
    g_para[P_MAXI(g_ch) + MX_RELEASE] = v;
    para_edited();
}

static void dyn_tab_build2(lv_obj_t *tab)
{
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    int y = 0;

    mswitch_create(tab, &g_maxi_sw, "MAXIMIZER", 20, y + 8, 340,
                   ACCENT_AGC);
    g_maxi_sw.idx = P_SW_MAXI;
    mswitch_create(tab, &g_agc_sw, "AGC", 520, y + 8, 200, ACCENT_AGC);
    g_agc_sw.idx = P_SW_AGC;

    static const char *ch_map3[] = {"CH0", "CH1", ""};
    g_dyn_ch_bm = lv_btnmatrix_create(tab);
    lv_btnmatrix_set_map(g_dyn_ch_bm, ch_map3);
    lv_obj_set_pos(g_dyn_ch_bm, 800, y);
    lv_obj_set_size(g_dyn_ch_bm, 240, 76);
    lv_obj_add_event_cb(g_dyn_ch_bm, eq_ch_bm_cb, LV_EVENT_VALUE_CHANGED,
                        NULL);
    neon_btnmatrix_style(g_dyn_ch_bm, ACCENT_AGC);
    y += 96;

    lv_obj_t *t1 = lv_label_create(tab);
    lv_label_set_text(t1, "AGC transfer (drag COMP / EXP / NOISE)");
    lv_obj_set_style_text_font(t1, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(t1, lv_color_hex(0x5a6a80), 0);
    lv_obj_set_pos(t1, 24, y);
    y += 40;

    g_drcv_a.base = P_MAXI(g_ch) + MXM_AGC_COMP_START;
    g_drcv_a.ch_ref = g_ch;
    drcv_create(tab, 8, y, &g_drcv_a);
    drcv_redraw(&g_drcv_a);
    y += DRCV_H + 10;

    g_dyn_knb[0].name = "MAX THR";
    g_dyn_knb[0].min = maxi_spec[0].min;
    g_dyn_knb[0].max = maxi_spec[0].max;
    g_dyn_knb[0].log_scale = 0; g_dyn_knb[0].dec = 1;
    g_dyn_knb[0].unit = "dB"; g_dyn_knb[0].accent = ACCENT_AGC;
    g_dyn_knb[0].add_step = 0.5f; g_dyn_knb[0].mul_step = 0;
    g_dyn_knb[0].changed = knob_maxi_thr_changed;
    g_dyn_knb[1].name = "CEILING";
    g_dyn_knb[1].min = maxi_spec[1].min;
    g_dyn_knb[1].max = maxi_spec[1].max;
    g_dyn_knb[1].log_scale = 0; g_dyn_knb[1].dec = 1;
    g_dyn_knb[1].unit = "dB"; g_dyn_knb[1].accent = ACCENT_EQ;
    g_dyn_knb[1].add_step = 0.5f; g_dyn_knb[1].mul_step = 0;
    g_dyn_knb[1].changed = knob_maxi_ceil_changed;
    g_dyn_knb[2].name = "RELEASE";
    g_dyn_knb[2].min = maxi_spec[2].min;
    g_dyn_knb[2].max = maxi_spec[2].max;
    g_dyn_knb[2].log_scale = 0; g_dyn_knb[2].dec = 0;
    g_dyn_knb[2].unit = "ms"; g_dyn_knb[2].accent = ACCENT_DRC;
    g_dyn_knb[2].add_step = 5; g_dyn_knb[2].mul_step = 0;
    g_dyn_knb[2].changed = knob_maxi_rel_changed;
    knob_create(tab, &g_dyn_knb[0], 30, y);
    knob_create(tab, &g_dyn_knb[1], (SW - KNOB_SZ) / 2, y);
    knob_create(tab, &g_dyn_knb[2], SW - KNOB_SZ - 30, y);
    y += KNOB_SZ + 112;

    /* agc_spec is the single source for BOTH styles */
    for (int i = 0; i < 5; i++) {
        const row_spec_t *sp = &agc_spec[dyn2_map[i]];

        g_dyn2_rows[i].accent = ACCENT_AGC;
        g_dyn2_rows[i].min = sp->min;
        g_dyn2_rows[i].max = sp->max;
        g_dyn2_rows[i].log_scale = (uint8_t)sp->log;
        g_dyn2_rows[i].dec = (uint8_t)sp->dec;
        g_dyn2_rows[i].unit = sp->unit;
        g_dyn2_rows[i].idx = P_MAXI(g_ch) + sp->fld;
        srow_create(tab, &g_dyn2_rows[i], sp->name, 20, y, SLIDE_W);
        y += ROW_H;
    }
}

static void dyn_tab_sync2(void)
{
    int base = P_MAXI(g_ch);

    mswitch_sync(&g_maxi_sw);
    mswitch_sync(&g_agc_sw);
    lv_btnmatrix_set_selected_btn(g_dyn_ch_bm, (uint32_t)g_ch);
    if (g_link)
        lv_btnmatrix_set_btn_ctrl(g_dyn_ch_bm, 1,
                                  LV_BTNMATRIX_CTRL_DISABLED);
    else
        lv_btnmatrix_clear_btn_ctrl(g_dyn_ch_bm, 1,
                                    LV_BTNMATRIX_CTRL_DISABLED);

    g_drcv_a.base = base + MXM_AGC_COMP_START;
    g_drcv_a.ch_ref = g_ch;
    drcv_redraw(&g_drcv_a);

    g_dyn_knb[0].value = g_para[base + MX_THRESHOLD];
    g_dyn_knb[1].value = g_para[base + MX_CEILING];
    g_dyn_knb[2].value = g_para[base + MX_RELEASE];
    for (int i = 0; i < 3; i++) {
        knob_redraw(&g_dyn_knb[i]);
        knob_refresh_label(&g_dyn_knb[i]);
    }

    for (int i = 0; i < 5; i++) {
        g_dyn2_rows[i].idx = base + agc_spec[dyn2_map[i]].fld;
        srow_sync(&g_dyn2_rows[i]);
    }
}

/* ---------------- SYS tab: preGain / AutoGain / other modules / files --- */

static mswitch_t g_sys_sw[8];
static const char *sys_sw_names[8] = {
    "EQ10", "MBDRC", "AGC", "MAXI", "AutoGain", "Bass", "Exciter", "DeEsser"
};
static const int sys_sw_idx[8] = {
    P_SW_EQ10, P_SW_MBDRC, P_SW_AGC, P_SW_MAXI,
    P_SW_AUTOGAIN, P_SW_BASS, P_SW_EXCITER, P_SW_DEESSER,
};

static srow_t g_pre_row[2], g_ag_rows[5], g_bass_rows[3], g_exc_rows[2],
              g_de_rows[2];
static lv_obj_t *g_preset_roller, *g_path_full_label;
static char g_preset_files[NSLOTS][512];
static int g_preset_cnt;
static int g_preset_sel = -1;

static void sys_scan_presets(void)
{
    g_preset_cnt = 0;

    /* active file first */
    snprintf(g_preset_files[g_preset_cnt], sizeof(g_preset_files[0]),
             "%s", g_para_path);
    g_preset_cnt++;

    DIR *d = opendir(g_slot_dir);

    if (d) {
        struct dirent *ent;

        while ((ent = readdir(d)) && g_preset_cnt < NSLOTS) {
            if (ent->d_type != DT_REG) continue;
            const char *ext = strstr(ent->d_name, ".bin");

            if (!ext || strcmp(ext, ".bin") != 0) continue;
            snprintf(g_preset_files[g_preset_cnt],
                     sizeof(g_preset_files[0]), "%s/%.*s", g_slot_dir,
                     200, ent->d_name);
            g_preset_cnt++;
        }
        closedir(d);
    }
}

static void preset_roller_cb(lv_event_t *e)
{
    (void)e;
    int sel = (int)lv_roller_get_selected(g_preset_roller);

    if (sel < 0 || sel >= g_preset_cnt)
        return;
    g_preset_sel = sel;
    lv_label_set_text_fmt(g_path_full_label, "sel: %s",
                          g_preset_files[g_preset_sel]);
}

static void preset_load_cb(lv_event_t *e)
{
    (void)e;
    if (g_preset_sel < 0 || g_preset_sel >= g_preset_cnt) {
        status_set("pick a file first");
        return;
    }
    if (eqdrc_para_load(g_preset_files[g_preset_sel], g_para) != 0) {
        status_set("load failed: %s", g_preset_files[g_preset_sel]);
        return;
    }
    g_link = (int)g_para[P_LINK];
    ui_sync_all();
    status_set("loaded %s -> applying", g_preset_files[g_preset_sel]);
    para_edited();      /* writes into the ACTIVE file (hot reload) */
}

static void slot_save_cb(lv_event_t *e)
{
    int slot = (int)(uintptr_t)lv_event_get_user_data(e);
    char path[512];

    snprintf(path, sizeof(path), "%s/slot%d.bin", g_slot_dir, slot);
    if (eqdrc_para_save(path, g_para) == 0) {
        status_set("saved %s", path);
        sys_scan_presets();
        /* refresh roller options */
        static char opts[4096];

        opts[0] = 0;
        for (int i = 0; i < g_preset_cnt; i++) {
            strncat(opts, g_preset_files[i], sizeof(opts) - strlen(opts) - 2);
            if (i + 1 < g_preset_cnt)
                strncat(opts, "\n", sizeof(opts) - strlen(opts) - 2);
        }
        lv_roller_set_options(g_preset_roller, opts, LV_ROLLER_MODE_NORMAL);
    } else {
        status_set("slot save failed");
    }
}

static void sys_tab_build(lv_obj_t *tab)
{
    int y = 0;

    lv_obj_t *cap = lv_label_create(tab);
    lv_label_set_text(cap, "module switches");
    lv_obj_set_style_text_color(cap, lv_color_hex(0xffd27f), 0);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(cap, 20, y);
    y += 40;

    for (int i = 0; i < 8; i++) {
        int col = i % 4, row = i / 4;

        mswitch_create(tab, &g_sys_sw[i], sys_sw_names[i],
                       20 + col * 265, y + row * 84, 220,
                       i < 4 ? ACCENT_SYS : 0x9AB0C8);
        g_sys_sw[i].idx = sys_sw_idx[i];
    }
    y += 2 * 84 + 8;

    g_pre_row[0].accent = ACCENT_SYS;
    g_pre_row[0].idx = P_PREGAIN(0);
    g_pre_row[0].min = -24; g_pre_row[0].max = 12;
    g_pre_row[0].dec = 1; g_pre_row[0].unit = "dB";
    srow_create(tab, &g_pre_row[0], "preGain ch0", 20, y, SLIDE_W);
    y += ROW_H;
    g_pre_row[1].accent = ACCENT_SYS;
    g_pre_row[1].idx = P_PREGAIN(1);
    g_pre_row[1].min = -24; g_pre_row[1].max = 12;
    g_pre_row[1].dec = 1; g_pre_row[1].unit = "dB";
    srow_create(tab, &g_pre_row[1], "preGain ch1", 20, y, SLIDE_W);
    y += ROW_H;

    lv_obj_t *t1 = lv_label_create(tab);
    lv_label_set_text(t1, "--- AutoGain (EBU R128) ---");
    lv_obj_set_style_text_color(t1, lv_color_hex(0xffd27f), 0);
    lv_obj_set_style_text_font(t1, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(t1, 20, y + 4);
    y += 44;

    static const struct { const char *n; int fld; float mn, mx; int dec;
                          const char *u; } ag_spec[5] = {
        { "target", AG_TARGET_I_DB,  -40, -6, 1, "dB" },
        { "attack", AG_ATTACK_TIME,    5, 2000, 0, "ms" },
        { "release", AG_RELEASE_TIME,  5, 4000, 0, "ms" },
        { "win",    AG_R128_WINS,    100, 5000, 0, "ms" },
        { "step",   AG_R128_STEPS,    10, 1000, 0, "ms" },
    };
    for (int i = 0; i < 5; i++) {
        g_ag_rows[i].accent = ACCENT_SYS;
        g_ag_rows[i].idx = P_AUTOGAIN + ag_spec[i].fld;
        g_ag_rows[i].min = ag_spec[i].mn;
        g_ag_rows[i].max = ag_spec[i].mx;
        g_ag_rows[i].dec = (uint8_t)ag_spec[i].dec;
        g_ag_rows[i].unit = ag_spec[i].u;
        srow_create(tab, &g_ag_rows[i], ag_spec[i].n, 20, y, SLIDE_W);
        y += ROW_H;
    }

    lv_obj_t *t2 = lv_label_create(tab);
    lv_label_set_text(t2, "--- Bass / Exciter / DeEsser (ch0) ---");
    lv_obj_set_style_text_color(t2, lv_color_hex(0xffd27f), 0);
    lv_obj_set_style_text_font(t2, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(t2, 20, y + 4);
    y += 44;

    static const struct { const char *n; int off; float mn, mx; int dec;
                          const char *u; } oth_spec[7] = {
        { "bass amount", BASS_AMOUNT_DB,   0, 15,   1, "dB" },
        { "bass drive",  BASS_DRIVE,       0, 10,   2, "" },
        { "bass freq",   BASS_FREQ,       30, 300,  0, "Hz" },
        { "excit cutoff", EXC_CUTOFF,   1000, 12000, 0, "Hz" },
        { "excit mix",   EXC_MIX,          0, 1,    2, "" },
        { "deesser f0",  DE_F0,         2000, 9000, 0, "Hz" },
        { "deesser thr", DE_THRESHOLD,  -60, 0,    1, "dB" },
    };
    int bases[7];
    for (int i = 0; i < 7; i++)
        bases[i] = i < 3 ? P_BASS(0) : (i < 5 ? P_EXCITER(0) : P_DEESSER(0));
    for (int i = 0; i < 7; i++) {
        srow_t *r = i < 3 ? &g_bass_rows[i]
                  : (i < 5 ? &g_exc_rows[i - 3] : &g_de_rows[i - 5]);

        r->accent = ACCENT_SYS;
        r->idx = bases[i] + oth_spec[i].off;
        r->min = oth_spec[i].mn;
        r->max = oth_spec[i].mx;
        r->dec = (uint8_t)oth_spec[i].dec;
        r->unit = oth_spec[i].u;
        srow_create(tab, r, oth_spec[i].n, 20, y, SLIDE_W);
        y += ROW_H;
    }

    /* file section */
    lv_obj_t *t3 = lv_label_create(tab);
    lv_label_set_text(t3, "--- file / presets ---");
    lv_obj_set_style_text_color(t3, lv_color_hex(0xffd27f), 0);
    lv_obj_set_style_text_font(t3, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(t3, 20, y + 4);
    y += 44;

    g_path_full_label = lv_label_create(tab);
    lv_label_set_text(g_path_full_label, g_para_path);
    lv_obj_set_style_text_font(g_path_full_label, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(g_path_full_label, 20, y);
    lv_obj_set_width(g_path_full_label, SW - 40);
    lv_label_set_long_mode(g_path_full_label, LV_LABEL_LONG_DOT);
    y += 48;

    g_preset_roller = lv_roller_create(tab);
    lv_roller_set_visible_row_count(g_preset_roller, 3);
    lv_obj_set_pos(g_preset_roller, 20, y);
    lv_obj_set_size(g_preset_roller, 620, 190);
    lv_obj_add_event_cb(g_preset_roller, preset_roller_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *load_btn = lv_btn_create(tab);
    lv_obj_set_pos(load_btn, 670, y);
    lv_obj_set_size(load_btn, 370, 80);
    lv_obj_add_event_cb(load_btn, preset_load_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lb = lv_label_create(load_btn);
    lv_label_set_text(lb, LV_SYMBOL_UPLOAD " load+apply");
    lv_obj_center(lb);
    y += 200;

    for (int i = 0; i < SLOT_NUM; i++) {
        lv_obj_t *b = lv_btn_create(tab);
        lv_obj_set_pos(b, 20 + i * 350, y);
        lv_obj_set_size(b, 330, 76);
        lv_obj_add_event_cb(b, slot_save_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)(i + 1));
        lv_obj_t *bl = lv_label_create(b);
        lv_label_set_text_fmt(bl, "save %s/slot%d.bin", g_slot_dir, i + 1);
        lv_obj_center(bl);
    }
    y += 96;
}

static void sys_tab_sync(void)
{
    for (int i = 0; i < 8; i++)
        mswitch_sync(&g_sys_sw[i]);
    srow_sync(&g_pre_row[0]);
    srow_sync(&g_pre_row[1]);
    for (int i = 0; i < 5; i++)
        srow_sync(&g_ag_rows[i]);
    for (int i = 0; i < 3; i++)
        srow_sync(&g_bass_rows[i]);
    for (int i = 0; i < 2; i++) {
        srow_sync(&g_exc_rows[i]);
        srow_sync(&g_de_rows[i]);
    }
}

/* ---------------- header / footer ---------------- */

static void status_set(const char *fmt, ...)
{
    static char buf[192];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_status_label)
        lv_label_set_text(g_status_label, buf);
}

static void build_header_footer(lv_obj_t *scr)
{
    lv_obj_t *hdr = lv_obj_create(scr);

    lv_obj_set_pos(hdr, 0, SAFE_TOP);
    lv_obj_set_size(hdr, SW, HEADER_H);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x121722), 0);
    lv_obj_set_style_bg_grad_color(hdr, lv_color_hex(0x0A0D14), 0);
    lv_obj_set_style_bg_grad_dir(hdr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_shadow_color(hdr, lv_color_hex(ACCENT_EQ), 0);
    lv_obj_set_style_shadow_width(hdr, 26, 0);
    lv_obj_set_style_shadow_opa(hdr, 0x28, 0);

    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, LV_SYMBOL_VOLUME_MAX " EQDRC TUNER");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_shadow_width(title, 24, 0);
    lv_obj_set_style_shadow_color(title, lv_color_hex(ACCENT_EQ), 0);
    lv_obj_set_style_shadow_opa(title, 0xB0, 0);
    lv_obj_set_pos(title, 16, 22);

    g_path_label = lv_label_create(hdr);
    lv_label_set_text(g_path_label, g_para_path);
    lv_obj_set_style_text_font(g_path_label, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(g_path_label, lv_color_hex(0x9ab0c8), 0);
    lv_obj_set_pos(g_path_label, 480, 32);
    lv_obj_set_width(g_path_label, SW - 480 - 120);
    lv_label_set_long_mode(g_path_label, LV_LABEL_LONG_DOT);

    lv_obj_t *style_btn = lv_btn_create(hdr);
    lv_obj_set_pos(style_btn, SW - 200, 12);
    lv_obj_set_size(style_btn, 84, 60);
    lv_obj_set_style_radius(style_btn, 14, 0);
    lv_obj_set_style_bg_color(style_btn, lv_color_hex(0x1A2029), 0);
    lv_obj_set_style_border_color(style_btn, lv_color_hex(ACCENT_SYS), 0);
    lv_obj_set_style_border_width(style_btn, 2, 0);
    lv_obj_add_event_cb(style_btn, style_toggle_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *slbl = lv_label_create(style_btn);
    lv_label_set_text(slbl, LV_SYMBOL_SHUFFLE);
    lv_obj_set_style_text_font(slbl, &lv_font_montserrat_24, 0);
    lv_obj_center(slbl);

    lv_obj_t *exit_btn = lv_btn_create(hdr);
    lv_obj_set_pos(exit_btn, SW - 100, 12);
    lv_obj_set_size(exit_btn, 84, 60);
    lv_obj_set_style_radius(exit_btn, 14, 0);
    lv_obj_add_event_cb(exit_btn, exit_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(exit_btn);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(xl, &lv_font_montserrat_24, 0);
    lv_obj_center(xl);

    /* footer */
    lv_obj_t *ftr = lv_obj_create(scr);
    lv_obj_set_pos(ftr, 0, SH - SAFE_BOT - FOOTER_H);
    lv_obj_set_size(ftr, SW, FOOTER_H);
    lv_obj_clear_flag(ftr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ftr, lv_color_hex(0x0A0D14), 0);
    lv_obj_set_style_bg_grad_color(ftr, lv_color_hex(0x121722), 0);
    lv_obj_set_style_bg_grad_dir(ftr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(ftr, 0, 0);

    g_status_label = lv_label_create(ftr);
    lv_label_set_text(g_status_label, "ready");
    lv_obj_set_style_text_font(g_status_label, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(g_status_label, 16, 20);
    lv_obj_set_width(g_status_label, SW - 560);
    lv_label_set_long_mode(g_status_label, LV_LABEL_LONG_DOT);

    g_auto_sw = lv_switch_create(ftr);
    lv_obj_set_pos(g_auto_sw, SW - 500, 8);
    lv_obj_set_size(g_auto_sw, 90, 52);
    neon_switch_style(g_auto_sw, ACCENT_EQ);
    if (g_auto_apply)
        lv_obj_add_state(g_auto_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(g_auto_sw, auto_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *al = lv_label_create(ftr);
    lv_label_set_text(al, "auto");
    lv_obj_set_style_text_font(al, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(al, lv_color_hex(ACCENT_EQ), 0);
    lv_obj_set_pos(al, SW - 396, 20);

    lv_obj_t *wr = lv_btn_create(ftr);
    lv_obj_set_pos(wr, SW - 300, 4);
    lv_obj_set_size(wr, 280, 62);
    lv_obj_set_style_radius(wr, 16, 0);
    lv_obj_set_style_bg_color(wr, accent_mix(ACCENT_EQ, 0x50), 0);
    lv_obj_set_style_border_color(wr, lv_color_hex(ACCENT_EQ), 0);
    lv_obj_set_style_border_width(wr, 2, 0);
    lv_obj_set_style_shadow_color(wr, lv_color_hex(ACCENT_EQ), 0);
    lv_obj_set_style_shadow_width(wr, 26, 0);
    lv_obj_set_style_shadow_opa(wr, 0x80, 0);
    lv_obj_add_event_cb(wr, apply_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *wl = lv_label_create(wr);
    lv_label_set_text(wl, LV_SYMBOL_SAVE " write now");
    lv_obj_center(wl);
}

/* ---------------- ui_sync_all ---------------- */

static void ui_sync_all(void)
{
    if (g_style2) {
        eq_tab_sync2();
        drc_tab_sync2();
        dyn_tab_sync2();
        sys_tab_sync();
        return;
    }
    eq_tab_sync();
    drc_tab_sync();
    dyn_tab_sync();
    sys_tab_sync();
}

/* ---------------- main ---------------- */

static void sig_handler(int sig)
{
    if (sig == SIGUSR1)
        lv_port_disp_dump();
    else
        g_quit = 1;
}

int main(int argc, char **argv)
{
    int rot = 0;
    const char *path = DEFAULT_PARA_PATH;
    int opt;

    (void)rot;                          /* board port reads it via env */

    while ((opt = getopt(argc, argv, "p:r:")) != -1) {
        switch (opt) {
        case 'p': path = optarg; break;
        case 'r': rot = atoi(optarg); break;
        default:
            fprintf(stderr, "usage: %s [-p para.bin] [-r 0|90|180|270]\n",
                    argv[0]);
            return 1;
        }
    }

    {
        /* the panel's DRM master is exclusive: a second instance would
         * fight the first one's modeset (seen first-hand) */
        static FILE *lock_hold;      /* keep the lock for our lifetime */

        lock_hold = fopen("/tmp/eqt.lock", "w");
        if (!lock_hold ||
            lockf(fileno(lock_hold), F_TLOCK, 0) < 0) {
            fprintf(stderr, "eqdrc_tuner: already running (/tmp/eqt.lock)\n");
            return 1;
        }
    }
    if (!spec_maps_valid()) {
        fprintf(stderr, "eqdrc_tuner: internal spec-map mismatch\n");
        return 1;
    }

    snprintf(g_para_path, sizeof(g_para_path), "%s", path);
    if (access(g_para_path, R_OK) != 0 && !strcmp(path, DEFAULT_PARA_PATH)) {
        /* PC sim convenience: local working copy */
        snprintf(g_para_path, sizeof(g_para_path), "eqdrc_tuner.bin");
    }
    if (eqdrc_para_load(g_para_path, g_para) != 0) {
        fprintf(stderr, "cannot load %s\n", g_para_path);
        return 1;
    }
    struct stat st;

    if (stat(g_para_path, &st) == 0)
        g_base_mtime = st.st_mtime;
    para_enforce_constraints();
    g_link = (int)g_para[P_LINK];
    {
        const char *sd = getenv("EQT_SLOT_DIR");

        snprintf(g_slot_dir, sizeof(g_slot_dir), "%s",
                 sd ? sd : SLOT_DIR_DEF);
    }
    mkdir(g_slot_dir, 0755);

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGUSR1, sig_handler);

#ifdef EQDRC_PC_SIM
    lv_port_init();             /* lv_init + SDL display + mouse input */
#else
    lv_init();
    lv_port_disp_init(0, 0, rot);
    lv_port_indev_init(rot);
#endif

    lv_disp_t *disp = lv_disp_get_default();
    lv_theme_t *th = lv_theme_default_init(disp,
                                           lv_palette_main(LV_PALETTE_BLUE),
                                           lv_palette_main(LV_PALETTE_TEAL),
                                           true, &lv_font_montserrat_20);
    lv_disp_set_theme(disp, th);

    lv_obj_t *scr = lv_scr_act();

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
    build_header_footer(scr);

    lv_obj_t *tv = lv_tabview_create(scr, LV_DIR_TOP, TAB_H);
    lv_obj_set_pos(tv, 0, SAFE_TOP + HEADER_H);
    lv_obj_set_size(tv, SW, SH - SAFE_TOP - HEADER_H - SAFE_BOT - FOOTER_H);
    g_tabview = tv;

    /* style priority: EQT_STYLE env > saved toggle choice > Studio Deck */
    {
        const char *env = getenv("EQT_STYLE");

        if (env)
            g_style2 = atoi(env) == 1 ? 0 : 1;
        else {
            char path[512];
            FILE *f;

            snprintf(path, sizeof(path), "%s/style.txt", g_slot_dir);
            f = fopen(path, "r");
            g_style2 = 1;
            if (f) {
                int c = fgetc(f);

                if (c == '1')
                    g_style2 = 0;
                fclose(f);
            }
        }
    }

    lv_obj_t *tab_eq = lv_tabview_add_tab(tv, "EQ10");
    lv_obj_t *tab_drc = lv_tabview_add_tab(tv, "MBDRC");
    lv_obj_t *tab_dyn = lv_tabview_add_tab(tv, "AGC/MAX");
    lv_obj_t *tab_sys = lv_tabview_add_tab(tv, "SYS");

    lv_obj_t *tabs[4] = { tab_eq, tab_drc, tab_dyn, tab_sys };

    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_color(tabs[i], lv_color_hex(0x0C0F16), 0);
        lv_obj_set_style_bg_grad_color(tabs[i], lv_color_hex(0x11141D), 0);
        lv_obj_set_style_bg_grad_dir(tabs[i], LV_GRAD_DIR_VER, 0);
    }
    /* tab bar: dark with neon active page */
    lv_obj_set_style_bg_color(tv, lv_color_hex(0x0B0E14), LV_PART_MAIN);
    lv_obj_set_style_border_width(tv, 0, 0);
    neon_btnmatrix_style(tv, ACCENT_EQ);
    lv_obj_set_style_text_font(tv, &lv_font_montserrat_26, LV_PART_ITEMS);
    lv_obj_set_style_text_font(tv, &lv_font_montserrat_26,
                               LV_PART_ITEMS | LV_STATE_CHECKED);

    g_tab_pages[0] = tab_eq;
    g_tab_pages[1] = tab_drc;
    g_tab_pages[2] = tab_dyn;
    if (g_style2) {
        eq_tab_build2(tab_eq);
        drc_tab_build2(tab_drc);
        dyn_tab_build2(tab_dyn);
    } else {
        eq_tab_build(tab_eq);
        drc_tab_build(tab_drc);
        dyn_tab_build(tab_dyn);
    }
    sys_tab_build(tab_sys);

    sys_scan_presets();
    {
        static char opts[4096];

        opts[0] = 0;
        for (int i = 0; i < g_preset_cnt; i++) {
            strncat(opts, g_preset_files[i], sizeof(opts) - strlen(opts) - 2);
            if (i + 1 < g_preset_cnt)
                strncat(opts, "\n", sizeof(opts) - strlen(opts) - 2);
        }
        lv_roller_set_options(g_preset_roller, opts, LV_ROLLER_MODE_NORMAL);
    }

    ui_sync_all();
    {
        /* report (never rewrite) values outside their UI ranges: legal
         * DSP values this tool cannot display faithfully, e.g.
         * freq start = 0; the row label carries a "*" marker */
        const struct { const srow_t *rows; int cnt; } grp[] = {
            { g_drc_rows, 11 },  { g_drc2_rows, 4 },
            { g_maxi_rows, 3 },  { g_agc_rows, 8 },
            { g_dyn2_rows, 5 },  { g_ag_rows, 5 },
            { g_pre_row, 2 },    { g_bass_rows, 3 },
            { g_exc_rows, 2 },   { g_de_rows, 2 },
            { &g_eq_fc_row, 1 }, { &g_eq_q_row, 1 }, { &g_eq_gain_row, 1 },
        };
        int out = 0;

        for (unsigned g = 0; g < sizeof(grp) / sizeof(grp[0]); g++)
            for (int i = 0; i < grp[g].cnt; i++) {
                const srow_t *r = &grp[g].rows[i];

                if (!r->slider)          /* not built in this style */
                    continue;
                if (g_para[r->idx] < r->min || g_para[r->idx] > r->max)
                    out++;
            }
        /* knobs carry no srow_t: check their params explicitly */
        {
            const float *ep = &g_para[P_EQ(g_ch, g_eq_band)];
            const float *db = &g_para[P_MBDRC(g_ch) +
                                     mb_band_base[g_drc_band]];
            const float *mp = &g_para[P_MAXI(g_ch)];
            struct { float v, lo, hi; } kchk[] = {
                { ep[EQ_FC],   FC_MIN,  FC_MAX },
                { ep[EQ_Q],    Q_MIN,   Q_MAX },
                { ep[EQ_BOOST], EQ_GAIN_MIN, EQ_GAIN_MAX },
                { db[MB_GAIN_DB],     drc_spec[2].min, drc_spec[2].max },
                { db[MB_ATTACK_TIME], drc_spec[8].min, drc_spec[8].max },
                { db[MB_RELEASE_TIME], drc_spec[9].min, drc_spec[9].max },
                { mp[MX_THRESHOLD], maxi_spec[0].min, maxi_spec[0].max },
                { mp[MX_CEILING],   maxi_spec[1].min, maxi_spec[1].max },
                { mp[MX_RELEASE],   maxi_spec[2].min, maxi_spec[2].max },
            };

            for (unsigned i = 0; i < sizeof(kchk) / sizeof(kchk[0]); i++)
                if (kchk[i].v < kchk[i].lo || kchk[i].v > kchk[i].hi)
                    out++;
        }
        if (out)
            status_set("loaded %s | %d param(s) outside UI range (kept "
                       "as-is, marked *)", g_para_path, out);
        else
            status_set("loaded %s", g_para_path);
    }

    {
        const char *tab = getenv("EQT_TAB");

        if (tab)
            lv_tabview_set_act(tv, atoi(tab), LV_ANIM_OFF);
    }

    g_poll_timer = lv_timer_create(poll_timer_cb, 1000, NULL);
    g_sweep_timer = lv_timer_create(ui_tick_timer_cb, 66, NULL);

    while (!g_quit) {
        lv_task_handler();
        usleep(3000);
    }

    printf("eqdrc_tuner: bye (%d writes)\n", g_write_cnt);
    return 0;
}
