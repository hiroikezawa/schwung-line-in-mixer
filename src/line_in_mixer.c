/* Line In Mixer - Schwung audio FX: adds Move's audio input on top of this
 * chain's own sound (the slot's sound generator and any FX before this one).
 *
 *   out = internal + ext * MIX * duck * (MUTE ? 0 : 1) * (FB GUARD ? 0 : 1)
 *   ext = width( (1 - w) * strip(ANALOG) + w * strip(USB) ),  w = SOURCE
 *   strip = pan( level * gain * LPF( HPF( input ) ) )
 *
 * The internal signal is never scaled, filtered or clipped by anything here
 * except the final int16 clamp of the sum: with the external part at zero the
 * output is the input bit for bit. MIX, SOURCE, MUTE and DUCK act only on the
 * external part.
 *
 * WHERE THE INPUT COMES FROM (Schwung 1.7.3 / Move 2.x, read from the source)
 *   Move's audio input reaches Linux as ONE stereo stream: 128 frames of int16
 *   in the SPI mailbox at host->mapped_memory + host->audio_in_offset, refilled
 *   every block (44.1 kHz, same clock as this DSP). The XMOS audio chip decides
 *   what is in it - the 3.5 mm line in (or the built-in mic when nothing is
 *   plugged in) OR the USB-C audio coming from a computer / iPad - with bit0 of
 *   its 37 12 routing word, which Move's own sampling page owns
 *   (docs/SPI_PROTOCOL.md). So:
 *     - there is no second stream: ANALOG and USB-C cannot be heard at the
 *       same time, and BOTH is not offered;
 *     - this module cannot see which source the XMOS is delivering. SOURCE
 *       picks which of the two channel strips (gain/level/HPF/LPF/pan) is
 *       applied; the input itself is chosen on Move (sampling input source).
 *     - the stream is already resampled to and clocked by the SPI frame, so
 *       there is no sample rate mismatch or drift to correct here, and no
 *       ring buffer that could under- or overrun. Unplugging USB-C delivers
 *       zeros; nothing here waits on it.
 *   This is the same input the in-tree linein sound generator and
 *   timncox/schwung-smack's smack-in read; this module reads it as an audio
 *   FX so the slot's sound generator stays where it is.
 *
 * FEEDBACK GUARD
 *   With SOURCE = ANALOG, nothing in the line-in jack and the built-in speaker
 *   playing, the input is the built-in microphone listening to the speaker.
 *   Schwung's own guard only covers sound generators, so this module mutes its
 *   external part itself in exactly that state (STATUS shows FB GUARD). The
 *   jack states are the shim's (shadow_control_t.speaker_active /
 *   line_in_connected, /schwung-control), mapped read-only once.
 *
 * DUCK: an envelope follower on the internal signal (1 ms attack, 200 ms
 *   release) turns the external part down while the sound generator plays:
 *   gain = 1 - DUCK * min(1, env / 0.25). DUCK 100 % silences it while the
 *   internal sound peaks at -12 dBFS or above. DUCK 0 % = no ducking.
 *
 * HPF 20 Hz and LPF 20000 Hz are OFF (exactly bypassed); in between each is a
 * 12 dB/oct Butterworth TPT state-variable filter. PAN is a balance control
 * on the stereo input. WIDTH is mid/side on the summed external signal
 * (0 % mono, 100 % unchanged, 200 % double side).
 *
 * Threads/realtime: process_block, set_param and get_param run on the SPI
 * callback. No allocation after init (static instance pool), no locks, no I/O
 * there. Shared fields are relaxed C11 atomics.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiro Ikezawa
 */
#define _POSIX_C_SOURCE 200809L /* O_CLOEXEC */
#include "host/audio_fx_api_v2.h"
#include "host/shadow_constants.h" /* shadow_control_t: speaker / line-in jack state */
#include "line_in_mixer_meta.h"    /* generated from module.json by scripts/metadata.py */

#include <fcntl.h>
#include <math.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))

/* The two bytes read from /schwung-control. Identical in Schwung 1.4 - 1.7.3;
 * a header that moves them fails the build instead of reading the wrong byte. */
_Static_assert(offsetof(shadow_control_t, speaker_active) == 58, "shadow_control_t layout changed");
_Static_assert(offsetof(shadow_control_t, line_in_connected) == 59, "shadow_control_t layout changed");
#define CTRL_MAP_BYTES 64

