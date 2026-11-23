/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* The module as Schwung loads it: dsp.so opened, move_plugin_init_v2 given a host table, an instance made over a
 * module folder (its Modules: the test modules, a broken file, a second copy), then every synth: its contract
 * (written out for tests/check_contract.py), its factory sounds played, every parameter set and read back, its cutoff,
 * its state saved and loaded, its cost. Then a folder with no modules, and two instances of one synth.
 *
 *   test_dsp MODULE_DIR OUT_DIR        (MODULE_DIR holds dsp.so and Modules/)
 *   test_dsp --one MODULE_DIR          (each synth plays: for a folder of one module) */
#include <dlfcn.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "schwung_api.h"

static int fails;
static int have_ppg;                             /* the PPG module in Modules (its file stays out of the repositories:
                                                  * PPG's waves and programs are PPG's) */
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static void h_log(const char *m) { printf("  log: %s\n", m); }
static float h_bpm(void) { return 120.0f; }
static host_api_v1_t HOST = {.api_version = 1, .sample_rate = MOVE_SAMPLE_RATE,
                             .frames_per_block = MOVE_FRAMES_PER_BLOCK, .log = h_log, .get_bpm = h_bpm};
static plugin_api_v2_t *api;

#define BIG (64 * 1024)
static char buf[BIG];

static const char *get(void *in, const char *key)
{
    int n = api->get_param(in, key, buf, sizeof buf);
    return n < 0 ? 0 : buf;
}
static void set(void *in, const char *key, const char *fmt, ...)
{
    char v[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v, sizeof v, fmt, ap);
    va_end(ap);
    api->set_param(in, key, v);
}
static void note(void *in, int on, int n, int vel)
{
    uint8_t m[3] = {(uint8_t)(on ? 0x90 : 0x80), (uint8_t)n, (uint8_t)vel};
    api->on_midi(in, m, 3, MOVE_MIDI_SOURCE_INTERNAL);
}
/* the RMS of blocks of output (full scale 1) */
static double play(void *in, int blocks)
{
    int16_t out[2 * MOVE_FRAMES_PER_BLOCK];
    double sum = 0;
    for (int b = 0; b < blocks; b++) {
        api->render_block(in, out, MOVE_FRAMES_PER_BLOCK);
        for (int i = 0; i < 2 * MOVE_FRAMES_PER_BLOCK; i++)
            sum += (double)out[i] * out[i];
    }
    return sqrt(sum / (2.0 * MOVE_FRAMES_PER_BLOCK * blocks)) / 32768.0;
}
static double hit(void *in, int n)              /* a note half a second, its release */
{
    double r;
    note(in, 1, n, 100);
    r = play(in, 172);
    note(in, 0, n, 0);
    play(in, 200);
    return r;
}

/* ---- a JSON array of objects, as Schwung's contract has it: each object's text */
typedef struct { const char *at; int len; } obj_t;
static int objects(const char *s, obj_t *o, int cap)
{
    int n = 0, depth = 0, str = 0;
    const char *start = 0;
    for (; *s; s++) {
        if (str) {
            if (*s == '\\' && s[1])
                s++;
            else if (*s == '"')
                str = 0;
            continue;
        }
        if (*s == '"')
            str = 1;
        else if (*s == '{' && depth++ == 0)
            start = s;
        else if (*s == '}' && --depth == 0 && n < cap)
            o[n].at = start, o[n].len = (int)(s - start + 1), n++;
    }
    return n;
}
static int field(const obj_t *o, const char *key, char *out, int cap)   /* a string or number field of the object */
{
    char pat[64], t[8192];
    const char *at, *q;
    int n = 0;
    snprintf(t, sizeof t, "%.*s", o->len, o->at);
    snprintf(pat, sizeof pat, "\"%s\":", key);
    if (!(at = strstr(t, pat)))
        return -1;
    q = at + strlen(pat);
    if (*q == '"')
        for (q++; *q && *q != '"' && n + 1 < cap; q++)
            out[n++] = *q;
    else
        for (; *q && *q != ',' && *q != '}' && *q != ']' && n + 1 < cap; q++)
            out[n++] = *q;
    out[n] = 0;
    return 0;
}
static int count_options(const obj_t *o)
{
    char t[8192];
    const char *at;
    int n = 0, str = 0;
    snprintf(t, sizeof t, "%.*s", o->len, o->at);
    if (!(at = strstr(t, "\"options\":[")))
        return 0;
    for (at += 11; *at && (str || *at != ']'); at++) {
        if (*at == '\\' && at[1]) {
            at++;
            continue;
        }
        if (*at == '"' && !str)
            n++;
        if (*at == '"')
            str = !str;
    }
    return n;
}

