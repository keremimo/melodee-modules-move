/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
#include "mv_synth.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- JSON */
void mv_jf(mv_json *j, const char *fmt, ...)
{
    va_list ap;
    while (!j->err) {
        uint32_t room = j->cap - j->n;
        int n;
        va_start(ap, fmt);
        n = vsnprintf(j->b ? j->b + j->n : 0, j->b ? room : 0, fmt, ap);
        va_end(ap);
        if (n < 0) {
            j->err = 1;
            return;
        }
        if ((uint32_t)n < room) {
            j->n += (uint32_t)n;
            return;
        }
        uint32_t cap = j->cap ? j->cap : 4096u;
        while (cap - j->n <= (uint32_t)n)
            cap *= 2u;
        char *b = realloc(j->b, cap);
        if (!b) {
            j->err = 1;
            return;
        }
        j->b = b;
        j->cap = cap;
    }
}
void mv_js(mv_json *j, const char *s)
{
    char t[256];
    uint32_t n = 0;
    t[n++] = '"';
    for (; s && *s && n < sizeof t - 8u; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            t[n++] = '\\', t[n++] = (char)c;
        else if (c < 0x20u)
            n += (uint32_t)snprintf(t + n, sizeof t - n, "\\u%04x", c);
        else
            t[n++] = (char)c;
    }
    t[n++] = '"';
    t[n] = 0;
    mv_jf(j, "%s", t);
}
int mv_json_field(const char *json, const char *key, char *buf, uint32_t cap)
{
    size_t kn = strlen(key);
    const char *at = json;
    if (!json || !cap)
        return -1;
    while ((at = strchr(at, '"'))) {
        const char *q = at + 1;
        if (!strncmp(q, key, kn) && q[kn] == '"') {
            q += kn + 1;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
                q++;
            if (*q == ':') {
                uint32_t n = 0;
                q++;
                while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
                    q++;
                if (*q == '"') {
                    for (q++; *q && *q != '"' && n + 1u < cap; q++)
                        buf[n++] = *q;
                } else {
                    for (; *q && *q != ',' && *q != '}' && *q != ' ' && n + 1u < cap; q++)
                        buf[n++] = *q;
                }
                buf[n] = 0;
                return 0;
            }
        }
        at++;                                    /* (a string that is not the key: past its quote) */
    }
    return -1;
}

/* ---- parameters */
static int sch_shown(const mm_sch_param *sp) { return sp && !(sp->flags & MOD3_SF_HIDDEN) && sp->max > sp->min; }

static const mm_common *common_find(uint32_t id)
{
    for (uint32_t i = 0; i < MM_NCOMMON; i++)
        if (MM_COMMON[i].id == id)
            return &MM_COMMON[i];
    return 0;
}

const mv_param *mv_find(const mv_synth *s, const char *key)
{
    for (uint32_t i = 0; i < s->nparam; i++)
        if (!strcmp(s->param[i].key, key))
            return &s->param[i];
    return 0;
}
static int32_t param_index(const mv_synth *s, const char *key)
{
    const mv_param *p = mv_find(s, key);
    return p ? (int32_t)(p - s->param) : -1;
}

/* how Schwung is told of it: a list of the FM-1's texts (names), or a number in Schwung's own units where the FM-1's
 * text is one ("+7 st", "50%", "-4.0 dB"), else the module's value as it is (times, rates, cutoffs) */
static void classify(mv_param *p, int own_text)
{
    int32_t n = p->max - p->min + 1;
    p->kind = MV_INT;
    p->ofs = 0;
    p->div = 1;
    p->sunit = 0;
    switch (p->fmt) {
    case MOD3_F_ENUM:
    case MOD3_F_ONOFF:
    case MOD3_F_NOTE:
        if (n <= 128)
            p->kind = MV_ENUM;
        break;
    case MOD3_F_SEMI: p->sunit = "st"; break;
    case MOD3_F_OCT: p->sunit = "oct"; break;
    case MOD3_F_BPM: p->sunit = "BPM"; break;
    case MOD3_F_STEPS: p->sunit = "STEP"; break;
    case MOD3_F_INT: p->sunit = p->unit && *p->unit ? p->unit : 0; break;
    case MOD3_F_OFS:
        p->ofs = (int16_t)((p->min + p->max) / 2);
        p->sunit = p->unit && *p->unit ? p->unit : 0;
        break;
    case MOD3_F_PCT:
        if (p->max == 100 && p->min >= 0)
            p->sunit = "%";
        else if (p->min >= 0 && p->max <= 127)
            p->kind = MV_FLOAT, p->div = 127, p->sunit = "%";
        break;
    case MOD3_F_BIPCT:
        if (p->min >= -64 && p->max <= 64)
            p->kind = MV_FLOAT, p->div = 64, p->sunit = "%";
        break;
    case MOD3_F_DB:
        p->kind = MV_FLOAT, p->div = 2, p->ofs = 112, p->sunit = "dB";
        break;
    default:                                     /* (TIME, LFOHZ, CUTOFF, ..: the value as it is) */
        break;
    }
    if (own_text && p->kind != MV_ENUM && n <= 64)   /* the module's own words */
        p->kind = MV_ENUM, p->ofs = 0, p->div = 1, p->sunit = 0;
}

/* a name's last word made long ("FILTER 1 RES": "FILTER 1 RESO", "CUT": "CUTOFF"): the knob keeps the FM-1's */
static void long_words(char *name, size_t cap)
{
    static const char *const MAP[][2] = {{"CUT", "CUTOFF"}, {"RES", "RESO"}, {"EMPH", "EMPHASIS"}, {"ATK", "ATTACK"},
                                         {"DEC", "DECAY"}, {"SUS", "SUSTAIN"}, {"REL", "RELEASE"}};
    char *w = strrchr(name, ' ');
    w = w ? w + 1 : name;
    for (uint32_t i = 0; i < sizeof MAP / sizeof MAP[0]; i++)
        if (!strcmp(w, MAP[i][0]) && (size_t)(w - name) + strlen(MAP[i][1]) < cap) {
            strcpy(w, MAP[i][1]);
            return;
        }
}