#define LIM_POOL 64              /* 4 slots x 8 FX + Master FX + sends, with room */
#define SMOOTH_MS 20.0f          /* gain / pan / width / mix glide */
#define SOURCE_XFADE_MS 15.0f    /* SOURCE switch crossfade */
#define DUCK_ATTACK_MS 1.0f
#define DUCK_RELEASE_MS 200.0f
#define DUCK_FULL 0.25f          /* -12 dBFS internal peak = full duck */
#define NOSIG_SECONDS 2.0f
#define HPF_OFF_HZ 20.0f
#define LPF_OFF_HZ 20000.0f

enum { SRC_ANALOG, SRC_USB };
enum { ST_OK, ST_MUTED, ST_FBGUARD, ST_NOSIG };

/* ---- the user parameters (order = state order) ------------------------- */
enum {
    P_MIX, P_AN_LEVEL, P_USB_LEVEL, P_SOURCE, P_AN_GAIN, P_USB_GAIN, P_DUCK, P_MUTE,
    P_AN_HPF, P_USB_HPF, P_AN_LPF, P_USB_LPF, P_AN_PAN, P_USB_PAN, P_WIDTH,
    P_COUNT
};

static const char *const OPT_SOURCE[] = {"ANALOG", "USB-C", NULL};
static const char *const OPT_MUTE[] = {"off", "on", NULL};

typedef struct {
    const char *key;
    float min, max, def;
    int integer;                /* rounded to whole numbers */
    const char *const *options; /* enum: index, or option name accepted */
} pdef_t;

/* Kept equal to module.json by scripts/metadata.py. */
static const pdef_t PDEF[P_COUNT] = {
    [P_MIX] = {"mix", 0, 1, 1, 0, NULL},
    [P_AN_LEVEL] = {"an_level", 0, 1, 1, 0, NULL},
    [P_USB_LEVEL] = {"usb_level", 0, 1, 1, 0, NULL},
    [P_SOURCE] = {"source", 0, 1, SRC_ANALOG, 1, OPT_SOURCE},
    [P_AN_GAIN] = {"an_gain", -24, 24, 0, 0, NULL},
    [P_USB_GAIN] = {"usb_gain", -24, 24, 0, 0, NULL},
    [P_DUCK] = {"duck", 0, 1, 0, 0, NULL},
    [P_MUTE] = {"mute", 0, 1, 0, 1, OPT_MUTE},
    [P_AN_HPF] = {"an_hpf", 20, 1000, 20, 1, NULL},
    [P_USB_HPF] = {"usb_hpf", 20, 1000, 20, 1, NULL},
    [P_AN_LPF] = {"an_lpf", 1000, 20000, 20000, 1, NULL},
    [P_USB_LPF] = {"usb_lpf", 1000, 20000, 20000, 1, NULL},
    [P_AN_PAN] = {"an_pan", -100, 100, 0, 1, NULL},
    [P_USB_PAN] = {"usb_pan", -100, 100, 0, 1, NULL},
    [P_WIDTH] = {"width", 0, 200, 100, 1, NULL},
};

/* One input channel strip (ANALOG or USB). Both run every block, so the
 * filters are warm when SOURCE crossfades to the other one. */
typedef struct {
    float hp_ic1[2], hp_ic2[2], lp_ic1[2], lp_ic2[2];
    float gain_s, pl_s, pr_s;  /* smoothed: level * gain, balance L/R */
    float hp_hz_s, lp_hz_s;    /* smoothed cutoffs */
} strip_t;

typedef struct {
    _Atomic float p[P_COUNT];
    /* ---- written by process, read by get_param ---- */
    _Atomic float peak_hold;   /* input peak since the last read, linear */
    _Atomic int status;
    /* ---- process-only state ---- */
    strip_t strip[2];
    float w_s;                 /* SOURCE weight, 0 = ANALOG .. 1 = USB */
    float ext_s;               /* MIX * mute * guard */
    float width_s;
    float duck_env;
    float quiet_s;             /* seconds the input has been below NOSIG_DB */
    int pooled;
} lim_t;