static void write_file(const char *dir, const char *name, const char *ext, const char *text)
{
    char path[4096];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s.%s.json", dir, name, ext);
    if ((f = fopen(path, "w"))) {
        fputs(text, f);
        fclose(f);
    }
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

/* every parameter: its value read back as set, at its ends and its middle; the cutoff closed and open */
static void check_params(void *in, const char *name)
{
    static obj_t o[512];
    static char cp[BIG];
    int n, tried = 0, ok = 0;
    double closed = -1, open = -1;
    snprintf(cp, sizeof cp, "%s", get(in, "chain_params"));
    n = objects(cp, o, 512);
    CHECK(n > 3, "%s: %d chain_params", name, n);
    for (int i = 0; i < n; i++) {
        char key[32], type[16], mn[32] = "0", mx[32] = "0", stp[32] = "1", was[64], back[64], pname[64] = "";
        double lo, hi, step;
        if (field(&o[i], "key", key, sizeof key) || field(&o[i], "type", type, sizeof type))
            continue;
        if (!strcmp(key, "preset") || !strcmp(key, "synth"))
            continue;
        field(&o[i], "name", pname, sizeof pname);
        CHECK(get(in, key), "%s: %s has no value", name, key);
        snprintf(was, sizeof was, "%.63s", get(in, key) ? buf : "");
        if (!strcmp(type, "enum")) {
            lo = 0, hi = count_options(&o[i]) - 1;
            CHECK(hi >= 1 && hi < 128, "%s: %s has %g options", name, key, hi + 1);
        } else {
            field(&o[i], "min", mn, sizeof mn), field(&o[i], "max", mx, sizeof mx);
            lo = atof(mn), hi = atof(mx);
            CHECK(lo < hi, "%s: %s range %s..%s", name, key, mn, mx);
            CHECK(atof(was) >= lo - 1e-6 && atof(was) <= hi + 1e-6, "%s: %s = %s outside %s..%s", name, key, was,
                  mn, mx);
        }
        api->set_param(in, key, was);            /* the value it has: the same */
        CHECK(get(in, key) && !strcmp(buf, was), "%s: %s = %s set back reads %s", name, key, was, buf);
        field(&o[i], "step", stp, sizeof stp);
        step = strcmp(type, "float") ? 1.0 : atof(stp);
        double ends[3] = {lo, hi, lo + step * floor((hi - lo) / step / 2)};
        for (int e = 0; e < 3; e++) {
            char v[32];
            snprintf(v, sizeof v, !strcmp(type, "float") ? "%.4f" : "%.0f", ends[e]);
            set(in, key, "%s", v);
            tried++;
            snprintf(back, sizeof back, "%.63s", get(in, key) ? buf : "");
            if (fabs(atof(back) - atof(v)) < step / 2)
                ok++;
            else if (getenv("MV_VERBOSE"))
                printf("  %s: %s (%s) set %s reads %s\n", name, key, pname, v, back);
        }
        api->set_param(in, key, was);
        if (closed < 0 && strstr(pname, "CUTOFF") && strcmp(type, "enum")) {   /* the cutoff, its first sound */
            set(in, "preset", "0");
            set(in, key, "%s", mn);
            closed = hit(in, 48);
            set(in, key, "%s", mx);
            open = hit(in, 48);
            set(in, "preset", "0");
            printf("  %s: %s closed %.4f open %.4f\n", name, pname, closed, open);
            CHECK(open > closed * 1.5 && open > 0.01, "%s: the cutoff opens", name);
        }
    }
    printf("  %s: %d parameters, %d of %d values read back as set\n", name, n - 2, ok, tried);
    CHECK(ok >= tried * 9 / 10, "%s: values read back", name);
}

static void check_synth(void *in, const char *dir, int k)
{
    char name[64], st[8192], st2[8192];
    const char *v;
    int sounds, quiet = 0;
    double t0, t;
    set(in, "synth", "%d", k);
    CHECK((v = get(in, "synth")) && atoi(v) == k, "synth %d selected", k);
    snprintf(name, sizeof name, "%.63s", get(in, "name") ? buf : "?");
    v = get(in, "chain_params");
    CHECK(v && strlen(v) < BIG - 1, "%s: chain_params fits 64 KB", name);
    if (v && dir)
        write_file(dir, name, "chain_params", v);
    printf("== %s: chain_params %zu bytes", name, v ? strlen(v) : 0);
    v = get(in, "ui_hierarchy");
    CHECK(v != 0, "%s: ui_hierarchy", name);
    if (v && dir)
        write_file(dir, name, "ui_hierarchy", v);
    printf(", ui_hierarchy %zu bytes\n", v ? strlen(v) : 0);
    sounds = (v = get(in, "preset_count")) ? atoi(v) : 0;
    CHECK(sounds > 0, "%s: factory sounds", name);
    if (dir) {                                   /* (their names, for the page preview) */
        char path[4096];
        FILE *f;
        snprintf(path, sizeof path, "%s/%s.presets.txt", dir, name);
        if ((f = fopen(path, "w"))) {
            for (int i = 0; i < sounds; i++) {
                set(in, "preset", "%d", i);
                fprintf(f, "%s\n", get(in, "preset_name") ? buf : "");
            }
            fclose(f);
        }
    }
    for (int i = 0; i < sounds; i++) {           /* every factory sound plays */
        double r;
        set(in, "preset", "%d", i);
        CHECK((v = get(in, "preset")) && atoi(v) == i, "%s: sound %d", name, i);
        CHECK((v = get(in, "preset_name")) && *v && strcmp(v, "-"), "%s: sound %d's name", name, i);
        r = hit(in, 60);
        if (r < 0.002) {
            quiet++;
            printf("  %s: sound %d (%s) rms %.5f\n", name, i, get(in, "preset_name"), r);
        }
    }
    CHECK(quiet <= sounds / 8, "%s: %d of %d sounds quiet", name, quiet, sounds);
    check_params(in, name);
    /* the state: saved, the sound changed, loaded back */
    set(in, "preset", "%d", sounds > 1 ? 1 : 0);
    snprintf(st, sizeof st, "%.8000s", (v = get(in, "state")) ? v : "");
    CHECK(strstr(st, "\"synth\":") && strstr(st, "\"patch\":"), "%s: state %s", name, st);
    set(in, "preset", "0");
    set(in, "state", "%s", st);
    snprintf(st2, sizeof st2, "%.8000s", (v = get(in, "state")) ? v : "");
    CHECK(!strcmp(st, st2), "%s: state loads back\n  %s\n  %s", name, st, st2);
    CHECK((v = get(in, "preset_name")) && strcmp(v, "-"), "%s: the loaded state's sound name", name);
    /* the cost: eight voices on the audio callback */
    set(in, "preset", "0");
    for (int i = 0; i < 8; i++)
        note(in, 1, 48 + 3 * i, 100);
    t0 = now();
    play(in, 345);                               /* (one second) */
    t = now() - t0;
    for (int i = 0; i < 8; i++)
        note(in, 0, 48 + 3 * i, 0);
    play(in, 200);
    printf("  %s: eight voices %.1f%% of one core\n", name, 100.0 * t);
}

static void *make(const char *dir, const char *defaults)
{
    void *in = api->create_instance(dir, defaults);
    CHECK(in != 0, "create_instance %s", dir);
    return in;
}

/* ---- SysEx banks */
static int state_patch(void *in, uint8_t *p, int cap)   /* the state's patch bytes: their count */
{
    const char *v = get(in, "state"), *h = v ? strstr(v, "\"patch\":\"") : 0;
    int n = 0;
    for (h = h ? h + 9 : 0; h && h[0] != '"' && h[1] && n < cap; h += 2) {
        unsigned x;
        if (sscanf(h, "%2x", &x) != 1)
            return -1;
        p[n++] = (uint8_t)x;
    }
    return n;
}
static int bank_named(void *in, const char *label)      /* its index in bank_list, -1 none */
{
    static obj_t o[128];
    const char *v = get(in, "bank_list");
    char t[128];
    int n = v ? objects(v, o, 128) : 0;
    for (int i = 0; i < n; i++)
        if (!field(&o[i], "label", t, sizeof t) && !strcmp(t, label))
            return i;
    return -1;
}
static int preset_max(void *in)                          /* chain_params' Sound range */
{
    obj_t o[1];
    char t[32];
    const char *v = get(in, "chain_params");
    return v && objects(v, o, 1) == 1 && !field(&o[0], "max", t, sizeof t) ? atoi(t) : -1;
}
static FILE *open_syx(const char *sx, const char *name)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/%s.syx", sx, name);
    return fopen(path, "wb");
}
static void drop_syx(const char *sx, const char *name)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/%s.syx", sx, name);
    remove(path);
}
/* files written into the folder's SysEx (HYDRA's native frames of three factory sounds with bytes between them, a PPG
 * bank of made-up programs, one cut short, one without a frame), then a new instance: banks named for their files,
 * their sounds, the Sound list's range, a state on a bank's sound, no bank for a synth without SysEx. A PPG factory
 * bank beside them (the Makefile's "PPG factory", when it has one) must give the factory programs back */