static mv_param *padd(mv_synth *s, uint32_t cap, const char *key, uint8_t src, const char *name, const char *label)
{
    mv_param *p;
    if (s->nparam >= cap || mv_find(s, key))
        return 0;
    p = &s->param[s->nparam++];
    memset(p, 0, sizeof *p);
    snprintf(p->key, sizeof p->key, "%s", key);
    p->src = src;
    snprintf(p->name, sizeof p->name, "%s", name);
    long_words(p->name, sizeof p->name);
    snprintf(p->label, sizeof p->label, "%s", label);
    return p;
}

/* a schema parameter: its long name, else its label after its page's title (a second page's "1B": "1"), or the label
 * alone where it names the title already ("RING A" on RING); its knob's label the FM-1's */
static int32_t add_sch(mv_synth *s, uint32_t cap, const mm_sch_param *sp, const char *title)
{
    char name[40], pre[48] = "", key[8];
    const char *label = sp->label ? sp->label : "?", *last;
    const mod_engine_v3 *e = &s->img.eng;
    mv_param *p;
    size_t n;
    int32_t at;
    snprintf(key, sizeof key, "s%u", (unsigned)sp->id);
    if ((at = param_index(s, key)) >= 0)
        return at;
    if (title) {
        snprintf(pre, sizeof pre, "%s", title);
        n = strlen(pre);
        if (n >= 2 && pre[n - 1] >= 'A' && pre[n - 1] <= 'Z' && pre[n - 2] >= '0' && pre[n - 2] <= '9')
            pre[n - 1] = 0;
    }
    last = strrchr(pre, ' ');
    last = last ? last + 1 : pre;
    if (sp->name)
        snprintf(name, sizeof name, "%s", sp->name);
    else if (!*pre || strstr(label, last))
        snprintf(name, sizeof name, "%s", label);
    else
        snprintf(name, sizeof name, "%.20s %s", pre, label);
    if (!(p = padd(s, cap, key, MV_SRC_SCH, name, label)))
        return -1;
    p->sp = sp;
    p->fmt = sp->fmt;
    p->min = sp->min;
    p->max = sp->max;
    p->def = sp->def;
    p->names = (const char *const *)sp->names;
    p->unit = sp->unit;
    if ((sp->flags & MOD3_SF_DESC) && e->desc) {  /* the module's descriptor of it now */
        const mod_param_v3 *d;
        mm_enter(&s->part);
        if ((d = e->desc(&s->part.h, MOD3_DESC_ID + sp->id)) && d->max > d->min) {
            p->fmt = d->fmt;
            p->min = d->min;
            p->max = d->max;
            p->def = d->def < d->min ? d->min : d->def > d->max ? d->max : d->def;
            p->names = d->names;
            p->unit = d->unit;
        }
    }
    classify(p, (sp->flags & MOD3_SF_FORMAT) && e->pformat);
    return (int32_t)(p - s->param);
}

/* the part's own (by P_* number) */
static int32_t add_part(mv_synth *s, uint32_t cap, uint32_t id, const char *pre)
{
    const mm_common *c = common_find(id);
    char key[8], name[40], label[16];
    const char *base, *lab;
    mv_param *p;
    int32_t at;
    if (!c)
        return -1;
    snprintf(key, sizeof key, "p%u", (unsigned)id);
    if ((at = param_index(s, key)) >= 0)
        return at;
    base = c->label;
    snprintf(name, sizeof name, "%s%s", pre, base);   /* (padd makes ATK ATTACK) */
    lab = c->label;                              /* (its page says LFO, ENV: not its knob) */
    if (!strncmp(lab, "LFO ", 4) || !strncmp(lab, "ENV ", 4))
        lab += 4;
    snprintf(label, sizeof label, "%s", lab);
    if (!(p = padd(s, cap, key, MV_SRC_PART, name, label)))
        return -1;
    p->pid = (uint16_t)id;
    p->fmt = c->fmt;
    p->min = c->min;
    p->max = c->max;
    p->def = c->def;
    p->names = c->names;
    classify(p, 0);
    return (int32_t)(p - s->param);
}

static int32_t add_edit(mv_synth *s, uint32_t cap, uint32_t k)   /* E1..E8 */
{
    const mod_param_v3 *d = &s->img.eng.edit[k];
    char key[8];
    mv_param *p;
    int32_t at;
    snprintf(key, sizeof key, "p%u", (unsigned)(MM_P_E0 + k));
    if ((at = param_index(s, key)) >= 0)
        return at;
    if (!(p = padd(s, cap, key, MV_SRC_PART, d->label, d->label)))
        return -1;
    p->pid = (uint16_t)(MM_P_E0 + k);
    p->fmt = d->fmt;
    p->min = d->min;
    p->max = d->max;
    p->def = d->def;
    p->names = d->names;
    p->unit = d->unit;
    classify(p, 0);
    return (int32_t)(p - s->param);
}

/* ---- levels: a schema section each (its FM-1 pages a row of four each), the engine's EDIT pages, ENV, LFO, VOICE */
static mv_level *ladd(mv_synth *s, uint32_t cap, const char *name)
{
    mv_level *l;
    char t[24];
    uint32_t i;
    snprintf(t, sizeof t, "%s", name);
    for (i = 0; t[i]; i++)
        if (t[i] >= 'a' && t[i] <= 'z')
            t[i] = (char)(t[i] - 32);
    if (s->nlevel >= cap)
        return 0;
    l = &s->level[s->nlevel];
    memset(l, 0, sizeof *l);
    snprintf(l->key, sizeof l->key, "l%u", (unsigned)s->nlevel);
    memcpy(l->name, t, sizeof l->name);
    s->nlevel++;
    return l;
}
/* a row of n knobs (MV_NONE: an empty one), a level's next: 0 when the level is full */
static int lrow(mv_level *l, const int32_t *k, uint32_t n)
{
    if (l->nknob + 4u > MV_LEVEL_KNOBS)
        return 0;
    for (uint32_t c = 0; c < 4u; c++)
        l->knob[l->nknob++] = (uint16_t)(c < n && k[c] >= 0 ? (uint32_t)k[c] : MV_NONE);
    return 1;
}
static int lrow_empty(const int32_t *k, uint32_t n)
{
    for (uint32_t c = 0; c < n; c++)
        if (k[c] >= 0)
            return 0;
    return 1;
}