static lim_t g_pool[LIM_POOL];
static atomic_int g_pool_used[LIM_POOL];
static float g_sr = 44100.0f;
/* Copied out of the host struct, never kept as a pointer to it: each chain
 * instance hands over its own copy, and the one that loaded us last may be
 * freed while another slot still runs this effect. Every copy points at the
 * same mailbox. */
static uint8_t *volatile g_mailbox;
static volatile int g_audio_in_offset;

/* ---- jack state (feedback guard) ---------------------------------------- */
static const volatile uint8_t *g_ctrl;
static int g_ctrl_tries;

static void ctrl_attach(void)
{
    if (g_ctrl || g_ctrl_tries > 8) return;
    ++g_ctrl_tries;
    const int fd = open("/dev/shm" SHM_SHADOW_CONTROL, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size >= CTRL_MAP_BYTES) {
        void *m = mmap(NULL, CTRL_MAP_BYTES, PROT_READ, MAP_SHARED, fd, 0);
        if (m != MAP_FAILED) g_ctrl = m;
    }
    close(fd);
}

/* Built-in speaker playing and nothing in the line-in jack: the analog input
 * is the built-in mic hearing the speaker. Unknown (no shim) = no risk. */
static int mic_feedback_risk(void)
{
    const volatile uint8_t *c = g_ctrl;
    if (!c) return 0;
    return c[offsetof(shadow_control_t, speaker_active)] != 0 &&
           c[offsetof(shadow_control_t, line_in_connected)] == 0;
}

static inline float rd(_Atomic float *x) { return atomic_load_explicit(x, memory_order_relaxed); }
static inline void wr(_Atomic float *x, float v) { atomic_store_explicit(x, v, memory_order_relaxed); }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float db_to_lin(float db) { return expf(db * 0.11512925f); }
static inline float coef_ms(float ms, int frames) { return 1.0f - expf(-(float)frames / (ms * 0.001f * g_sr)); }

static int param_index(const char *key)
{
    for (int i = 0; i < P_COUNT; ++i)
        if (!strcmp(key, PDEF[i].key)) return i;
    return -1;
}

static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') ++s;
    return s;
}

/* A number string ("0.5", "-6", "1000"), or an enum's option name. */
static int parse_value(int idx, const char *s, float *out)
{
    s = skip_ws(s);
    if (PDEF[idx].options) {
        for (int o = 0; PDEF[idx].options[o]; ++o) {
            const size_t n = strlen(PDEF[idx].options[o]);
            if (!strncmp(s, PDEF[idx].options[o], n) && !*skip_ws(s + n)) { *out = (float)o; return 1; }
        }
    }
    char *end;
    const float v = strtof(s, &end);
    if (end == s || !isfinite(v) || *skip_ws(end)) return 0;
    *out = v;
    return 1;
}

static void apply(lim_t *in, int idx, const char *val)
{
    float v;
    if (!parse_value(idx, val, &v)) return;
    v = clampf(v, PDEF[idx].min, PDEF[idx].max);
    if (PDEF[idx].integer) v = floorf(v + 0.5f);
    wr(&in->p[idx], v);
}

/* state: {"v":1,"mix":1,...}. Each known key is looked up on its own; a
 * missing key keeps its value. Numbers only (enums as their index). */
static void restore(lim_t *in, const char *json)
{
    for (int i = 0; i < P_COUNT; ++i) {
        char pat[24];
        snprintf(pat, sizeof(pat), "\"%s\"", PDEF[i].key);
        const char *p = strstr(json, pat);
        if (!p) continue;
        p = skip_ws(p + strlen(pat));
        if (*p++ != ':') continue;
        p = skip_ws(p);
        char num[24];
        size_t n = 0;
        while (n + 1 < sizeof(num) && *p && *p != ',' && *p != '}' && *p != ' ') num[n++] = *p++;
        num[n] = 0;
        apply(in, i, num);
    }
}

/* ---- targets --------------------------------------------------------------- */

typedef struct { float gain, pl, pr, hp, lp; } strip_target_t;

static strip_target_t strip_target(lim_t *in, int k)
{
    const int lvl = k ? P_USB_LEVEL : P_AN_LEVEL, gn = k ? P_USB_GAIN : P_AN_GAIN;
    const int hp = k ? P_USB_HPF : P_AN_HPF, lp = k ? P_USB_LPF : P_AN_LPF, pan = k ? P_USB_PAN : P_AN_PAN;
    const float p = rd(&in->p[pan]) * 0.01f;
    strip_target_t t = {
        .gain = rd(&in->p[lvl]) * db_to_lin(rd(&in->p[gn])),
        .pl = p > 0.0f ? 1.0f - p : 1.0f,
        .pr = p < 0.0f ? 1.0f + p : 1.0f,
        .hp = rd(&in->p[hp]),
        .lp = rd(&in->p[lp]),
    };
    return t;
}

