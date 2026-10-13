#!/usr/bin/env python3
"""Move's glibc is 2.34 (scripts/build.sh); reject newer symbol requirements."""
import re
import subprocess
import sys
output = subprocess.check_output(["readelf", "--version-info", sys.argv[1]], text=True)
versions = {tuple(map(int, s.split("."))) for s in re.findall(r"GLIBC_([0-9.]+)", output)}
assert versions and max(versions) <= (2, 34), f"Move-incompatible GLIBC: {sorted(versions)}"
header = subprocess.check_output(["readelf", "-h", sys.argv[1]], text=True)
assert "AArch64" in header
symbols = subprocess.check_output(["readelf", "-Ws", "--dyn-syms", sys.argv[1]], text=True)
assert "move_audio_fx_init_v2" in symbols
# Without this export the chain host never delivers MIDI (CC) to an audio FX.
assert "move_audio_fx_on_midi" in symbols
print("PASS: AArch64/audio FX v2 + on_midi exports, maximum required GLIBC", ".".join(map(str, max(versions))))