/* the knobs Schwung's pages draw as an envelope or a filter (viz), by the FM-1's labels on one row: an envelope of two
 * stages or more, a filter of a cutoff and a resonance */
static void viz_row(mv_synth *s, const int32_t *k, uint32_t n, const char *group)
{
    static const struct { const char *label, *role; char kind; } ROLES[] = {
        {"ATTACK", "attack", 'e'}, {"ATK", "attack", 'e'}, {"DECAY", "decay", 'e'}, {"DEC", "decay", 'e'},
        {"SUSTAIN", "sustain", 'e'}, {"SUS", "sustain", 'e'}, {"RELEASE", "release", 'e'}, {"REL", "release", 'e'},
        {"HOLD", "hold", 'e'}, {"CUTOFF", "cutoff", 'c'}, {"CUT", "cutoff", 'c'}, {"RES", "resonance", 'r'},
        {"RESO", "resonance", 'r'},
        {"EMPH", "resonance", 'r'}};
    int32_t role[4] = {-1, -1, -1, -1};
    uint32_t ne = 0, nc = 0, nr = 0, c, i;
    for (c = 0; c < n; c++) {
        const mv_param *p = k[c] >= 0 ? &s->param[k[c]] : 0;
        if (!p || p->kind == MV_ENUM || p->vrole)
            continue;
        for (i = 0; i < sizeof ROLES / sizeof ROLES[0]; i++)
            if (!strcmp(p->label, ROLES[i].label)) {
                role[c] = (int32_t)i;
                ne += ROLES[i].kind == 'e', nc += ROLES[i].kind == 'c', nr += ROLES[i].kind == 'r';
                break;
            }
    }
    for (c = 0; c < n; c++) {
        char kind;
        if (role[c] < 0)
            continue;
        kind = ROLES[role[c]].kind;
        if (kind == 'e' ? ne < 2u : (nc != 1u || nr != 1u))
            continue;
        s->param[k[c]].vrole = ROLES[role[c]].role;
        snprintf(s->param[k[c]].vgroup, sizeof s->param[k[c]].vgroup, "%c%s", kind == 'e' ? 'e' : 'f', group);
    }
}

