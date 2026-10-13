/* Line In Mixer through the real Schwung audio FX v2 ABI (dlopen + the
 * exported move_audio_fx_init_v2), with a fake SPI mailbox standing in for
 * Move's audio input.
 *
 *   test_fx <line-in-mixer.so>          functional tests
 *   test_fx <line-in-mixer.so> cpu      + timing
 *   test_fx <line-in-mixer.so> guard    feedback guard (creates a FAKE
 *                                       /dev/shm/schwung-control: desktop only,
 *                                       refuses to run where one exists)
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiro Ikezawa
 */
#include "host/audio_fx_api_v2.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define N MOVE_FRAMES_PER_BLOCK
#define SR 44100.0

static audio_fx_api_v2_t *api;
static uint8_t mailbox[4096];
static int failures;
static void *g_so;

static void check(int cond, const char *msg)
{
    printf("%s %s\n", cond ? "  ok " : "FAIL ", msg);
    if (!cond) ++failures;
}

static int16_t *ain(void) { return (int16_t *)(mailbox + MOVE_AUDIO_IN_OFFSET); }

static void set(void *in, const char *k, const char *v) { api->set_param(in, k, v); }

static float getf(void *in, const char *k)
{
    char b[4096];
    return api->get_param(in, k, b, sizeof(b)) >= 0 ? strtof(b, NULL) : NAN;
}

/* Generators: per-sample stereo values in [-1, 1]. */
typedef struct { double fl, fr, al, ar; } tone_t; /* freq 0 = DC of amplitude a */
static double tone(double f, double a, long n) { return f == 0 ? a : a * sin(2 * M_PI * f * n / SR); }

static int16_t q(double v) { v = v * 32768.0; return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrint(v)); }

/* Run `blocks` blocks: internal = it, input = xt. Returns the RMS of
 * (out - internal) per channel over the last `measure` blocks, and the max
 * |out - internal - expect| if expect is given. */
typedef struct { double rms_l, rms_r, max_int_dev; long t; } run_t;

static run_t run(void *in, tone_t it, tone_t xt, int blocks, int measure, long t0)
{
    run_t r = {0, 0, 0, t0};
    int16_t io[2 * N], ref[2 * N];
    long cnt = 0;
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < N; ++i, ++r.t) {
            ref[2 * i] = io[2 * i] = q(tone(it.fl, it.al, r.t));
            ref[2 * i + 1] = io[2 * i + 1] = q(tone(it.fr, it.ar, r.t));
            ain()[2 * i] = q(tone(xt.fl, xt.al, r.t));
            ain()[2 * i + 1] = q(tone(xt.fr, xt.ar, r.t));
        }
        api->process_block(in, io, N);
        if (b >= blocks - measure)
            for (int i = 0; i < N; ++i) {
                const double dl = (io[2 * i] - ref[2 * i]) / 32768.0, dr = (io[2 * i + 1] - ref[2 * i + 1]) / 32768.0;
                r.rms_l += dl * dl;
                r.rms_r += dr * dr;
                ++cnt;
            }
    }
    r.rms_l = sqrt(r.rms_l / (cnt ? cnt : 1));
    r.rms_r = sqrt(r.rms_r / (cnt ? cnt : 1));
    return r;
}

static double db(double v) { return 20 * log10(v > 1e-12 ? v : 1e-12); }

/* Every output sample equal to the internal input, bit for bit. */
static int exact_internal(void *in, tone_t it, tone_t xt, int blocks)
{
    int16_t io[2 * N], ref[2 * N];
    int ok = 1;
    long t = 0;
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < N; ++i, ++t) {
            ref[2 * i] = io[2 * i] = q(tone(it.fl, it.al, t));
            ref[2 * i + 1] = io[2 * i + 1] = q(tone(it.fr, it.ar, t));
            ain()[2 * i] = q(tone(xt.fl, xt.al, t));
            ain()[2 * i + 1] = q(tone(xt.fr, xt.ar, t));
        }
        api->process_block(in, io, N);
        if (b >= blocks / 2 && memcmp(io, ref, sizeof(io))) ok = 0;
    }
    return ok;
}

static void *fresh(void) { return api->create_instance(".", NULL); }

