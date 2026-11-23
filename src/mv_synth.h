/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* One synth module file (.mmod) as a Schwung sound generator plays it: the file, its own copy of the AArch64 code
 * (melodee-modules' mm_image.h), the part that plays it (mm_engine.h), and what Schwung's pages are given (its
 * chain_params and ui_hierarchy, built when it is opened). Every call after mv_synth_open is Schwung's audio callback's:
 * no file reads, no allocation. */
#pragma once
#include <stdint.h>
#include "mm_engine.h"
#include "mm_file.h"
#include "mm_image.h"
#include "mm_schema.h"

enum { MV_SRC_PART, MV_SRC_BEND, MV_SRC_SCH };
enum { MV_INT, MV_FLOAT, MV_ENUM };

typedef struct mv_param {
    char key[8];                                 /* "s<schema id>", "p<P_* number>", "bend" */
    uint8_t src;                                 /* MV_SRC_* */
    uint8_t kind;                                /* MV_* as Schwung is told */
    uint16_t pid;                                /* MV_SRC_PART: its P_* number */
    const mm_sch_param *sp;                      /* MV_SRC_SCH: its schema record */
    uint8_t fmt;                                 /* MOD3_F_* (the module's descriptor's when it has one) */
    int16_t min, max, def;                       /* the module's values */
    const char *const *names;
    const char *unit;                            /* the module's */
    int16_t ofs;                                 /* Schwung's value: (v - ofs) / div */
    uint16_t div;
    const char *sunit;                           /* Schwung's unit (0 none) */
    char name[40], label[16];                    /* its whole name, its knob's */
    const char *vrole;                           /* Schwung's picture of it (viz): its role in vgroup, 0 none */
    char vgroup[16];
} mv_param;

#define MV_LEVEL_KNOBS 64u
#define MV_NONE 0xFFFFu
typedef struct mv_level {
    char key[12], name[24];
    uint16_t knob[MV_LEVEL_KNOBS];               /* params by index, MV_NONE an empty knob; four to a row */
    uint16_t nknob;
} mv_level;

/* a bank of sounds from a SysEx file (.syx): what the module offered for user slots when it was given the file */
#define MV_BANK_NAME 64u
#define MV_MAX_BANKS 127u                        /* (Schwung's lists: 128 at most, the factory sounds one of them) */
typedef struct mv_bank {
    char name[MV_BANK_NAME];                     /* its file's name, without .syx */
    uint32_t n;                                  /* its sounds */
    uint8_t *patch;                              /* n patches of the file's patch_len bytes */
    char (*sname)[MM_PST_NAME + 1];              /* their names */
} mv_bank;

typedef struct mv_synth {
    mm_file file;
    mm_schema sch;
    mm_image img;
    char idhex[17];
    mv_param *param;
    uint32_t nparam;
    mv_level *level;
    uint32_t nlevel;
    mv_bank *bank;                               /* bank b (1..nbank) at bank[b - 1]; bank 0 the factory sounds */
    uint32_t nbank, bank_cur;                    /* the bank the Sound list shows */
    char *chain_params, *hierarchy, *bank_list;  /* (JSON; chain_params from the synth on: mv_chain_params) */
    uint32_t chain_len, hier_len, bank_list_len;
    int32_t preset, preset_bank;                 /* the sound last loaded (-1 none) and its bank */
    char preset_name[MM_PST_NAME + 1];
    int32_t out[MM_CTL];                         /* the module's block, and as int16 at its level */
    int16_t tl[MM_CTL], tr[MM_CTL];
    uint32_t tpos;
    int32_t glv;
    float gk;
    mm_part part;                                /* (last) */
} mv_synth;

/* path into s (calloc'd by the caller, never moved: the module keeps its part): 0, else what is wrong */
const char *mv_synth_open(mv_synth *s, const char *path);
void mv_synth_close(mv_synth *s);
/* SysEx file name's n bytes (data) to the synth's module: the sounds it offers become a bank named for the file. Their
 * count (0: no bank), the module's last message in msg (cap bytes) */
uint32_t mv_synth_sysex(mv_synth *s, const char *name, const uint8_t *data, uint32_t n, char *msg, uint32_t cap);
/* its chain_params and ui_hierarchy (JSON), once every synth is open and has its banks (nsynths of them): 0, else -1 */
int mv_synth_contract(mv_synth *s, uint32_t nsynths);
int mv_chain_params(const mv_synth *s, char *buf, int cap);   /* (the Sound's range is the bank's) */

const mv_param *mv_find(const mv_synth *s, const char *key);
/* a parameter's value as Schwung reads and writes it (text), and its text as the FM-1 shows it */
int mv_get(mv_synth *s, const mv_param *p, char *buf, int cap);
void mv_set(mv_synth *s, const mv_param *p, const char *val);
void mv_text(mv_synth *s, const mv_param *p, char *buf, uint32_t cap);
int32_t mv_value(mv_synth *s, const mv_param *p); /* (the module's value) */

int mv_preset(mv_synth *s, uint32_t k);          /* the Sound list's sound k (bank_cur's): 0, else -1 */
int mv_bank_pick(mv_synth *s, uint32_t b);       /* bank b to the Sound list, its first sound loaded: 0, else -1 */
uint32_t mv_preset_count(const mv_synth *s);     /* (bank_cur's) */
int32_t mv_preset_index(const mv_synth *s);      /* the loaded sound's place in the Sound list (0 another bank's) */
/* its sound as text ({"p":hex,"bend":n,"patch":hex,"preset":k,"bank":name}) and back */
int mv_state_get(mv_synth *s, char *buf, int cap);
void mv_state_set(mv_synth *s, const char *json);

void mv_midi(mv_synth *s, const uint8_t *msg, int len);
/* frames of stereo at 44.1 kHz (a multiple of MM_CTL), added to nothing: out written */
void mv_render(mv_synth *s, int16_t *out, int frames);

/* (JSON helpers, shared with dsp.c) */
typedef struct mv_json {
    char *b;
    uint32_t n, cap;
    int err;
} mv_json;
void mv_jf(mv_json *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void mv_js(mv_json *j, const char *s);           /* a quoted string */
/* the value of "key" in a flat JSON object: a string (unquoted) or a number, into buf; 0 found */
int mv_json_field(const char *json, const char *key, char *buf, uint32_t cap);
