/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* Melodee Modules as a Schwung sound generator (plugin_api_v2): every synth module file (.mmod) in the Modules folder
 * of the module's own folder, each opened when the instance is made (Schwung's loader thread: file reads, the code
 * mapped, the SysEx folder's files given to the modules, the pages built), one of them playing. Picking another (its
 * "Synth" list), a bank, a sound, a value or a saved state is then only memory: Schwung calls everything after
 * create_instance on its audio callback. */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "mv_synth.h"
#include "schwung_api.h"

#define MV_MAX_SYNTHS 128u                       /* (Schwung's lists: 128 at most) */

typedef struct mv_inst {
    mv_synth *syn[MV_MAX_SYNTHS];
    uint32_t nsyn, cur;
    char *synth_list;                            /* [{"index":0,"label":"HYDRA"},..] */
    uint32_t synth_list_len;
    char err[256];
} mv_inst;

static const host_api_v1_t *host;

static void say(const char *fmt, const char *a, const char *b)   /* (Schwung's log: the loader thread only) */
{
    char t[512];
    snprintf(t, sizeof t, fmt, a, b);
    if (host && host->log)
        host->log(t);
    else
        fprintf(stderr, "%s\n", t);
}

static int by_name(const void *a, const void *b)
{
    return strcmp((*(mv_synth *const *)a)->file.name, (*(mv_synth *const *)b)->file.name);
}

static void scan(mv_inst *in, const char *dir)
{
    DIR *dp = opendir(dir);
    struct dirent *de;
    if (!dp) {
        snprintf(in->err, sizeof in->err, "no folder %.200s", dir);
        return;
    }
    while ((de = readdir(dp)) && in->nsyn < MV_MAX_SYNTHS) {
        char path[4096];
        size_t n = strlen(de->d_name);
        const char *err;
        mv_synth *s;
        uint32_t i;
        if (de->d_name[0] == '.' || n < 6 || strcasecmp(de->d_name + n - 5, ".mmod"))
            continue;
        if (snprintf(path, sizeof path, "%s/%s", dir, de->d_name) >= (int)sizeof path || !(s = calloc(1, sizeof *s)))
            continue;
        if ((err = mv_synth_open(s, path))) {
            say("melodee-modules: %s: %s", de->d_name, err);
            free(s);
            continue;
        }
        for (i = 0; i < in->nsyn && memcmp(in->syn[i]->file.id, s->file.id, 8); i++)
            ;
        if (i < in->nsyn) {                      /* the same module twice: the newer version */
            mv_synth *drop = s->file.head.version > in->syn[i]->file.head.version ? in->syn[i] : s;
            if (drop != s)
                in->syn[i] = s;
            mv_synth_close(drop);
            free(drop);
            continue;
        }
        in->syn[in->nsyn++] = s;
    }
    closedir(dp);
    qsort(in->syn, in->nsyn, sizeof in->syn[0], by_name);
}

/* the SysEx files (.syx) in dir, by name: each to every synth's module; what a module offers is a bank of its synth's
 * (Schwung's loader thread, as the module files are read) */
#define MV_SYX_MAX (1u << 20)                    /* (a file, at most: a DX7 bank is 4 KB, a PPG bank 10 KB) */
#define MV_MAX_SYX 512u
static int by_file(const void *a, const void *b)   /* (letters without case, numbers by value: bank 2 before 10) */
{
    const unsigned char *x = *(const unsigned char *const *)a, *y = *(const unsigned char *const *)b;
    while (*x && *y) {
        if (*x >= '0' && *x <= '9' && *y >= '0' && *y <= '9') {
            unsigned long u = strtoul((const char *)x, (char **)&x, 10), v = strtoul((const char *)y, (char **)&y, 10);
            if (u != v)
                return u < v ? -1 : 1;
            continue;
        }
        if (tolower(*x) != tolower(*y))
            return tolower(*x) - tolower(*y);
        x++, y++;
    }
    return *x - *y;
}
static void scan_sysex(mv_inst *in, const char *dir)
{
    DIR *dp = opendir(dir);
    struct dirent *de;
    char *name[MV_MAX_SYX];
    uint32_t nname = 0;
    if (!dp)
        return;                                  /* (no SysEx folder: the factory sounds alone) */
    while ((de = readdir(dp)) && nname < MV_MAX_SYX) {
        size_t n = strlen(de->d_name);
        if (de->d_name[0] != '.' && n > 4 && !strcasecmp(de->d_name + n - 4, ".syx") &&
            (name[nname] = strdup(de->d_name)))
            nname++;
    }
    closedir(dp);
    qsort(name, nname, sizeof name[0], by_file);
    for (uint32_t f = 0; f < nname; f++) {
        char path[4096], msg[64] = "", bank[MV_BANK_NAME];
        uint8_t *data = 0;
        long len = -1;
        uint32_t got = 0;
        FILE *fp;
        if (snprintf(path, sizeof path, "%s/%s", dir, name[f]) < (int)sizeof path && (fp = fopen(path, "rb"))) {
            if (!fseek(fp, 0, SEEK_END) && (len = ftell(fp)) > 0 && len <= (long)MV_SYX_MAX &&
                !fseek(fp, 0, SEEK_SET) && (data = malloc((size_t)len)) && fread(data, 1, (size_t)len, fp) != (size_t)len)
                len = -1;
            fclose(fp);
        }
        if (len > (long)MV_SYX_MAX) {
            say("melodee-modules: SysEx/%s: %s", name[f], "larger than 1 MB");
        } else if (data && len > 0) {
            snprintf(bank, sizeof bank, "%.*s", (int)strlen(name[f]) - 4, name[f]);
            for (uint32_t i = 0; i < in->nsyn; i++) {
                char m[64];
                got += mv_synth_sysex(in->syn[i], bank, data, (uint32_t)len, m, sizeof m);
                if (m[0])
                    snprintf(msg, sizeof msg, "%s", m);
            }
            if (!got)
                say("melodee-modules: SysEx/%s: %s", name[f], msg[0] ? msg : "no synth here takes its SysEx");
        }
        free(data);
        free(name[f]);
    }
}