static int params_make(mv_synth *s)
{
    const mod_engine_v3 *e = &s->img.eng;
    const mm_schema *sc = &s->sch;
    static const uint32_t ENV1[] = {MM_P_ATK, MM_P_DEC, MM_P_SUS, MM_P_REL}, ENV2[] = {MM_P_ED_FLT, MM_P_ED_PIT,
                                                                                        MM_P_ED_SHP};
    static const uint32_t LFO1[] = {MM_P_LRATE, MM_P_LWAVE, MM_P_LFADE, MM_P_LSYNC},
                          LFO2[] = {MM_P_LD_PIT, MM_P_LD_FLT, MM_P_LD_SHP, MM_P_LD_AMP},
                          LFO3[] = {MM_P_LPHASE, MM_P_LTRIG, MM_P_LPOL};
    static const uint32_t VOICE1[] = {MM_P_VOICE, MM_P_GLIDE, MM_P_GLMODE, MM_P_DETUNE},
                          VOICE2[] = {MM_P_PRIO, MM_P_ALLOC, MM_P_LEVEL};
    static const struct { const char *name; const uint32_t *row[3]; uint32_t n[3]; } GROUPS[] = {
        {"ENV", {ENV1, ENV2, 0}, {4, 3, 0}},
        {"LFO", {LFO1, LFO2, LFO3}, {4, 4, 3}},
        {"VOICE", {VOICE1, VOICE2, 0}, {4, 3, 0}}};
    uint8_t covered[MOD3_P_COUNT] = {0};
    int own_env = e->ownenv != 0, own_lfo = 0, edit = !(sc->flags & MOD_SCH_F_NOEDIT);
    uint32_t pcap = sc->nparam + 8u + MM_NCOMMON + 1u, lcap = sc->npage + 16u, i, c;
    mv_level *l = 0, *more = 0;
    const char *lsec = 0;
    int32_t k[4];
    s->param = calloc(pcap, sizeof *s->param);
    s->level = calloc(lcap, sizeof *s->level);
    if (!s->param || !s->level)
        return 1;
    for (i = 0; i < sc->nparam; i++)
        if (sc->param[i].kind == MOD3_SK_TRACK)
            covered[sc->param[i].off] = 1;
    for (i = 0; i < sc->npage; i++) {            /* the schema's pages: a level for each section */
        const mm_sch_page *g = &sc->page[i];
        const char *sec = g->section[0] ? g->section : g->title;
        char vg[12];
        own_env |= (g->flags & MOD3_PG_ENV) != 0;
        own_lfo |= (g->flags & MOD3_PG_LFO) != 0;
        for (c = 0; c < 4u; c++) {
            const mm_sch_param *sp = g->id[c] == MOD_SCH_NONE ? 0 : mm_schema_find(sc, g->id[c]);
            k[c] = sch_shown(sp) ? add_sch(s, pcap, sp, g->title) : -1;
        }
        if (lrow_empty(k, 4))
            continue;
        snprintf(vg, sizeof vg, "%u", (unsigned)i);
        viz_row(s, k, 4, vg);
        if (!l || strcmp(lsec, sec)) {
            for (l = 0, c = 0; c < s->nlevel && !l; c++)   /* (a section again, further on: its level) */
                if (s->level[c].nknob && !strcasecmp(s->level[c].name, sec))
                    l = &s->level[c];
            if (!l && !(l = ladd(s, lcap, sec)))
                return 1;
            lsec = sec;
        }
        if (!lrow(l, k, 4)) {                    /* (a full level: a second) */
            char t[24];
            snprintf(t, sizeof t, "%.20s 2", l->name);
            if (!(l = ladd(s, lcap, t)) || !lrow(l, k, 4))
                return 1;
        }
    }
    for (i = 0; i < sc->nparam; i++) {           /* (shown, on no page: OTHER) */
        char key[8];
        int32_t at;
        snprintf(key, sizeof key, "s%u", (unsigned)sc->param[i].id);
        if (!sch_shown(&sc->param[i]) || param_index(s, key) >= 0 || (at = add_sch(s, pcap, &sc->param[i], 0)) < 0)
            continue;
        if (!more && !(more = ladd(s, lcap, "OTHER")))
            return 1;
        if (more->nknob < MV_LEVEL_KNOBS)
            more->knob[more->nknob++] = (uint16_t)at;
    }
    if (more)                                    /* (to whole rows) */
        while (more->nknob % 4u)
            more->knob[more->nknob++] = MV_NONE;
    for (uint32_t pg = 0; edit && pg < 2u; pg++) {   /* EDIT 1 (E1..E4), EDIT 2 (E5..E8): the engine's pages */
        const char *title = e->page_title[pg] ? e->page_title[pg] : pg ? "EDIT 2" : "EDIT 1";
        for (c = 0; c < 4u; c++) {
            const mod_param_v3 *d = &e->edit[pg * 4u + c];
            k[c] = covered[MM_P_E0 + pg * 4u + c] || !d->label || !strcmp(d->label, "-") || d->max <= d->min
                       ? -1 : add_edit(s, pcap, pg * 4u + c);
        }
        if (lrow_empty(k, 4))
            continue;
        viz_row(s, k, 4, pg ? "x2" : "x1");
        if (!(l = ladd(s, lcap, title)) || !lrow(l, k, 4))
            return 1;
    }
    for (uint32_t g = 0; g < sizeof GROUPS / sizeof GROUPS[0]; g++) {
        /* (a synth with its own LFO pages: the part's after them as TRACK LFO, as the FM-1's LFO button has them) */
        const char *pre = own_lfo && g == 1u ? "TRACK " : "";
        char name[24];
        if (g == 0u && own_env)
            continue;
        snprintf(name, sizeof name, "%s%s", pre, GROUPS[g].name);
        l = 0;
        for (uint32_t r = 0; r < 3u && GROUPS[g].row[r]; r++) {
            uint32_t n = GROUPS[g].n[r];
            for (c = 0; c < 4u; c++)
                k[c] = c < n && !covered[GROUPS[g].row[r][c]] ? add_part(s, pcap, GROUPS[g].row[r][c], pre) : -1;
            if (g == 2u && r == 1u) {            /* VOICE: the wheel's range after LEVEL */
                mv_param *b = padd(s, pcap, "bend", MV_SRC_BEND, "BEND", "BEND");
                if (b) {
                    b->fmt = MOD3_F_SEMI;
                    b->min = 0;
                    b->max = 24;
                    b->def = 2;
                    classify(b, 0);
                }
                k[3] = param_index(s, "bend");
            }
            if (lrow_empty(k, 4))
                continue;
            if (g == 0u)
                viz_row(s, k, 4, "a");
            if (!l && !(l = ladd(s, lcap, name)))
                return 1;
            if (!lrow(l, k, 4))
                return 1;
        }
    }
    return 0;
}

/* ---- values */
int32_t mv_value(mv_synth *s, const mv_param *p)
{
    if (p->src == MV_SRC_BEND)
        return s->part.bend_semis;
    if (p->src == MV_SRC_PART)
        return s->part.h.p[p->pid];
    return mm_sch_get(&s->part, s->part.patch, p->sp);
}
void mv_text(mv_synth *s, const mv_param *p, char *buf, uint32_t cap)
{
    int32_t v = mv_value(s, p);
    if (p->src == MV_SRC_SCH)
        mm_sch_text(&s->part, s->part.patch, p->sp, v, buf, cap);
    else
        mm_format(p->fmt, v, p->min, p->max, p->names, p->unit, buf, cap);
}
static void text_of(mv_synth *s, const mv_param *p, int32_t v, char *buf, uint32_t cap)
{
    if (p->src == MV_SRC_SCH)
        mm_sch_text(&s->part, s->part.patch, p->sp, v, buf, cap);
    else
        mm_format(p->fmt, v, p->min, p->max, p->names, p->unit, buf, cap);
}
int mv_get(mv_synth *s, const mv_param *p, char *buf, int cap)
{
    int32_t v = mv_value(s, p);
    if (p->kind == MV_ENUM)
        return snprintf(buf, (size_t)cap, "%d", (int)(v - p->min));
    if (p->kind == MV_FLOAT)
        return snprintf(buf, (size_t)cap, "%.4f", (double)(v - p->ofs) / p->div);
    return snprintf(buf, (size_t)cap, "%d", (int)(v - p->ofs));
}
/* set as the FM-1's pages do: the patch written, the module told of an edit that changed it (or one it asks to hear
 * of). An enum by its index or its text */
void mv_set(mv_synth *s, const mv_param *p, const char *val)
{
    char *end;
    double x = strtod(val, &end);
    int32_t v;
    if (end == val || *end) {                    /* not a number: an option's text */
        char t[32];
        if (p->kind != MV_ENUM)
            return;
        for (v = p->min; v <= p->max; v++) {
            text_of(s, p, v, t, sizeof t);
            if (!strcmp(t, val))
                break;
        }
        if (v > p->max)
            return;
    } else if (p->kind == MV_ENUM) {
        v = p->min + (int32_t)lround(x);
    } else {
        v = (int32_t)lround(x * p->div) + p->ofs;
    }
    v = v < p->min ? p->min : v > p->max ? p->max : v;
    if (p->src == MV_SRC_BEND) {
        s->part.bend_semis = v;
    } else if (p->src == MV_SRC_PART) {
        s->part.h.p[p->pid] = (int16_t)v;
    } else if (mm_sch_put(&s->part, s->part.patch, p->sp, v) || (p->sp->flags & MOD3_SF_NOTIFY)) {
        mm_patch_note(&s->part, MOD3_PATCH_EDIT, p->sp->id);
    }
}