static void test_sysex(const char *dir)
{
    char sx[4096], st[8192], fname[3][64], t[64];
    uint8_t pat[3][512], p[512], q[512];
    int plen[3], b, n, same = 0, factory;
    const char *v;
    FILE *f;
    void *in = make(dir, "{\"synth\":\"HYDRA\"}");
    snprintf(sx, sizeof sx, "%s/SysEx", dir);
    mkdir(sx, 0755);
    if ((f = open_syx(sx, "HYDRA test"))) {
        for (int i = 0; i < 3; i++) {
            set(in, "preset", "%d", i + 1);
            snprintf(fname[i], sizeof fname[i], "%.63s", get(in, "preset_name") ? buf : "");
            plen[i] = state_patch(in, pat[i], sizeof pat[i]);
            CHECK(plen[i] == 144, "HYDRA: a patch of %d bytes", plen[i]);
            fputc(0xF0, f), fputc(0x7D, f), fputc(0x48, f), fputc(0x59, f), fputc(0x01, f);
            for (int k = 0; k < plen[i]; k++)
                fputc(pat[i][k] & 15, f), fputc(pat[i][k] >> 4, f);
            fputc(0xF7, f);
            fputc(0x00, f), fputc(0x12, f);      /* (bytes between frames) */
        }
        fclose(f);
    }
    if ((f = open_syx(sx, "PPG made"))) {        /* (a 2.3's length: no 20 bytes after the programs) */
        fputc(0xF0, f), fputc(0x29, f), fputc(0x01, f), fputc(0x0D, f);
        for (int k = 0; k < 10200; k++)
            fputc((k * 7 + k / 102) & 15, f);
        fputc(0xF7, f);
        fclose(f);
    }
    if ((f = open_syx(sx, "PPG cut"))) {
        fputc(0xF0, f), fputc(0x29, f), fputc(0x01, f), fputc(0x0D, f);
        for (int k = 0; k < 300; k++)
            fputc(k & 15, f);
        fputc(0xF7, f);
        fclose(f);
    }
    if ((f = open_syx(sx, "no frame"))) {
        fputs("not SysEx", f);
        fclose(f);
    }
    api->destroy_instance(in);
    in = make(dir, "{\"synth\":\"HYDRA\"}");
    /* HYDRA: its bank, three sounds as they were sent */
    b = bank_named(in, "HYDRA test");
    CHECK(b == 1, "HYDRA: bank %d", b);
    CHECK(bank_named(in, "PPG made") < 0 && bank_named(in, "no frame") < 0, "HYDRA: no other synth's bank");
    CHECK((v = get(in, "ui_hierarchy")) && strstr(v, "\"banks\":{") && strstr(v, "\"items_param\":\"bank_list\""),
          "HYDRA: a Bank level");
    set(in, "bank", "%d", b);
    CHECK((v = get(in, "bank")) && atoi(v) == b, "HYDRA: bank picked");
    CHECK((v = get(in, "preset_count")) && atoi(v) == 3, "HYDRA: three sounds in the bank (%s)", v ? v : "-");
    CHECK(preset_max(in) == 2, "HYDRA: the Sound's range is the bank's (max %d)", preset_max(in));
    for (int i = 0; i < 3; i++) {
        set(in, "preset", "%d", i);
        CHECK((v = get(in, "preset")) && atoi(v) == i, "HYDRA: bank sound %d", i);
        v = get(in, "preset_name");
        CHECK(v && *v && !strncmp(v, fname[i], strlen(v)), "HYDRA: bank sound %d named %s (factory %s)", i,
              v ? v : "-", fname[i]);
        CHECK(state_patch(in, p, sizeof p) == plen[i] && !memcmp(p, pat[i], (size_t)plen[i]),
              "HYDRA: bank sound %d is the sound sent", i);
        CHECK(hit(in, 60) > 0.002, "HYDRA: bank sound %d plays", i);
    }
    snprintf(st, sizeof st, "%.8000s", (v = get(in, "state")) ? v : "");
    CHECK(strstr(st, "\"bank\":\"HYDRA test\"") && strstr(st, "\"preset\":2"), "HYDRA: a bank sound's state %s", st);
    set(in, "bank", "0");
    CHECK((v = get(in, "preset_count")) && atoi(v) == 17 && preset_max(in) == 16, "HYDRA: the factory bank again");
    set(in, "state", "%s", st);
    CHECK((v = get(in, "bank")) && atoi(v) == b && (v = get(in, "preset")) && atoi(v) == 2 &&
          (v = get(in, "preset_name")) && !strncmp(v, fname[2], strlen(v)), "HYDRA: a state shows its bank and sound");
    CHECK(state_patch(in, p, sizeof p) == plen[2] && !memcmp(p, pat[2], (size_t)plen[2]), "HYDRA: the state's sound");
    if (have_ppg) {                              /* (the PPG module: a local test file only) */
        /* PPG: the made-up bank, a hundred programs; the cut one and the file without a frame: no bank */
        set(in, "synth", "PPG");
        b = bank_named(in, "PPG made");
        CHECK(b >= 1, "PPG: bank %d", b);
        CHECK(bank_named(in, "PPG cut") < 0 && bank_named(in, "no frame") < 0 && bank_named(in, "HYDRA test") < 0,
              "PPG: no bank of a broken file or another synth's");
        set(in, "bank", "%d", b);
        CHECK((v = get(in, "preset_count")) && atoi(v) == 100 && preset_max(in) == 99, "PPG: a hundred programs");
        for (int i = 0; i < 100; i += 11) {
            set(in, "preset", "%d", i);
            snprintf(t, sizeof t, "PROGRAM %02d", i);
            CHECK((v = get(in, "preset_name")) && !strcmp(v, t), "PPG: program %d named %s", i, v ? v : "-");
            hit(in, 60);                             /* (made-up values: whatever it plays, it plays) */
        }
        factory = bank_named(in, "PPG factory");
        if (factory < 0) {
            printf("  PPG: no factory bank here (PPG_BANK): the factory programs not compared\n");
        } else {                                     /* program k: factory sound k + 1, its name aside */
            for (int k = 0; k < 100; k++) {
                set(in, "bank", "%d", factory);
                set(in, "preset", "%d", k);
                n = state_patch(in, p, sizeof p);
                set(in, "bank", "0");
                set(in, "preset", "%d", k + 1);
                same += n == 115 && state_patch(in, q, sizeof q) == 115 && !memcmp(p, q, 103);
            }
            printf("  PPG: the factory bank gives %d of 100 factory programs back\n", same);
            CHECK(same == 100, "PPG: the factory bank is the factory programs (%d of 100)", same);
        }
    } else {
        printf("  PPG: no PPG module (its file stays local): its banks not tested\n");
    }
    /* TRIO: no SysEx, no Bank level */
    set(in, "synth", "TRIO");
    CHECK((v = get(in, "bank_list")) && objects(v, (obj_t[2]){0}, 2) == 1, "TRIO: the factory sounds alone");
    CHECK((v = get(in, "ui_hierarchy")) && !strstr(v, "\"banks\""), "TRIO: no Bank level");
    api->destroy_instance(in);
    drop_syx(sx, "HYDRA test");
    drop_syx(sx, "PPG made");
    drop_syx(sx, "PPG cut");
    drop_syx(sx, "no frame");
}

