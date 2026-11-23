/* SPDX-License-Identifier: MIT
 * Copyright (c) 2025-2026 Charles Vestal */
/* Schwung's module ABI as a sound generator is built against it: the host table a module is given and the v2 plug-in
 * table it gives back. Declared field for field from Schwung's src/host/plugin_api_v1.h (MIT, THIRD_PARTY.md): the
 * layout is the contract. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define MOVE_SAMPLE_RATE 44100
#define MOVE_FRAMES_PER_BLOCK 128
#define MOVE_MIDI_SOURCE_INTERNAL 0
#define MOVE_MIDI_SOURCE_EXTERNAL 2
#define MOVE_MIDI_SOURCE_HOST 3
#define MOVE_MIDI_SOURCE_FX_BROADCAST 4          /* (for the audio effects only: not the synth's) */
#define MOVE_MIDI_SOURCE_TOUCH 5

typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
    int (*get_clock_status)(void);
    void *mod_emit_value;
    void *mod_clear_source;
    void *mod_host_ctx;
    float (*get_bpm)(void);
    int (*midi_inject_to_move)(const uint8_t *msg, int len);
    int (*slot_recv_channel)(void *instance);
    double (*get_beat_position)(void);
    void *reserved[8];
} host_api_v1_t;
_Static_assert(offsetof(host_api_v1_t, reserved) == 120, "Schwung's host table layout");

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void *(*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

typedef plugin_api_v2_t *(*move_plugin_init_v2_fn)(const host_api_v1_t *host);

#define MOVE_PLUGIN_API_VERSION_2 2