static void select_synth(mv_inst *in, uint32_t k)
{
    if (k >= in->nsyn || k == in->cur)
        return;
    mm_kill(&in->syn[in->cur]->part);            /* (its notes would not hear their note-offs) */
    in->cur = k;
}
static int32_t synth_named(const mv_inst *in, const char *id, const char *name)
{
    for (uint32_t i = 0; id && i < in->nsyn; i++)
        if (!strcmp(in->syn[i]->idhex, id))
            return (int32_t)i;
    for (uint32_t i = 0; name && i < in->nsyn; i++)
        if (!strcasecmp(in->syn[i]->file.name, name))
            return (int32_t)i;
    return -1;
}

static void destroy_instance(void *instance)
{
    mv_inst *in = instance;
    if (!in)
        return;
    for (uint32_t i = 0; i < in->nsyn; i++) {
        mv_synth_close(in->syn[i]);
        free(in->syn[i]);
    }
    free(in->synth_list);
    free(in);
}

static void *create_instance(const char *module_dir, const char *json_defaults)
{
    mv_inst *in = calloc(1, sizeof *in);
    char dir[4096], t[64];
    mv_json j = {0};
    if (!in)
        return 0;
    snprintf(dir, sizeof dir, "%s/Modules", module_dir && *module_dir ? module_dir : ".");
    scan(in, dir);
    if (!in->nsyn && !in->err[0])
        snprintf(in->err, sizeof in->err, "no synth module files (.mmod) in %.200s", dir);
    snprintf(dir, sizeof dir, "%s/SysEx", module_dir && *module_dir ? module_dir : ".");
    scan_sysex(in, dir);
    mv_jf(&j, "[");
    for (uint32_t i = 0; i < in->nsyn; i++) {
        if (mv_synth_contract(in->syn[i], in->nsyn))
            say("melodee-modules: %s: %s", in->syn[i]->file.name, "out of memory");
        mv_jf(&j, "%s{\"index\":%u,\"label\":", i ? "," : "", (unsigned)i);
        mv_js(&j, in->syn[i]->file.name);
        mv_jf(&j, "}");
    }
    mv_jf(&j, "]");
    if (j.err) {
        free(j.b);
        destroy_instance(in);
        return 0;
    }
    in->synth_list = j.b;
    in->synth_list_len = j.n;
    if (!mv_json_field(json_defaults, "synth", t, sizeof t)) {   /* (module.json's defaults, a patch's config) */
        int32_t k = synth_named(in, t, t);
        if (k >= 0)
            in->cur = (uint32_t)k;
    }
    return in;
}

static void on_midi(void *instance, const uint8_t *msg, int len, int source)
{
    mv_inst *in = instance;
    if (!in->nsyn || source == MOVE_MIDI_SOURCE_FX_BROADCAST || source == MOVE_MIDI_SOURCE_TOUCH)
        return;
    mv_midi(in->syn[in->cur], msg, len);
}