/* ---- sounds */
static void sound_put(mv_synth *s, const int16_t *p, int32_t bend, const uint8_t *patch)
{
    memcpy(s->part.h.p, p, sizeof s->part.h.p);
    s->part.bend_semis = bend;
    mm_kill(&s->part);                           /* a new sound: its notes stop */
    if (s->file.patch_len)                       /* (the module may set the part's own values from it, correct it) */
        mm_patch_set(&s->part, patch, s->file.patch_len, MOD3_PATCH_NEW);
}
/* factory sound k: the part's own defaults, then its common block, as the device loads a sound; its patch over the
 * schema's default patch */
static int factory_sound(mv_synth *s, uint32_t k)
{
    uint8_t c[MM_PST_COMMON], patch[MOD3_PATCH_MAX], full[MOD3_PATCH_MAX];
    int16_t p[MOD3_P_COUNT];
    char name[MM_PST_NAME + 1] = "";
    uint32_t n = 0;
    if (k >= s->file.npresets || mm_file_sound(&s->file, k, name, 0, c, patch, sizeof patch, &n))
        return -1;
    mm_preset(&s->img.eng, p, (const int8_t *)c, c + 8, (int8_t)c[12], c[13]);
    memset(full, 0, sizeof full);
    if (s->file.patch_len) {
        memcpy(full, s->file.patch_def, s->file.patch_len);
        memcpy(full, patch, n < s->file.patch_len ? n : s->file.patch_len);
    }
    sound_put(s, p, 2, full);
    s->preset = (int32_t)k;
    s->preset_bank = 0;
    memcpy(s->preset_name, name, sizeof s->preset_name);
    return 0;
}
/* bank b's sound k (b 1..): a SysEx sound is the module's patch alone, so the part's own values are the file's first
 * sound's (its INIT), as a user slot's on the device start from a new sound's */
static int bank_sound(mv_synth *s, uint32_t b, uint32_t k)
{
    const mv_bank *bk = b && b <= s->nbank ? &s->bank[b - 1u] : 0;
    uint8_t c[MM_PST_COMMON], patch[MOD3_PATCH_MAX];
    int16_t p[MOD3_P_COUNT];
    uint32_t n = 0;
    if (!bk || k >= bk->n || !s->file.patch_len)
        return -1;
    if (s->file.npresets && !mm_file_sound(&s->file, 0, 0, 0, c, patch, sizeof patch, &n))
        mm_preset(&s->img.eng, p, (const int8_t *)c, c + 8, (int8_t)c[12], c[13]);
    else
        mm_defaults(&s->img.eng, p);
    memset(patch, 0, sizeof patch);
    memcpy(patch, bk->patch + (size_t)k * s->file.patch_len, s->file.patch_len);
    sound_put(s, p, 2, patch);
    s->preset = (int32_t)k;
    s->preset_bank = (int32_t)b;
    memcpy(s->preset_name, bk->sname[k], sizeof s->preset_name);
    return 0;
}
int mv_preset(mv_synth *s, uint32_t k)
{
    return s->bank_cur ? bank_sound(s, s->bank_cur, k) : factory_sound(s, k);
}
int mv_bank_pick(mv_synth *s, uint32_t b)
{
    if (b > s->nbank)
        return -1;
    s->bank_cur = b;
    return mv_preset(s, 0);
}
uint32_t mv_preset_count(const mv_synth *s)
{
    return s->bank_cur ? s->bank[s->bank_cur - 1u].n : s->file.npresets;
}
int32_t mv_preset_index(const mv_synth *s)
{
    return s->preset >= 0 && s->preset_bank == (int32_t)s->bank_cur ? s->preset : 0;
}

/* ---- SysEx banks */
typedef struct sx_take_ctx {
    mv_synth *s;
    uint8_t *patch;                              /* (grown) */
    uint32_t n, cap;
} sx_take_ctx;
static void sx_take(void *ctx, const uint8_t *patch, uint32_t len)
{
    sx_take_ctx *c = ctx;
    uint32_t pl = c->s->file.patch_len;
    if (c->n == c->cap) {
        uint32_t cap = c->cap ? 2u * c->cap : 32u;
        uint8_t *more = realloc(c->patch, (size_t)cap * pl);
        if (!more)
            return;
        c->patch = more, c->cap = cap;
    }
    memcpy(c->patch + (size_t)c->n * pl, c->s->file.patch_def, pl);   /* (a shorter offer: the rest as it starts) */
    memcpy(c->patch + (size_t)c->n * pl, patch, len < pl ? len : pl);
    c->n++;
}
static void clean_name(char *d, const char *src, uint32_t n, uint32_t cap)   /* (printable, no quote: JSON as is) */
{
    uint32_t k = 0;
    for (uint32_t i = 0; i < n && src[i] && k + 1u < cap; i++)
        d[k++] = src[i] < 32 || src[i] > 126 ? ' ' : src[i] == '"' || src[i] == '\\' ? '_' : src[i];
    while (k && d[k - 1u] == ' ')
        k--;
    d[k] = 0;
}
uint32_t mv_synth_sysex(mv_synth *s, const char *name, const uint8_t *data, uint32_t n, char *msg, uint32_t cap)
{
    sx_take_ctx c = {s, 0, 0, 0};
    mv_bank *more, *bk;
    if (msg && cap)
        msg[0] = 0;
    if (!s->file.patch_len || !s->sch.nsysex || !s->img.eng.sysex || s->nbank >= MV_MAX_BANKS)
        return 0;
    mm_sysex_file(&s->part, &s->sch, data, n, sx_take, &c, msg, cap);
    if (!c.n || !(more = realloc(s->bank, (s->nbank + 1u) * sizeof *more))) {
        free(c.patch);
        return 0;
    }
    s->bank = more;
    bk = &s->bank[s->nbank];
    memset(bk, 0, sizeof *bk);
    if (!(bk->sname = calloc(c.n, sizeof *bk->sname))) {
        free(c.patch);
        return 0;
    }
    clean_name(bk->name, name, MV_BANK_NAME, sizeof bk->name);
    bk->n = c.n;
    bk->patch = c.patch;
    for (uint32_t k = 0; k < c.n; k++) {         /* (the name in its patch, else the bank's and its number) */
        if (s->sch.nlen)
            clean_name(bk->sname[k], (const char *)bk->patch + (size_t)k * s->file.patch_len + s->sch.name,
                       s->sch.nlen, sizeof bk->sname[k]);
        if (!bk->sname[k][0])
            snprintf(bk->sname[k], sizeof bk->sname[k], "%.9s %u", bk->name, (unsigned)k + 1u);
    }
    s->nbank++;
    return c.n;
}