/* MIX, unless muted or guarded (or there is no input to read). */
static float ext_target(lim_t *in, int *muted, int *guard)
{
    *muted = rd(&in->p[P_MUTE]) > 0.5f;
    *guard = (int)rd(&in->p[P_SOURCE]) == SRC_ANALOG && mic_feedback_risk();
    return (*muted || *guard || !g_mailbox) ? 0.0f : rd(&in->p[P_MIX]);
}

/* Jump the smoothed values to their targets (create / preset restore: a
 * reloaded set must not glide in from the defaults). */
static void snap_smoothed(lim_t *in)
{
    for (int k = 0; k < 2; ++k) {
        const strip_target_t t = strip_target(in, k);
        in->strip[k].gain_s = t.gain;
        in->strip[k].pl_s = t.pl;
        in->strip[k].pr_s = t.pr;
        in->strip[k].hp_hz_s = t.hp;
        in->strip[k].lp_hz_s = t.lp;
    }
    in->w_s = rd(&in->p[P_SOURCE]);
    int muted, guard;
    in->ext_s = ext_target(in, &muted, &guard);
    in->width_s = rd(&in->p[P_WIDTH]) * 0.01f;
}

/* ---- instance lifecycle ---------------------------------------------------- */

static void init_instance(lim_t *in, int pooled)
{
    memset(in, 0, sizeof(*in));
    for (int i = 0; i < P_COUNT; ++i) wr(&in->p[i], PDEF[i].def);
    in->pooled = pooled;
    snap_smoothed(in);
}

static void *create(const char *module_dir, const char *config_json)
{
    (void)module_dir;
    ctrl_attach(); /* a later chance if the shim had not created it at init */
    lim_t *in = NULL;
    for (int i = 0; i < LIM_POOL && !in; ++i) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&g_pool_used[i], &expected, 1)) {
            in = &g_pool[i];
            init_instance(in, 1);
        }
    }
    if (!in) { /* more than 64 at once: a small calloc beats a missing effect */
        in = calloc(1, sizeof(*in));
        if (!in) return NULL;
        init_instance(in, 0);
    }
    if (config_json && *config_json) { restore(in, config_json); snap_smoothed(in); }
    return in;
}

static void destroy(void *ptr)
{
    lim_t *in = ptr;
    if (!in) return;
    if (in->pooled) atomic_store(&g_pool_used[in - g_pool], 0);
    else free(in);
}

static void set_param(void *ptr, const char *key, const char *val)
{
    lim_t *in = ptr;
    if (!in || !key || !val) return;
    if (!strcmp(key, "state")) { restore(in, val); snap_smoothed(in); return; }
    const int idx = param_index(key);
    if (idx >= 0) apply(in, idx, val);
}

static int copy_out(char *buf, int size, const char *text)
{
    const size_t n = strlen(text);
    if (n >= (size_t)size) { buf[0] = 0; return -1; }
    memcpy(buf, text, n + 1);
    return (int)n;
}

static int fmt_param(char *buf, int size, int idx, float v)
{
    if (PDEF[idx].integer) return snprintf(buf, size, "%d", (int)v);
    return snprintf(buf, size, "%.4f", (double)v);
}

