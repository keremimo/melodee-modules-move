# Melodee Modules for Schwung

A Schwung sound generator for the Ableton Move that plays Melodee synth modules (`.mmod` files, ABI 3). Put module
files in its `Modules` folder; the module's Synth list picks one. The modules' code runs from each file's AA64 chunk,
as in the Melodee Modules CLAP (the `melodee-modules` submodule): build a file with `mod_build.py --native`.

## Install

Copy the `melodee-modules` folder from `dist/melodee-modules-module.tar.gz` to the Move:

    /data/UserData/schwung/modules/sound_generators/melodee-modules/

then copy module files into its `Modules` folder, and SysEx files (`.syx`) into its `SysEx` folder. The module reads
both folders when it is loaded into a slot: a file added later shows after the module is loaded again.

Load only module files you trust: the AA64 code runs in Schwung's process as any module's code does.

## On the Move

- **Synth**: every module file in `Modules`, by name (the newer version of a module found twice). Picking one keeps
  each synth's own sound while the slot is loaded.
- **Sound**: the sounds of the bank the Bank list shows (Schwung's preset list); at first the file's factory sounds.
- **Bank**: Factory, then a bank for each file in `SysEx` that the synth's module takes, by the file's name. Picking a
  bank loads its first sound. Each file is given to every synth's module as the FM-1 gives it SysEx that came in, and
  the sounds the module offers for user slots make the bank. HYDRA takes its own messages (`F0 7D 48 59 01`, one sound
  each, as an FM-1 sends them); PPG takes a PPG Wave 2.2 or 2.3 bank of the V8 OS (`F0 29 01 0D`, 100 programs,
  named PROGRAM 00..99). TRIO and WHEEL have no SysEx: no Bank list. A SysEx sound is the module's patch; the part's
  own values (ENV, LFO, VOICE) start from the file's first sound. A file no module takes is named in Schwung's log.
- **Pages**: a level for each section of the module's pages (HYDRA's OSC 1, MUTANT 1, MIXER, ..., MACROS), each
  FM-1 page one row of four knobs; then the engine's EDIT pages, ENV, LFO (TRACK LFO beside a synth's own LFO pages)
  and VOICE, as the FM-1 has them. The root's eight knobs are the engine's EDIT pages, else the first level's first
  two rows. Knob labels are the FM-1's; the header shows the whole name.
- **Values**: names as the FM-1 writes them (waves, modes, destinations), semitones, percent and dB in Schwung's
  units. Times, rates and cutoffs show the module's value (0-127): Schwung draws numbers itself and has no text from
  a module for them. A row with an ADSR, or a cutoff and a resonance, is declared to Schwung as an envelope or a
  filter picture (viz).
- **State**: the synth, its part's values and its patch, as Schwung saves a slot and a preset, and the bank and sound
  it came from (a set plays the same sound when its SysEx file is gone).

Everything after the module is loaded runs on Schwung's audio callback: the files are read, the code mapped, the
SysEx banks made and the pages built when the instance is made, so a Synth, a Bank, a Sound or a value is only
memory.

Not here, as in the CLAP: the modulation matrix, the arpeggiator, scales, chords, SPREAD, the FX, SysEx over MIDI,
and the modules' own drawing.

## Build

    git submodule update --init
    make             # build/<os>-<arch>/melodee-modules/ (dsp.so, module.json) and the test host
    make test        # the module as Schwung loads it, with the test modules from the submodule (a broken file and a
                     # second TRIO beside them): every synth, factory sound and parameter, the cutoff, the state, two
                     # instances, SysEx banks (HYDRA's messages, PPG banks, broken files; with PPG_BANK, a PPG Wave
                     # 2.2 factory bank against the factory programs); each synth's chain_params and ui_hierarchy
                     # checked by tests/check_contract.py
    make test-each   # each module file alone in Modules
    make move        # build/move-aarch64/ for the Move (Cortex-A72) in Docker (arm64v8/gcc:12-bookworm), tests run
    make dist        # dist/melodee-modules-module.tar.gz from the Move build: one melodee-modules/ folder

## Licence

GPL-3.0-only (`LICENSE`). `src/schwung_api.h` is MIT (`THIRD_PARTY.md`).