static const char HEX[] = "0123456789abcdef";
static int hex_in(const char *h, uint8_t *b, uint32_t cap)   /* bytes read, -1 not hex */
{
    uint32_t n = 0;
    for (; h[0] && h[1] && n < cap; h += 2) {
        const char *x = strchr(HEX, h[0] | 0x20), *y = strchr(HEX, h[1] | 0x20);
        if (!x || !y)
            return -1;
        b[n++] = (uint8_t)((x - HEX) << 4 | (y - HEX));
    }
    return (int)n;
}
int mv_state_get(mv_synth *s, char *buf, int cap)
{
    const char *bank = s->preset_bank > 0 && s->preset >= 0 ? s->bank[s->preset_bank - 1].name : "";
    int n = snprintf(buf, (size_t)cap, "{\"synth\":\"%s\",\"name\":\"%s\",\"bank\":\"%s\",\"preset\":%d,\"bend\":%d,"
                     "\"p\":\"", s->idhex, s->file.name, bank, (int)s->preset, (int)s->part.bend_semis);
    for (uint32_t i = 0; i < MOD3_P_COUNT && n + 5 < cap; i++) {
        uint16_t v = (uint16_t)s->part.h.p[i];
        buf[n++] = HEX[v >> 4 & 15u], buf[n++] = HEX[v & 15u], buf[n++] = HEX[v >> 12], buf[n++] = HEX[v >> 8 & 15u];
    }
    n += snprintf(buf + n, n < cap ? (size_t)(cap - n) : 0, "\",\"patch\":\"");
    for (uint32_t i = 0; i < s->file.patch_len && n + 3 < cap; i++)
        buf[n++] = HEX[s->part.patch[i] >> 4], buf[n++] = HEX[s->part.patch[i] & 15u];
    n += snprintf(buf + n, n < cap ? (size_t)(cap - n) : 0, "\"}");
    return n < cap ? n : -1;
}
void mv_state_set(mv_synth *s, const char *json)
{
    char t[2 * MOD3_PATCH_MAX + 8];
    uint8_t b[2 * MOD3_P_COUNT], patch[MOD3_PATCH_MAX], saved[MOD3_PATCH_MAX];
    int16_t p[MOD3_P_COUNT];
    int32_t bend = 2;
    int n;
    memcpy(p, s->part.h.p, sizeof p);
    if (!mv_json_field(json, "p", t, sizeof t) && (n = hex_in(t, b, sizeof b)) == (int)sizeof b)
        for (uint32_t i = 0; i < MOD3_P_COUNT; i++)
            p[i] = (int16_t)(b[2 * i] | b[2 * i + 1] << 8);
    for (uint32_t i = 0; i < s->nparam; i++) {   /* (the part's own it shows: in their ranges) */
        const mv_param *q = &s->param[i];
        if (q->src == MV_SRC_PART)
            p[q->pid] = (int16_t)(p[q->pid] < q->min ? q->min : p[q->pid] > q->max ? q->max : p[q->pid]);
    }
    if (!mv_json_field(json, "bend", t, sizeof t))
        bend = atoi(t);
    bend = bend < 0 ? 0 : bend > 24 ? 24 : bend;
    memset(patch, 0, sizeof patch);
    if (s->file.patch_len) {                     /* (a shorter patch saved: the rest as the schema starts it) */
        memcpy(patch, s->file.patch_def, s->file.patch_len);
        if (!mv_json_field(json, "patch", t, sizeof t) && (n = hex_in(t, saved, s->file.patch_len)) > 0)
            memcpy(patch, saved, (uint32_t)n);
    }
    sound_put(s, p, bend, patch);
    s->preset = -1;
    s->preset_bank = 0;
    s->preset_name[0] = 0;
    if (mv_json_field(json, "preset", t, sizeof t) || (n = atoi(t)) < 0)
        return;
    if (!mv_json_field(json, "bank", t, sizeof t) && t[0]) {   /* (its name: the sound it started from, the bank's
                                                                 * file here now; the Sound list shows that bank) */
        for (uint32_t i = 0; i < s->nbank; i++)
            if (!strcmp(s->bank[i].name, t) && (uint32_t)n < s->bank[i].n) {
                s->preset = n;
                s->preset_bank = (int32_t)i + 1;
                s->bank_cur = i + 1u;
                memcpy(s->preset_name, s->bank[i].sname[n], sizeof s->preset_name);
            }
    } else if ((uint32_t)n < s->file.npresets) {
        uint32_t len = 0;
        s->preset = n;
        s->bank_cur = 0;
        if (mm_file_sound(&s->file, (uint32_t)n, s->preset_name, 0, b, saved, sizeof saved, &len))
            s->preset_name[0] = 0;
    }
}