int main(int argc, char **argv)
{
    char so[4096], err[512];
    void *lib, *in;
    move_plugin_init_v2_fn init;
    int one = argc == 3 && !strcmp(argv[1], "--one"), n;
    const char *dir = one ? argv[2] : argc == 3 ? argv[1] : 0, *out = one ? 0 : argc == 3 ? argv[2] : 0;
    if (!dir) {
        fprintf(stderr, "usage: test_dsp MODULE_DIR OUT_DIR | --one MODULE_DIR\n");
        return 2;
    }
    {
        FILE *f;
        snprintf(so, sizeof so, "%s/Modules/ppg.mmod", dir);
        if ((f = fopen(so, "rb")))
            fclose(f), have_ppg = 1;
    }
    snprintf(so, sizeof so, "%s/dsp.so", dir);
    if (!(lib = dlopen(so, RTLD_NOW | RTLD_LOCAL))) {
        printf("FAIL dlopen: %s\n", dlerror());
        return 1;
    }
    init = (move_plugin_init_v2_fn)dlsym(lib, "move_plugin_init_v2");
    CHECK(init && (api = init(&HOST)) && api->api_version == 2, "move_plugin_init_v2");
    if (!api)
        return 1;
    if (out)
        mkdir(out, 0755);
    if (!(in = make(dir, 0)))
        return 1;
    CHECK(api->get_error(in, err, sizeof err) == 0, "get_error: %s", err);
    {
        static obj_t o[128];
        const char *list = get(in, "synth_list");
        n = list ? objects(list, o, 128) : 0;
        printf("synth_list %s\n", list ? list : "(none)");
    }
    CHECK(one ? n == 1 : n == 3 + have_ppg, "%d synths", n);
    for (int k = 0; k < n; k++)
        check_synth(in, out, k);
    if (!one && n > 1) {
        char st[8192], name[64];
        const char *v;
        /* a state names its synth: loading it picks that synth */
        set(in, "synth", "0");
        snprintf(name, sizeof name, "%.63s", get(in, "name"));
        snprintf(st, sizeof st, "%.8000s", get(in, "state"));
        set(in, "synth", "%d", n - 1);
        set(in, "state", "%s", st);
        CHECK((v = get(in, "synth")) && atoi(v) == 0 && !strcmp(get(in, "name"), name), "a state picks its synth");
        /* a synth by name, a patch's defaults */
        set(in, "synth", "TRIO");
        CHECK(!strcmp(get(in, "name"), "TRIO"), "a synth by name");
        api->destroy_instance(in);
        in = make(dir, "{\"synth\":\"WHEEL\"}");
        CHECK(in && !strcmp(get(in, "name"), "WHEEL"), "the defaults' synth");
        /* two new instances of one synth: their own statics, the same sound */
        api->destroy_instance(in);
        in = make(dir, "{\"synth\":\"HYDRA\"}");
        {
            void *b = make(dir, "{\"synth\":\"HYDRA\"}");
            int16_t x[2 * MOVE_FRAMES_PER_BLOCK], y[2 * MOVE_FRAMES_PER_BLOCK];
            double ex = 0, ey = 0;               /* (alike, not the same: every part's block moves the host's dither) */
            set(in, "preset", "3");
            set(b, "preset", "3");
            note(in, 1, 60, 100);
            note(b, 1, 60, 100);
            for (int i = 0; i < 100; i++) {
                api->render_block(in, x, MOVE_FRAMES_PER_BLOCK);
                if (i == 50)                     /* (b's own edit: in must not hear it) */
                    set(b, "p0", "-56");
                api->render_block(b, y, MOVE_FRAMES_PER_BLOCK);
                for (int k = 0; i < 50 && k < 2 * MOVE_FRAMES_PER_BLOCK; k++)
                    ex += (double)x[k] * x[k], ey += (double)y[k] * y[k];
            }
            CHECK(ex > 0 && fabs(sqrt(ex) - sqrt(ey)) < 0.05 * sqrt(ex), "two instances play alike (%g, %g)", ex, ey);
            CHECK(play(in, 10) > 0.001, "one instance's edit is its own");
            api->destroy_instance(b);
        }
    }
    api->destroy_instance(in);
    if (!one)
        test_sysex(dir);
    if (!one) {                                  /* a folder without modules: silence, an error to show */
        char empty[4096];
        int16_t x[2 * MOVE_FRAMES_PER_BLOCK];
        snprintf(empty, sizeof empty, "%s/none", out);
        mkdir(empty, 0755);
        in = make(empty, 0);
        CHECK(in && api->get_error(in, err, sizeof err) > 0, "no modules: an error");
        printf("no modules: %s\n", err);
        CHECK(get(in, "chain_params") && !strcmp(buf, "[]"), "no modules: no parameters");
        CHECK(get(in, "ui_hierarchy") != 0, "no modules: a page");
        memset(x, 1, sizeof x);
        api->render_block(in, x, MOVE_FRAMES_PER_BLOCK);
        note(in, 1, 60, 100);
        set(in, "preset", "1");
        CHECK(x[0] == 0 && x[2 * MOVE_FRAMES_PER_BLOCK - 1] == 0, "no modules: silence");
        api->destroy_instance(in);
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