static int get_param(void *ptr, const char *key, char *buf, int size)
{
    lim_t *in = ptr;
    if (!in || !key || !buf || size <= 0) return -1;
    int n;
    const int idx = param_index(key);
    if (idx >= 0) n = fmt_param(buf, size, idx, rd(&in->p[idx]));
    else if (!strcmp(key, "in_level")) {
        /* Peak-hold between reads, so a 4 Hz poll still sees every peak.
         * 20*log10 via logf: log10f binds a glibc newer than Move's. */
        const float pk = atomic_exchange_explicit(&in->peak_hold, 0.0f, memory_order_relaxed);
        const float db = pk > 0.001f ? 8.6858896f * logf(pk) : -60.0f;
        n = snprintf(buf, size, "%.1f", (double)clampf(db, -60.0f, 0.0f));
    } else if (!strcmp(key, "status")) n = snprintf(buf, size, "%d", atomic_load_explicit(&in->status, memory_order_relaxed));
    else if (!strcmp(key, "name")) return copy_out(buf, size, "Line In Mixer");
    else if (!strcmp(key, "chain_params")) return copy_out(buf, size, lim_chain_params);
    else if (!strcmp(key, "ui_hierarchy")) return copy_out(buf, size, lim_ui_hierarchy);
    else if (!strcmp(key, "state")) {
        n = snprintf(buf, size, "{\"v\":1");
        for (int i = 0; i < P_COUNT && n > 0 && n < size; ++i) {
            n += snprintf(buf + n, size - n, ",\"%s\":", PDEF[i].key);
            if (n >= size) break;
            n += fmt_param(buf + n, size - n, i, rd(&in->p[i]));
        }
        if (n > 0 && n < size) n += snprintf(buf + n, size - n, "}");
    } else { buf[0] = 0; return -1; }
    if (n < 0 || n >= size) { buf[0] = 0; return -1; }
    return n;
}

/* ---- DSP ------------------------------------------------------------------- */

/* Butterworth (k = sqrt 2) TPT state-variable filter (Zavalishin). */
typedef struct { float a1, a2, a3; } svf_t;
#define SVF_K 1.41421356f

static svf_t svf_coefs(float hz)
{
    const float g = tanf(3.14159265f * clampf(hz, 10.0f, 0.45f * g_sr) / g_sr);
    svf_t c;
    c.a1 = 1.0f / (1.0f + g * (g + SVF_K));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
    return c;
}

/* One sample; returns the low-pass, writes the high-pass. */
static inline float svf_run(const svf_t *c, float *ic1, float *ic2, float x, float *hp)
{
    const float v3 = x - *ic2;
    const float v1 = c->a1 * *ic1 + c->a2 * v3;
    const float v2 = *ic2 + c->a2 * *ic1 + c->a3 * v3;
    *ic1 = 2.0f * v1 - *ic1;
    *ic2 = 2.0f * v2 - *ic2;
    *hp = x - SVF_K * v1 - v2;
    return v2;
}

static inline float flush(float v) { return fabsf(v) < 1e-15f ? 0.0f : v; }

/* One-pole glide that LANDS: within eps it snaps to the target, so a gain
 * that was turned to 0 becomes exactly 0 (bit-exact internal) instead of
 * creeping toward it for ever. 1e-4 = -80 dB, ~60 blocks (0.18 s) at 20 ms. */
#define GAIN_EPS 1e-4f
#define HZ_EPS 0.5f
static inline float glide(float cur, float target, float a, float eps)
{
    const float d = target - cur;
    return fabsf(d) < eps ? target : cur + a * d;
}