/* ---- notes */
void mv_midi(mv_synth *s, const uint8_t *b, int len)
{
    mm_part *t = &s->part;
    uint32_t st, d1, d2;
    if (len < 2 || b[0] < 0x80u || b[0] >= 0xF0u)
        return;
    st = b[0] & 0xF0u;
    d1 = b[1] & 127u;
    d2 = len > 2 ? b[2] & 127u : 0u;
    switch (st) {
    case 0x90:
        if (len < 3)
            break;
        if (d2)
            mm_note_on(t, d1, d2);
        else
            mm_note_off(t, d1);
        break;
    case 0x80:
        mm_note_off(t, d1);
        break;
    case 0xB0:
        switch (d1) {
        case 1: t->h.mw = (uint8_t)d2; break;
        case 2: t->h.breath = (uint8_t)d2; break;
        case 4: t->h.foot = (uint8_t)d2; break;
        case 64: mm_sustain(t, d2 >= 64u); break;
        case 65: t->h.porta = (uint8_t)d2; break;
        case 120: mm_kill(t); break;
        case 123: mm_all_off(t); break;
        default: break;
        }
        break;
    case 0xD0:
        t->h.at = (uint8_t)d1;
        break;
    case 0xA0:
        t->h.at = (uint8_t)d2;
        break;
    case 0xE0:
        if (len >= 3)
            mm_bend(t, (int32_t)((d2 << 7) | d1) - 8192);
        break;
    default:
        break;
    }
}

/* the device's mixer: LEVEL ((v - 112) / 2 dB), a mono part on both sides, a stereo one (host 5) its left side and
 * out - left */
void mv_render(mv_synth *s, int16_t *out, int frames)
{
    for (int f = 0; f < frames; f++) {
        if (s->tpos >= MM_CTL) {
            int st = mm_tick(&s->part, s->out), lv = s->part.h.p[MM_P_LEVEL];
            if (lv != s->glv)
                s->glv = lv, s->gk = lv > 0 ? powf(10.0f, (float)(lv - 112) / 40.0f) : 0.0f;
            for (uint32_t i = 0; i < MM_CTL; i++) {
                float l = (float)(st ? s->part.left[i] : s->out[i]), r = (float)(st ? s->out[i] - s->part.left[i]
                                                                                    : s->out[i]);
                long a = lrintf(s->gk * l), b = lrintf(s->gk * r);
                s->tl[i] = (int16_t)(a > 32767 ? 32767 : a < -32768 ? -32768 : a);
                s->tr[i] = (int16_t)(b > 32767 ? 32767 : b < -32768 ? -32768 : b);
            }
            s->tpos = 0;
        }
        out[2 * f] = s->tl[s->tpos];
        out[2 * f + 1] = s->tr[s->tpos];
        s->tpos++;
    }
}

/* ---- opening */
const char *mv_synth_open(mv_synth *s, const char *path)
{
    const char *err;
    memset(s, 0, sizeof *s);
    s->preset = -1;
    s->tpos = MM_CTL;
    s->glv = -1;
    if ((err = mm_file_read(&s->file, path)))
        return err;
    for (int i = 0; i < 8; i++)                  /* the ID as mod_build.py --check prints it */
        snprintf(s->idhex + 2 * i, 3, "%02x", s->file.id[7 - i]);
    if (mm_schema_read(&s->sch, &s->file))
        mm_schema_free(&s->sch);                 /* (a schema this reader cannot use: E1..E8 only) */
    if ((err = mm_image_load(&s->img, s->file.aa64, s->file.aa64_len)))
        goto fail;
    if (mm_part_open(&s->part, s->img.desc, &s->img.eng)) {
        err = "the module refused the host";
        goto fail;
    }
    if (s->file.patch_len)
        mm_patch_set(&s->part, s->file.patch_def, s->file.patch_len, MOD3_PATCH_NEW);
    if (s->file.npresets)                        /* a new part: the file's first sound */
        mv_preset(s, s->file.first < s->file.npresets ? s->file.first : 0);
    if (params_make(s)) {
        err = "out of memory";
        goto fail;
    }
    return 0;
fail:
    mv_synth_close(s);
    return err;
}

void mv_synth_close(mv_synth *s)
{
    if (s->part.desc)
        mm_part_close(&s->part);
    mm_image_free(&s->img);
    mm_schema_free(&s->sch);
    mm_file_free(&s->file);
    free(s->param);
    free(s->level);
    for (uint32_t i = 0; i < s->nbank; i++) {
        free(s->bank[i].patch);
        free(s->bank[i].sname);
    }
    free(s->bank);
    free(s->chain_params);
    free(s->hierarchy);
    free(s->bank_list);
    memset(s, 0, sizeof *s);
}

/* ---- what Schwung's pages are given */
static void param_json(mv_synth *s, mv_json *j, const mv_param *p)
{
    char t[32];
    mv_jf(j, ",{\"key\":\"%s\",\"name\":", p->key);
    mv_js(j, p->name);
    mv_jf(j, ",\"short_name\":");
    mv_js(j, p->label);
    if (p->kind == MV_ENUM) {
        mv_jf(j, ",\"type\":\"enum\",\"options\":[");
        for (int32_t v = p->min; v <= p->max; v++) {
            text_of(s, p, v, t, sizeof t);
            mv_jf(j, v > p->min ? "," : "");
            mv_js(j, t);
        }
        mv_jf(j, "],\"default\":%d", (int)(p->def - p->min));
    } else if (p->kind == MV_FLOAT) {
        mv_jf(j, ",\"type\":\"float\",\"min\":%.6g,\"max\":%.6g,\"step\":%.6g,\"default\":%.6g",
              (double)(p->min - p->ofs) / p->div, (double)(p->max - p->ofs) / p->div, 1.0 / p->div,
              (double)(p->def - p->ofs) / p->div);
    } else {
        mv_jf(j, ",\"type\":\"int\",\"min\":%d,\"max\":%d,\"default\":%d", p->min - p->ofs, p->max - p->ofs,
              p->def - p->ofs);
    }
    if (p->sunit) {
        mv_jf(j, ",\"unit\":");
        mv_js(j, p->sunit);
    }
    if (p->vrole)
        mv_jf(j, ",\"viz\":{\"group\":\"%s\",\"role\":\"%s\"}", p->vgroup, p->vrole);
    mv_jf(j, "}");
}
static void knobs_json(const mv_synth *s, mv_json *j, const uint16_t *k, uint32_t n)
{
    while (n && k[n - 1] == MV_NONE)             /* (no trailing holes) */
        n--;
    mv_jf(j, "[");
    for (uint32_t i = 0; i < n; i++)
        mv_jf(j, k[i] == MV_NONE ? "%snull" : "%s\"%s\"", i ? "," : "", k[i] == MV_NONE ? "" : s->param[k[i]].key);
    mv_jf(j, "]");
}