static void set_param(void *instance, const char *key, const char *val)
{
    mv_inst *in = instance;
    mv_synth *s = in->nsyn ? in->syn[in->cur] : 0;
    const mv_param *p;
    if (!key || !val)
        return;
    if (!strcmp(key, "synth")) {                 /* its index in synth_list, or its name */
        char *end;
        long k = strtol(val, &end, 10);
        if (end == val || *end)
            k = synth_named(in, val, val);
        if (k >= 0)
            select_synth(in, (uint32_t)k);
    } else if (!s) {
        return;
    } else if (!strcmp(key, "preset")) {
        mv_preset(s, (uint32_t)atoi(val));
    } else if (!strcmp(key, "bank")) {           /* its index in bank_list (0 the factory sounds) */
        mv_bank_pick(s, (uint32_t)atoi(val));
    } else if (!strcmp(key, "state")) {          /* the synth it names, then its sound */
        char id[24] = "", name[MOD3_NAME_MAX + 1] = "";
        int32_t k;
        mv_json_field(val, "synth", id, sizeof id);
        mv_json_field(val, "name", name, sizeof name);
        if ((k = synth_named(in, id[0] ? id : 0, name[0] ? name : 0)) < 0)
            return;                              /* (a synth not here now: what plays stays) */
        select_synth(in, (uint32_t)k);
        mv_state_set(in->syn[in->cur], val);
    } else if ((p = mv_find(s, key))) {
        mv_set(s, p, val);
    }
}

static int put(char *buf, int cap, const char *s, uint32_t n)
{
    if (!buf || cap <= 0 || n >= (uint32_t)cap)
        return -1;
    memcpy(buf, s, n);
    buf[n] = 0;
    return (int)n;
}

static int get_param(void *instance, const char *key, char *buf, int buf_len)
{
    mv_inst *in = instance;
    mv_synth *s = in->nsyn ? in->syn[in->cur] : 0;
    const mv_param *p;
    if (!key || !buf || buf_len <= 0)
        return -1;
    if (!strcmp(key, "synth_list"))
        return put(buf, buf_len, in->synth_list, in->synth_list_len);
    if (!strcmp(key, "synth"))
        return snprintf(buf, (size_t)buf_len, "%u", (unsigned)in->cur);
    if (!s) {
        static const char NONE[] = "{\"levels\":{\"root\":{\"name\":\"Melodee Modules\",\"knobs\":[],\"params\":[]}}}";
        if (!strcmp(key, "ui_hierarchy"))
            return put(buf, buf_len, NONE, sizeof NONE - 1u);
        if (!strcmp(key, "chain_params"))
            return put(buf, buf_len, "[]", 2u);
        if (!strcmp(key, "state"))
            return -1;                           /* (none to save) */
        return -1;
    }
    if (!strcmp(key, "chain_params"))
        return mv_chain_params(s, buf, buf_len);
    if (!strcmp(key, "ui_hierarchy"))
        return put(buf, buf_len, s->hierarchy, s->hier_len);
    if (!strcmp(key, "state"))
        return mv_state_get(s, buf, buf_len);
    if (!strcmp(key, "bank_list"))
        return put(buf, buf_len, s->bank_list, s->bank_list_len);
    if (!strcmp(key, "bank"))
        return snprintf(buf, (size_t)buf_len, "%u", (unsigned)s->bank_cur);
    if (!strcmp(key, "preset"))
        return snprintf(buf, (size_t)buf_len, "%d", (int)mv_preset_index(s));
    if (!strcmp(key, "preset_count"))
        return snprintf(buf, (size_t)buf_len, "%u", (unsigned)mv_preset_count(s));
    if (!strcmp(key, "preset_name"))
        return snprintf(buf, (size_t)buf_len, "%s", s->preset < 0 ? "-" : s->preset_name);
    if (!strcmp(key, "name"))
        return snprintf(buf, (size_t)buf_len, "%s", s->file.name);
    if ((p = mv_find(s, key)))
        return mv_get(s, p, buf, buf_len);
    return -1;
}

static int get_error(void *instance, char *buf, int buf_len)
{
    mv_inst *in = instance;
    if (!in->err[0] || in->nsyn)
        return 0;
    return snprintf(buf, (size_t)buf_len, "%s", in->err);
}

static void render_block(void *instance, int16_t *out, int frames)
{
    mv_inst *in = instance;
    if (!in->nsyn) {
        memset(out, 0, (size_t)frames * 2u * sizeof *out);
        return;
    }
    if (host && host->get_bpm) {                 /* (LFO SYNC: the Move's tempo) */
        float bpm = host->get_bpm();
        if (bpm > 1.0f)
            in->syn[in->cur]->part.bpm = bpm;
    }
    mv_render(in->syn[in->cur], out, frames);
}

static plugin_api_v2_t api = {MOVE_PLUGIN_API_VERSION_2, create_instance, destroy_instance, on_midi,
                              set_param,                 get_param,       get_error,        render_block};

__attribute__((visibility("default"))) plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *h)
{
    host = h;
    mm_init();                                   /* (the tables, once, before any instance on any thread) */
    return &api;
}