static void functional(void)
{
    const tone_t silent = {0, 0, 0, 0}, inner = {220, 330, 0.3, 0.3}, ext1k = {1000, 1000, 0.25, 0.25};
    char msg[256];

    /* 1. defaults: out = internal + input (MIX 100 %, levels 100 %, 0 dB, filters off) */
    void *in = fresh();
    run_t r = run(in, inner, ext1k, 40, 20, 0);
    snprintf(msg, sizeof msg, "defaults add the input at unity (ext rms %.4f / %.4f, want %.4f)", r.rms_l, r.rms_r, 0.25 / sqrt(2));
    check(fabs(r.rms_l - 0.25 / sqrt(2)) < 0.002 && fabs(r.rms_r - 0.25 / sqrt(2)) < 0.002, msg);
    api->destroy_instance(in);

    /* 2. internal untouched: MIX 0, MUTE, silent input, any other settings */
    in = fresh();
    set(in, "mix", "0");
    check(exact_internal(in, inner, ext1k, 200), "MIX 0: output == internal, bit for bit");
    set(in, "mix", "1"); set(in, "mute", "on");
    check(exact_internal(in, inner, ext1k, 200), "MUTE on: output == internal, bit for bit");
    snprintf(msg, sizeof msg, "MUTE on: status MUTED (%g)", getf(in, "status"));
    check(getf(in, "status") == 1, msg);
    set(in, "mute", "off");
    const char *knobs[][2] = {{"an_gain", "24"}, {"usb_gain", "-24"}, {"an_hpf", "500"}, {"an_lpf", "2000"},
                              {"an_pan", "-100"}, {"width", "200"}, {"duck", "1"}, {"source", "USB-C"}};
    for (unsigned k = 0; k < sizeof knobs / sizeof *knobs; ++k) set(in, knobs[k][0], knobs[k][1]);
    check(exact_internal(in, inner, silent, 200), "silent input, every knob moved: output == internal, bit for bit");
    api->destroy_instance(in);

    /* 3. SOURCE picks the strip: USB gain applies only on USB-C */
    in = fresh();
    set(in, "usb_gain", "-6.0206"); set(in, "an_gain", "6");
    set(in, "source", "1");
    r = run(in, silent, ext1k, 200, 20, 0);
    snprintf(msg, sizeof msg, "SOURCE USB-C uses USB GAIN: -6 dB -> %.2f dB", db(r.rms_l * sqrt(2) / 0.25));
    check(fabs(db(r.rms_l * sqrt(2) / 0.25) + 6.02) < 0.1, msg);
    set(in, "source", "ANALOG");
    r = run(in, silent, ext1k, 200, 20, r.t);
    snprintf(msg, sizeof msg, "SOURCE ANALOG uses ANALOG GAIN: +6 dB -> %.2f dB", db(r.rms_l * sqrt(2) / 0.25));
    check(fabs(db(r.rms_l * sqrt(2) / 0.25) - 6.0) < 0.1, msg);
    set(in, "an_gain", "0"); set(in, "an_level", "0.5");
    r = run(in, silent, ext1k, 200, 20, r.t);
    snprintf(msg, sizeof msg, "ANALOG LEVEL 50%% -> %.2f dB", db(r.rms_l * sqrt(2) / 0.25));
    check(fabs(db(r.rms_l * sqrt(2) / 0.25) + 6.02) < 0.1, msg);
    set(in, "an_level", "1"); set(in, "mix", "0.25");
    r = run(in, silent, ext1k, 200, 20, r.t);
    snprintf(msg, sizeof msg, "MIX 25%% -> %.2f dB", db(r.rms_l * sqrt(2) / 0.25));
    check(fabs(db(r.rms_l * sqrt(2) / 0.25) + 12.04) < 0.1, msg);
    api->destroy_instance(in);

    /* 4. filters */
    struct { const char *k, *v; double f, lo, hi; const char *what; } fl[] = {
        {"an_hpf", "500", 40, -99, -30, "ANALOG HPF 500 Hz, 40 Hz tone"},
        {"an_hpf", "100", 2000, -0.2, 0.2, "ANALOG HPF 100 Hz, 2 kHz tone (pass)"},
        {"an_lpf", "1000", 10000, -99, -30, "ANALOG LPF 1 kHz, 10 kHz tone"},
        {"an_lpf", "8000", 200, -0.2, 0.2, "ANALOG LPF 8 kHz, 200 Hz tone (pass)"},
        {"an_hpf", "20", 25, -0.05, 0.05, "ANALOG HPF 20 Hz = off, 25 Hz tone"},
        {"an_lpf", "20000", 19000, -0.05, 0.05, "ANALOG LPF 20000 Hz = off, 19 kHz tone"},
    };
    for (unsigned k = 0; k < sizeof fl / sizeof *fl; ++k) {
        in = fresh();
        set(in, fl[k].k, fl[k].v);
        tone_t x = {fl[k].f, fl[k].f, 0.25, 0.25};
        r = run(in, silent, x, 400, 100, 0);
        const double g = db(r.rms_l * sqrt(2) / 0.25);
        snprintf(msg, sizeof msg, "%s: %.2f dB", fl[k].what, g);
        check(g >= fl[k].lo && g <= fl[k].hi, msg);
        api->destroy_instance(in);
    }
    in = fresh();
    set(in, "usb_lpf", "1000"); /* the other strip's filter must not touch ANALOG */
    r = run(in, silent, (tone_t){10000, 10000, 0.25, 0.25}, 200, 50, 0);
    check(fabs(db(r.rms_l * sqrt(2) / 0.25)) < 0.05, "USB LPF does not affect SOURCE ANALOG");
    api->destroy_instance(in);

    /* 5. pan and width */
    in = fresh();
    set(in, "an_pan", "-100");
    r = run(in, silent, ext1k, 200, 20, 0);
    snprintf(msg, sizeof msg, "ANALOG PAN -100: L %.2f dB, R %.1f dB", db(r.rms_l * sqrt(2) / 0.25), db(r.rms_r * sqrt(2) / 0.25));
    check(fabs(db(r.rms_l * sqrt(2) / 0.25)) < 0.05 && db(r.rms_r) < -80, msg);
    api->destroy_instance(in);
    in = fresh();
    set(in, "width", "0");
    r = run(in, silent, (tone_t){1000, 1000, 0.25, 0}, 200, 20, 0); /* input only on L */
    snprintf(msg, sizeof msg, "WIDTH 0%%: L-only input lands in both at -6 dB (L %.2f, R %.2f dB)",
             db(r.rms_l * sqrt(2) / 0.25), db(r.rms_r * sqrt(2) / 0.25));
    check(fabs(db(r.rms_l * sqrt(2) / 0.25) + 6.02) < 0.1 && fabs(db(r.rms_r * sqrt(2) / 0.25) + 6.02) < 0.1, msg);
    set(in, "width", "100");
    r = run(in, silent, (tone_t){1000, 1000, 0.25, 0}, 200, 20, r.t);
    check(fabs(db(r.rms_l * sqrt(2) / 0.25)) < 0.05 && db(r.rms_r) < -80, "WIDTH 100%: unchanged");
    api->destroy_instance(in);

    /* 6. duck: the internal sound turns the external part down */
    in = fresh();
    set(in, "duck", "1");
    r = run(in, (tone_t){0, 0, 0.5, 0.5}, ext1k, 200, 20, 0); /* loud internal (DC) */
    snprintf(msg, sizeof msg, "DUCK 100%% under a loud internal: ext %.1f dB", db(r.rms_l * sqrt(2) / 0.25));
    check(db(r.rms_l * sqrt(2) / 0.25) < -60, msg);
    set(in, "duck", "0.5");
    r = run(in, (tone_t){0, 0, 0.5, 0.5}, ext1k, 200, 20, r.t);
    snprintf(msg, sizeof msg, "DUCK 50%% under a loud internal: ext %.2f dB (want -6)", db(r.rms_l * sqrt(2) / 0.25));
    check(fabs(db(r.rms_l * sqrt(2) / 0.25) + 6.02) < 0.2, msg);
    set(in, "duck", "1");
    r = run(in, silent, ext1k, 400, 20, r.t); /* internal stops: ext returns */
    check(fabs(db(r.rms_l * sqrt(2) / 0.25)) < 0.1, "DUCK releases when the internal sound stops");
    api->destroy_instance(in);

    /* 7. clicks: MIX 0 -> 1 and a SOURCE switch glide (DC input, no jump) */
    in = fresh();
    set(in, "mix", "0");
    run(in, silent, (tone_t){0, 0, 0.5, 0.5}, 200, 1, 0);
    set(in, "mix", "1");
    int16_t io[2 * N];
    int maxstep = 0, prev = 0;
    for (int b = 0; b < 60; ++b) {
        memset(io, 0, sizeof io);
        for (int i = 0; i < 2 * N; ++i) ain()[i] = q(0.5);
        if (b == 30) set(in, "source", "1"), set(in, "usb_level", "0");
        api->process_block(in, io, N);
        for (int i = 0; i < N; ++i) {
            const int d = abs(io[2 * i] - prev);
            if (d > maxstep) maxstep = d;
            prev = io[2 * i];
        }
    }
    snprintf(msg, sizeof msg, "MIX step and SOURCE switch glide: largest sample step %d (of 16384)", maxstep);
    check(maxstep < 400, msg);
    api->destroy_instance(in);

    /* 8. clipping saturates, never wraps */
    in = fresh();
    r = run(in, (tone_t){0, 0, 0.95, -0.95}, (tone_t){0, 0, 0.95, -0.95}, 20, 1, 0);
    {
        int16_t b[2 * N];
        for (int i = 0; i < N; ++i) { b[2 * i] = 31130; b[2 * i + 1] = -31130; ain()[2 * i] = 31130; ain()[2 * i + 1] = -31130; }
        api->process_block(in, b, N);
        check(b[0] == 32767 && b[1] == -32768, "overload saturates at full scale (no wrap)");
    }
    api->destroy_instance(in);

    /* 9. meters and status */
    in = fresh();
    run(in, silent, ext1k, 20, 1, 0);
    const float lvl = getf(in, "in_level"); /* a read consumes the peak hold */
    snprintf(msg, sizeof msg, "INPUT level reads the input peak: %.1f dB (want -12.0)", lvl);
    check(fabs(lvl + 12.0) < 0.3, msg);
    check(getf(in, "in_level") == -60.0f, "INPUT level peak hold resets on read");
    check(getf(in, "status") == 0, "status OK with signal");
    run(in, silent, silent, (int)(2.1 * SR / N), 1, 0);
    check(getf(in, "status") == 3, "status NO SIG after 2 s of silence");
    run(in, silent, ext1k, 2, 1, 0);
    check(getf(in, "status") == 0, "status back to OK on signal");
    api->destroy_instance(in);

    /* 10. presets: state round trip, partial state, garbage */
    in = fresh();
    const char *vals[][2] = {{"mix", "0.37"}, {"an_level", "0.5"}, {"usb_level", "0.25"}, {"source", "USB-C"},
                             {"an_gain", "-3.5"}, {"usb_gain", "12"}, {"duck", "0.6"}, {"mute", "on"},
                             {"an_hpf", "120"}, {"usb_hpf", "300"}, {"an_lpf", "8000"}, {"usb_lpf", "4500"},
                             {"an_pan", "-40"}, {"usb_pan", "25"}, {"width", "150"}};
    for (unsigned k = 0; k < 15; ++k) set(in, vals[k][0], vals[k][1]);
    char state[16384];
    check(api->get_param(in, "state", state, sizeof state) > 0, "state serialises");
    void *b2 = api->create_instance(".", state);
    int same = 1;
    for (unsigned k = 0; k < 15; ++k) same &= getf(in, vals[k][0]) == getf(b2, vals[k][0]);
    printf("       state: %s\n", state);
    check(same, "create(state) restores all 15 parameters");
    void *b3 = fresh();
    set(b3, "state", state);
    same = 1;
    for (unsigned k = 0; k < 15; ++k) same &= getf(in, vals[k][0]) == getf(b3, vals[k][0]);
    check(same, "set_param(state) restores all 15 parameters");
    set(b3, "state", "{\"v\":1,\"width\":50}");
    check(getf(b3, "width") == 50 && getf(b3, "mix") == getf(in, "mix"), "partial state changes only its keys");
    set(b3, "mix", "nan"); set(b3, "mix", ""); set(b3, "mix", "abc"); set(b3, "source", "BOTH");
    check(getf(b3, "mix") == getf(in, "mix") && getf(b3, "source") == 1, "nan / empty / unknown values are ignored");
    set(b3, "an_hpf", "5"); set(b3, "an_lpf", "99999"); set(b3, "an_gain", "-100");
    check(getf(b3, "an_hpf") == 20 && getf(b3, "an_lpf") == 20000 && getf(b3, "an_gain") == -24, "out of range values clamp");
    char tiny[4];
    check(api->get_param(b3, "chain_params", tiny, sizeof tiny) < 0 && tiny[0] == 0, "too-small buffer: -1 and empty");
    check(api->get_param(b3, "chain_params", state, sizeof state) > 0 && state[0] == '[', "chain_params served");
    check(api->get_param(b3, "nope", state, sizeof state) < 0, "unknown key: -1");
    api->destroy_instance(in); api->destroy_instance(b2); api->destroy_instance(b3);

    /* 11. MIDI CC 20-34 (through the dlsym'd export, as the chain host calls it) */
    {
        typedef void (*midi_fn)(void *, const uint8_t *, int, int);
        midi_fn m = (midi_fn)dlsym(g_so, "move_audio_fx_on_midi");
        check(m != NULL, "exports move_audio_fx_on_midi");
        if (m) {
            in = fresh();
            const uint8_t c1[3] = {0xB3, 20, 0}, c2[3] = {0xB0, 23, 64}, c3[3] = {0xB0, 34, 127},
                          c4[3] = {0xB0, 28, 127}, c5[3] = {0xB0, 27, 127}, c6[3] = {0xB0, 21, 64},
                          c7[3] = {0xB0, 35, 0}, c8[3] = {0xB0, 20, 127};
            m(in, c1, 3, MOVE_MIDI_SOURCE_EXTERNAL);
            m(in, c2, 3, MOVE_MIDI_SOURCE_EXTERNAL);
            m(in, c3, 3, MOVE_MIDI_SOURCE_INTERNAL);
            m(in, c4, 3, MOVE_MIDI_SOURCE_EXTERNAL);
            m(in, c5, 3, MOVE_MIDI_SOURCE_EXTERNAL);
            m(in, c6, 3, MOVE_MIDI_SOURCE_EXTERNAL);
            m(in, c7, 3, MOVE_MIDI_SOURCE_EXTERNAL);
            check(getf(in, "mix") == 0 && getf(in, "source") == 1 && getf(in, "width") == 200 &&
                  getf(in, "an_hpf") == 1000 && getf(in, "mute") == 1 && fabsf(getf(in, "an_level") - 64 / 127.0f) < 1e-3,
                  "CC 20 MIX, 23 SOURCE, 27 MUTE, 21 AN LVL, 28 AN HPF, 34 WIDTH");
            m(in, c8, 3, MOVE_MIDI_SOURCE_FX_BROADCAST);
            m(in, c8, 3, MOVE_MIDI_SOURCE_HOST);
            check(getf(in, "mix") == 0, "CCs from other tracks (FX_BROADCAST) and the host are ignored");
            api->destroy_instance(in);
        }
    }

    /* 12. many instances (pool + calloc fallback) */
    void *many[80];
    int ok = 1;
    for (int k = 0; k < 80; ++k) ok &= (many[k] = fresh()) != NULL;
    for (int k = 0; k < 80; ++k) api->destroy_instance(many[k]);
    check(ok, "80 instances (pool of 64 + fallback)");
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <line-in-mixer.so> [cpu|guard]\n", argv[0]); return 2; }
    const int guard = argc > 2 && !strcmp(argv[2], "guard");
    if (guard) {
        if (access("/dev/shm/schwung-control", F_OK) == 0) {
            fprintf(stderr, "a real /dev/shm/schwung-control exists (on a Move?): guard test refused\n");
            return 2;
        }
        const int fd = shm_open("/schwung-control", O_CREAT | O_RDWR, 0600);
        if (fd < 0 || ftruncate(fd, 512) != 0) { perror("shm"); return 2; }
        uint8_t *c = mmap(NULL, 512, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        c[58] = 1; c[59] = 0; /* speaker on, nothing in the line-in jack */
        void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        audio_fx_init_v2_fn init = h ? (audio_fx_init_v2_fn)dlsym(h, AUDIO_FX_INIT_V2_SYMBOL) : NULL;
        host_api_v1_t host = {.api_version = 1, .sample_rate = 44100, .frames_per_block = N,
                              .mapped_memory = mailbox, .audio_out_offset = MOVE_AUDIO_OUT_OFFSET,
                              .audio_in_offset = MOVE_AUDIO_IN_OFFSET};
        api = init ? init(&host) : NULL;
        if (!api) { shm_unlink("/schwung-control"); fprintf(stderr, "load failed\n"); return 1; }
        const tone_t inner = {220, 330, 0.3, 0.3}, ext1k = {1000, 1000, 0.25, 0.25};
        void *in = fresh();
        check(exact_internal(in, inner, ext1k, 200), "FB GUARD (speaker, no line-in cable, ANALOG): internal only");
        check(getf(in, "status") == 2, "status FB GUARD");
        set(in, "source", "USB-C");
        run_t r = run(in, (tone_t){0, 0, 0, 0}, ext1k, 200, 20, 0);
        check(fabs(r.rms_l - 0.25 / sqrt(2)) < 0.002 && getf(in, "status") == 0, "USB-C is not guarded (not the mic)");
        set(in, "source", "ANALOG");
        c[59] = 1; /* cable plugged in */
        r = run(in, (tone_t){0, 0, 0, 0}, ext1k, 200, 20, 0);
        check(fabs(r.rms_l - 0.25 / sqrt(2)) < 0.002 && getf(in, "status") == 0, "cable plugged in: ANALOG passes");
        c[59] = 0; c[58] = 0; /* headphones */
        r = run(in, (tone_t){0, 0, 0, 0}, ext1k, 200, 20, 0);
        check(fabs(r.rms_l - 0.25 / sqrt(2)) < 0.002, "headphones (speaker off): mic passes");
        api->destroy_instance(in);
        shm_unlink("/schwung-control");
        printf(failures ? "FAILED: %d\n" : "PASS (guard)\n", failures);
        return failures ? 1 : 0;
    }

    void *h = g_so = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    audio_fx_init_v2_fn init = (audio_fx_init_v2_fn)dlsym(h, AUDIO_FX_INIT_V2_SYMBOL);
    check(init != NULL, "exports move_audio_fx_init_v2");
    if (!init) return 1;

    /* Host without a mailbox first (as early as possible: g_mailbox is only
     * ever set, never cleared). */
    host_api_v1_t bare = {.api_version = 1, .sample_rate = 44100, .frames_per_block = N};
    api = init(&bare);
    check(api && api->api_version == AUDIO_FX_API_VERSION_2, "api_version 2");
    {
        void *in = fresh();
        int16_t io[2 * N], ref[2 * N];
        for (int i = 0; i < 2 * N; ++i) ref[i] = io[i] = (int16_t)(i * 97 - 9000);
        api->process_block(in, io, N);
        check(!memcmp(io, ref, sizeof io), "no mailbox: internal passes untouched");
        check(getf(in, "status") == 3, "no mailbox: status NO SIG");
        api->destroy_instance(in);
    }

    host_api_v1_t host = {.api_version = 1, .sample_rate = 44100, .frames_per_block = N,
                          .mapped_memory = mailbox, .audio_out_offset = MOVE_AUDIO_OUT_OFFSET,
                          .audio_in_offset = MOVE_AUDIO_IN_OFFSET};
    api = init(&host);
    functional();

    if (argc > 2 && !strcmp(argv[2], "cpu")) {
        void *in = fresh();
        set(in, "an_hpf", "80"); set(in, "an_lpf", "12000"); set(in, "duck", "0.5"); set(in, "width", "120");
        int16_t io[2 * N];
        for (int i = 0; i < 2 * N; ++i) { io[i] = (int16_t)(i * 37); ain()[i] = (int16_t)(i * 53 - 5000); }
        const int blocks = 20000;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int b = 0; b < blocks; ++b) api->process_block(in, io, N);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        const double us = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1e3 / blocks;
        printf("  cpu: %.2f us per 128-frame block (%.2f %% of the 2902 us block)\n", us, us / 2902.0 * 100);
        api->destroy_instance(in);
    }
    printf(failures ? "FAILED: %d\n" : "PASS\n", failures);
    return failures ? 1 : 0;
}
