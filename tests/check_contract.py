#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
"""Each synth's contract as test_dsp wrote it (DIR/<name>.chain_params.json, DIR/<name>.ui_hierarchy.json) against
what Schwung's docs/MODULES.md asks of a sound generator: JSON, under 64 KB, keys once, every knob and list a key the
parameters have, real ranges, enums under 128 options with their default among them, the root a preset list, levels
reached from the root."""
import json
import pathlib
import sys

LIMIT = 64 * 1024
fails = []


def fail(name, text):
    fails.append(f"{name}: {text}")


def check(name, chain_text, hier_text):
    for what, text in (("chain_params", chain_text), ("ui_hierarchy", hier_text)):
        if len(text.encode()) >= LIMIT:
            fail(name, f"{what} is {len(text)} bytes")
    chain = json.loads(chain_text)
    hier = json.loads(hier_text)
    keys = {}
    for p in chain:
        k = p.get("key")
        if not k or k in keys:
            fail(name, f"key {k!r} missing or twice")
        keys[k] = p
        t = p.get("type")
        if t == "enum":
            o = p.get("options", [])
            if not 2 <= len(o) <= 128:
                fail(name, f"{k}: {len(o)} options")
            if not 0 <= p.get("default", 0) < len(o):
                fail(name, f"{k}: default {p.get('default')} of {len(o)}")
        elif t in ("int", "float"):
            if not p["min"] < p["max"]:
                fail(name, f"{k}: range {p['min']}..{p['max']}")
            d = p.get("default", p["min"])
            if not p["min"] - 1e-9 <= d <= p["max"] + 1e-9:
                fail(name, f"{k}: default {d} outside {p['min']}..{p['max']}")
            if t == "float" and not 0 < p.get("step", 0.01) <= p["max"] - p["min"]:
                fail(name, f"{k}: step {p.get('step')}")
            if p.get("unit") == "%" and t == "float" and p["max"] > 1:
                fail(name, f"{k}: a % float above 1 shows raw")
        else:
            fail(name, f"{k}: type {t!r}")
        if not p.get("name"):
            fail(name, f"{k}: no name")
        if len(p.get("short_name", "")) > 8:
            fail(name, f"{k}: short_name {p['short_name']!r}")
        viz = p.get("viz")
        if viz and (not viz.get("group") or not viz.get("role")):
            fail(name, f"{k}: viz {viz}")
    levels = hier.get("levels", {})
    root = levels.get("root")
    if not root:
        fail(name, "no root level")
        return
    for f in ("list_param", "count_param", "name_param"):
        if f not in root:
            fail(name, f"root has no {f}")
    if root.get("list_param") not in keys:
        fail(name, "the preset list's key is not a parameter")
    seen, todo = set(), ["root"]
    while todo:
        lk = todo.pop()
        if lk in seen:
            continue
        seen.add(lk)
        lv = levels.get(lk)
        if lv is None:
            fail(name, f"level {lk} linked, not there")
            continue
        knobs = lv.get("knobs", [])
        for k in knobs:
            if k is not None and k not in keys:
                fail(name, f"level {lk}: knob {k} is no parameter")
        if knobs and knobs[-1] is None:
            fail(name, f"level {lk}: a trailing empty knob")
        for k in ("list_param", "count_param", "name_param", "items_param", "select_param"):
            if k in lv and lv[k] in knobs:
                fail(name, f"level {lk}: selector {lv[k]} on a knob")
        if "select_param" in lv and lv["select_param"] not in keys:
            fail(name, f"level {lk}: select_param {lv['select_param']} is no parameter")
        for item in lv.get("params", []):
            if isinstance(item, str):
                if item not in keys:
                    fail(name, f"level {lk}: param {item} is no parameter")
            elif "level" in item:
                todo.append(item["level"])
            elif item.get("key") not in keys:
                fail(name, f"level {lk}: param {item}")
        if lk != "root" and not knobs and "items_param" not in lv:
            fail(name, f"level {lk}: no knobs")
        if lv.get("navigate_to") and lv["navigate_to"] not in levels:
            fail(name, f"level {lk}: navigate_to {lv['navigate_to']}")
    for lk in levels:
        if lk not in seen:
            fail(name, f"level {lk} is reached from nowhere")
    on_knob = {k for lv in levels.values() for k in lv.get("knobs", []) if k}
    for k in keys:
        if k not in on_knob and k not in ("preset", "synth", "bank"):
            fail(name, f"{k} is on no knob")
    print(f"{name}: {len(keys)} parameters, {len(levels)} levels, chain_params {len(chain_text)} bytes, "
          f"ui_hierarchy {len(hier_text)} bytes")


def main():
    d = pathlib.Path(sys.argv[1])
    names = sorted(p.name[: -len(".chain_params.json")] for p in d.glob("*.chain_params.json"))
    if not names:
        print("no contracts in", d)
        return 1
    for n in names:
        try:
            check(n, (d / f"{n}.chain_params.json").read_text(), (d / f"{n}.ui_hierarchy.json").read_text())
        except (ValueError, KeyError) as e:
            fail(n, f"{type(e).__name__}: {e}")
    for f in fails:
        print("FAIL", f)
    print(f"{len(fails)} FAILED" if fails else "contracts pass")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