static void process(void *ptr, int16_t *io, int frames)
{
    lim_t *in = ptr;
    if (!in || !io || frames <= 0) return;

    const int16_t *src = NULL;
    uint8_t *mb = g_mailbox;
    if (mb && g_audio_in_offset > 0)
        src = (const int16_t *)(mb + g_audio_in_offset);
    if (frames > MOVE_FRAMES_PER_BLOCK) frames = MOVE_FRAMES_PER_BLOCK; /* the mailbox holds one block */

    const int source = (int)rd(&in->p[P_SOURCE]);
    int muted, guard;
    const float ext_t = src ? ext_target(in, &muted, &guard) : 0.0f;
    if (!src) muted = guard = 0;
    const float duck = rd(&in->p[P_DUCK]);
    const float a = coef_ms(SMOOTH_MS, frames);
    const float inv = 1.0f / (float)frames;

    /* Per-block targets -> per-sample linear ramps (no zipper noise). */
    float g0[2], gd[2], pl0[2], pld[2], pr0[2], prd[2];
    svf_t hpc[2], lpc[2];
    int hp_on[2], lp_on[2];
    for (int k = 0; k < 2; ++k) {
        strip_t *s = &in->strip[k];
        const strip_target_t t = strip_target(in, k);
        g0[k] = s->gain_s; s->gain_s = glide(s->gain_s, t.gain, a, GAIN_EPS); gd[k] = (s->gain_s - g0[k]) * inv;
        pl0[k] = s->pl_s; s->pl_s = glide(s->pl_s, t.pl, a, GAIN_EPS); pld[k] = (s->pl_s - pl0[k]) * inv;
        pr0[k] = s->pr_s; s->pr_s = glide(s->pr_s, t.pr, a, GAIN_EPS); prd[k] = (s->pr_s - pr0[k]) * inv;
        s->hp_hz_s = glide(s->hp_hz_s, t.hp, a, HZ_EPS);
        s->lp_hz_s = glide(s->lp_hz_s, t.lp, a, HZ_EPS);
        hp_on[k] = s->hp_hz_s > HPF_OFF_HZ + 0.5f;
        lp_on[k] = s->lp_hz_s < LPF_OFF_HZ - 0.5f;
        hpc[k] = svf_coefs(s->hp_hz_s);
        lpc[k] = svf_coefs(s->lp_hz_s);
    }
    const float w0 = in->w_s;
    in->w_s = glide(in->w_s, (float)source, coef_ms(SOURCE_XFADE_MS, frames), GAIN_EPS);
    const float wd = (in->w_s - w0) * inv;
    const float e0 = in->ext_s;
    in->ext_s = glide(in->ext_s, ext_t, a, GAIN_EPS);
    const float ed = (in->ext_s - e0) * inv;
    const float wi0 = in->width_s;
    in->width_s = glide(in->width_s, rd(&in->p[P_WIDTH]) * 0.01f, a, GAIN_EPS);
    const float wid = (in->width_s - wi0) * inv;
    const float duck_att = 1.0f - expf(-1.0f / (DUCK_ATTACK_MS * 0.001f * g_sr));
    const float duck_rel = 1.0f - expf(-1.0f / (DUCK_RELEASE_MS * 0.001f * g_sr));

    const int active = e0 > 0.0f || in->ext_s > 0.0f;
    float peak = 0.0f;
    for (int i = 0; i < frames; ++i) {
        const float il = io[2 * i] * (1.0f / 32768.0f), ir = io[2 * i + 1] * (1.0f / 32768.0f);
        /* Duck envelope follows the internal signal even while ext is off,
         * so turning MIX up mid-phrase does not start un-ducked. */
        const float lvl = fmaxf(fabsf(il), fabsf(ir));
        in->duck_env += (lvl > in->duck_env ? duck_att : duck_rel) * (lvl - in->duck_env);

        float xl = 0.0f, xr = 0.0f;
        if (src) {
            xl = src[2 * i] * (1.0f / 32768.0f);
            xr = src[2 * i + 1] * (1.0f / 32768.0f);
            peak = fmaxf(peak, fmaxf(fabsf(xl), fabsf(xr)));
        }
        float el = 0.0f, er = 0.0f;
        for (int k = 0; k < 2; ++k) {
            strip_t *s = &in->strip[k];
            float yl = xl, yr = xr, h;
            /* Filters always run (warm for a SOURCE / cutoff change); their
             * output is used only when on. */
            float lpl, lpr;
            svf_run(&hpc[k], &s->hp_ic1[0], &s->hp_ic2[0], yl, &h); if (hp_on[k]) yl = h;
            svf_run(&hpc[k], &s->hp_ic1[1], &s->hp_ic2[1], yr, &h); if (hp_on[k]) yr = h;
            lpl = svf_run(&lpc[k], &s->lp_ic1[0], &s->lp_ic2[0], yl, &h); if (lp_on[k]) yl = lpl;
            lpr = svf_run(&lpc[k], &s->lp_ic1[1], &s->lp_ic2[1], yr, &h); if (lp_on[k]) yr = lpr;
            const float g = g0[k] + gd[k] * (float)i;
            const float wk = k ? (w0 + wd * (float)i) : 1.0f - (w0 + wd * (float)i);
            el += yl * g * (pl0[k] + pld[k] * (float)i) * wk;
            er += yr * g * (pr0[k] + prd[k] * (float)i) * wk;
        }
        if (!active) continue; /* internal untouched, bit for bit */
        const float width = wi0 + wid * (float)i;
        const float mid = 0.5f * (el + er), side = 0.5f * (el - er) * width;
        const float dg = 1.0f - duck * fminf(1.0f, in->duck_env * (1.0f / DUCK_FULL));
        const float e = (e0 + ed * (float)i) * dg;
        const float ol = il + (mid + side) * e, orr = ir + (mid - side) * e;
        io[2 * i] = (int16_t)lrintf(clampf(ol * 32768.0f, -32768.0f, 32767.0f));
        io[2 * i + 1] = (int16_t)lrintf(clampf(orr * 32768.0f, -32768.0f, 32767.0f));
    }
    for (int k = 0; k < 2; ++k) {
        strip_t *s = &in->strip[k];
        for (int c = 0; c < 2; ++c) {
            s->hp_ic1[c] = flush(s->hp_ic1[c]); s->hp_ic2[c] = flush(s->hp_ic2[c]);
            s->lp_ic1[c] = flush(s->lp_ic1[c]); s->lp_ic2[c] = flush(s->lp_ic2[c]);
        }
    }
    in->duck_env = flush(in->duck_env);

    /* ---- meters / status ---- */
    if (peak > rd(&in->peak_hold)) wr(&in->peak_hold, peak);
    const float nosig_lin = 0.00031623f; /* -70 dBFS */
    in->quiet_s = peak < nosig_lin ? fminf(in->quiet_s + (float)frames / g_sr, 60.0f) : 0.0f;
    int st = ST_OK;
    if (guard) st = ST_FBGUARD;
    else if (muted) st = ST_MUTED;
    else if (!src || in->quiet_s >= NOSIG_SECONDS) st = ST_NOSIG;
    atomic_store_explicit(&in->status, st, memory_order_relaxed);
}