static int level_is_edit(const mv_synth *s, const mv_level *l)   /* (the engine's E1..E8) */
{
    for (uint32_t c = 0; c < l->nknob; c++)
        if (l->knob[c] != MV_NONE)
            return s->param[l->knob[c]].src == MV_SRC_PART && s->param[l->knob[c]].pid >= MM_P_E0;
    return 0;
}

int mv_chain_params(const mv_synth *s, char *buf, int cap)
{
    uint32_t n = mv_preset_count(s);
    int k = snprintf(buf, (size_t)cap, "[{\"key\":\"preset\",\"name\":\"Sound\",\"type\":\"int\",\"min\":0,\"max\":%u}",
                     (unsigned)(n ? n - 1u : 0u));
    if (k < 0 || (uint32_t)k + s->chain_len >= (uint32_t)cap)
        return -1;
    memcpy(buf + k, s->chain_params, s->chain_len + 1u);
    return k + (int)s->chain_len;
}

int mv_synth_contract(mv_synth *s, uint32_t nsynths)
{
    mv_json j = {0}, h = {0}, b = {0};
    uint16_t root[8];
    uint32_t nroot = 0, i;
    /* chain_params: (the Sound, mv_chain_params) the synth, the bank, then every parameter */
    mv_jf(&j, ",{\"key\":\"synth\",\"name\":\"Synth\",\"type\":\"int\",\"min\":0,\"max\":%u}",
          (unsigned)(nsynths ? nsynths - 1u : 0u));
    if (s->nbank)
        mv_jf(&j, ",{\"key\":\"bank\",\"name\":\"Bank\",\"type\":\"int\",\"min\":0,\"max\":%u}", (unsigned)s->nbank);
    for (i = 0; i < s->nparam; i++)
        param_json(s, &j, &s->param[i]);
    mv_jf(&j, "]");
    /* ui_hierarchy: root the sounds and eight knobs (the engine's EDIT pages where it has them, else the first
     * level's first two rows), then the synths, then a level for each */
    for (i = 0; i < s->nlevel && nroot < 8u; i++)
        if (level_is_edit(s, &s->level[i]))
            for (uint32_t c = 0; c < s->level[i].nknob && nroot < 8u; c++)
                root[nroot++] = s->level[i].knob[c];
    if (!nroot && s->nlevel)
        for (uint32_t c = 0; c < s->level[0].nknob && nroot < 8u; c++)
            root[nroot++] = s->level[0].knob[c];
    mv_jf(&h, "{\"levels\":{\"root\":{\"name\":");
    mv_js(&h, s->file.name);
    mv_jf(&h, ",\"list_param\":\"preset\",\"count_param\":\"preset_count\",\"name_param\":\"preset_name\",\"knobs\":");
    knobs_json(s, &h, root, nroot);
    mv_jf(&h, ",\"params\":[{\"level\":\"synths\",\"label\":\"Synth\"}");
    if (s->nbank)
        mv_jf(&h, ",{\"level\":\"banks\",\"label\":\"Bank\"}");
    for (i = 0; i < s->nlevel; i++) {
        mv_jf(&h, ",{\"level\":\"%s\",\"label\":", s->level[i].key);
        mv_js(&h, s->level[i].name);
        mv_jf(&h, "}");
    }
    mv_jf(&h, "]},\"synths\":{\"name\":\"Synth\",\"items_param\":\"synth_list\",\"select_param\":\"synth\","
              "\"navigate_to\":\"root\"}");
    if (s->nbank)                                /* (the factory sounds, then a bank a SysEx file) */
        mv_jf(&h, ",\"banks\":{\"name\":\"Bank\",\"items_param\":\"bank_list\",\"select_param\":\"bank\","
                  "\"navigate_to\":\"root\"}");
    mv_jf(&b, "[{\"index\":0,\"label\":\"Factory\"}");
    for (i = 0; i < s->nbank; i++) {
        mv_jf(&b, ",{\"index\":%u,\"label\":", (unsigned)i + 1u);
        mv_js(&b, s->bank[i].name);
        mv_jf(&b, "}");
    }
    mv_jf(&b, "]");
    for (i = 0; i < s->nlevel; i++) {
        const mv_level *l = &s->level[i];
        uint32_t first = 1;
        mv_jf(&h, ",\"%s\":{\"name\":", l->key);
        mv_js(&h, l->name);
        mv_jf(&h, ",\"knobs\":");
        knobs_json(s, &h, l->knob, l->nknob);
        mv_jf(&h, ",\"params\":[");
        for (uint32_t c = 0; c < l->nknob; c++)
            if (l->knob[c] != MV_NONE) {
                mv_jf(&h, "%s\"%s\"", first ? "" : ",", s->param[l->knob[c]].key);
                first = 0;
            }
        mv_jf(&h, "]}");
    }
    mv_jf(&h, "}}");
    if (j.err || h.err || b.err) {
        free(j.b);
        free(h.b);
        free(b.b);
        return -1;
    }
    free(s->chain_params);
    free(s->hierarchy);
    free(s->bank_list);
    s->chain_params = j.b;
    s->chain_len = j.n;
    s->hierarchy = h.b;
    s->hier_len = h.n;
    s->bank_list = b.b;
    s->bank_list_len = b.n;
    return 0;
}
