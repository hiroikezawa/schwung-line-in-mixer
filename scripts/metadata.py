#!/usr/bin/env python3
"""Embed module.json's contract in the DSP (runtime callbacks never read files)
and check that the C parameter table (PDEF in src/line_in_mixer.c) agrees with
module.json on every key, range, default and enum option list.

    scripts/metadata.py <out.h>

SPDX-License-Identifier: MIT
Copyright (c) 2026 Hiro Ikezawa
"""
import json
import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parent.parent
caps = json.loads((root / "module.json").read_text())["capabilities"]
src = (root / "src" / "line_in_mixer.c").read_text()

params = {p["key"]: p for p in caps["chain_params"]}
assert not any(p.get("access") == "read" for p in caps["chain_params"]), "no readouts"

enum_names = {n: re.findall(r'"([^"]+)"', body)
              for n, body in re.findall(r"static const char \*const (OPT_\w+)\[\] = \{([^}]*)\}", src)}
consts = {"SRC_ANALOG": 0}
rows = re.findall(r'\[P_\w+\] = \{"(\w+)", (-?[\d.]+), (-?[\d.]+), (-?\w+), ([01]), (\w+)\}', src)
assert len(rows) == 13, f"PDEF rows found: {len(rows)}"
writable = [p["key"] for p in caps["chain_params"] if p.get("access") != "read"]
assert [r[0] for r in rows] == writable, "PDEF order must equal module.json's writable chain_params"
for key, lo, hi, dflt, integer, opts in rows:
    p = params[key]
    dflt = consts.get(dflt, dflt)
    if p["type"] == "enum":
        assert opts != "NULL" and enum_names[opts] == p["options"], f"{key}: options differ"
        assert (float(lo), float(hi)) == (0, len(p["options"]) - 1), f"{key}: enum range"
    else:
        assert (float(lo), float(hi)) == (p["min"], p["max"]), f"{key}: range {lo}..{hi} vs {p['min']}..{p['max']}"
        assert (integer == "1") == (p["type"] == "int"), f"{key}: int/float"
    assert float(dflt) == p["default"], f"{key}: default {dflt} vs {p['default']}"

levels = caps["ui_hierarchy"]["levels"]
assert levels["root"]["knobs"] == ["mix", "an_level", "usb_level", "source", "an_gain", "usb_gain", "mute"]
assert levels["tone"]["knobs"] == ["an_hpf", "usb_hpf", "an_lpf", "usb_lpf", "an_pan", "usb_pan"]


def literal(value):
    return json.dumps(json.dumps(value, separators=(",", ":")), ensure_ascii=True)


pathlib.Path(sys.argv[1]).write_text("\n".join([
    "/* Generated at build time from module.json; edit that instead. */",
    "static const char lim_chain_params[] = " + literal(caps["chain_params"]) + ";",
    "static const char lim_ui_hierarchy[] = " + literal(caps["ui_hierarchy"]) + ";",
]) + "\n")