/* ---- MIDI CC ------------------------------------------------------------------
 * Fixed, absolute CCs on the slot's own channel, in knob order:
 *   CC 20-27  MIX page   (MIX, AN LVL, USB LVL, SOURCE, AN GN, USB GN, DUCK, MUTE)
 *   CC 28-34  TONE page  knobs 2-8 (AN HPF, USB HPF, AN LPF, USB LPF, AN PAN,
 *                         USB PAN, WIDTH)  - knob 1 there is MIX = CC 20
 * 0..127 spans the parameter's range; enums split it evenly (SOURCE: 0-63
 * ANALOG, 64-127 USB-C; MUTE: 0-63 off, 64-127 on). CC 20-34 are not used
 * by Move's own controls. Only this slot's MIDI counts: FX_BROADCAST (every
 * other track's notes) and HOST (chain-generated) are ignored. */
#define CC_FIRST 20

static void on_midi(void *ptr, const uint8_t *msg, int len, int source)
{
    lim_t *in = ptr;
    if (!in || !msg || len < 3) return;
    if (source == MOVE_MIDI_SOURCE_FX_BROADCAST || source == MOVE_MIDI_SOURCE_HOST) return;
    if ((msg[0] & 0xF0) != 0xB0) return;
    const int idx = (int)msg[1] - CC_FIRST;
    if (idx < 0 || idx >= P_COUNT) return;
    const float t = (float)(msg[2] & 0x7F) / 127.0f;
    const pdef_t *d = &PDEF[idx];
    float v;
    if (d->options) {
        int n = 0;
        while (d->options[n]) ++n;
        v = floorf(t * (float)n * 0.99999f);
    } else {
        v = d->min + t * (d->max - d->min);
        if (d->integer) v = floorf(v + 0.5f);
    }
    wr(&in->p[idx], clampf(v, d->min, d->max));
}

static audio_fx_api_v2_t g_api = {
    .api_version = AUDIO_FX_API_VERSION_2,
    .create_instance = create,
    .destroy_instance = destroy,
    .process_block = process,
    .set_param = set_param,
    .get_param = get_param,
    .on_midi = on_midi,
};

EXPORT audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host)
{
    if (host) {
        if (host->sample_rate > 0) g_sr = (float)host->sample_rate;
        if (host->mapped_memory && host->audio_in_offset > 0) {
            g_audio_in_offset = host->audio_in_offset;
            g_mailbox = host->mapped_memory;
        }
    }
    ctrl_attach();
    return &g_api;
}

/* The entry point the chain host actually calls for an audio FX's MIDI
 * (chain_fx_load.c dlsyms it; the struct field alone is not enough). */
EXPORT void move_audio_fx_on_midi(void *instance, const uint8_t *msg, int len, int source)
{
    on_midi(instance, msg, len, source);
}
