#!/usr/bin/env python3
# Part of MIDI Muppet Max – Copyright (C) 2026 Jon Solberg – GPL v3 or later, see LICENSE.
"""Builds firmware hex files and editor/index.html with the firmware embedded."""
import json, os, subprocess, shutil
R = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
B = os.path.join(R, 'build'); os.makedirs(B, exist_ok=True)
VARIANTS = [(mcu, mhz) for mcu in ('atmega168', 'atmega328p') for mhz in (16, 8)]
for mcu, mhz in VARIANTS:
    name = f'mmmaxconfig-{mcu}-{mhz}mhz'
    subprocess.run(['bash', os.path.join(R, 'tools', 'build.sh'), f'{R}/midi_muppet_max_config/midi_muppet_max_config.ino', mcu,
                    os.path.join(B, name)], check=True, capture_output=True, env={**os.environ, 'FCPU': f'{mhz}000000L'})
    shutil.copy(os.path.join(B, name + '.hex'), f'{R}/firmware/midi_muppet_max_config-{mcu}-{mhz}mhz.hex')
t = open(f'{R}/editor/editor.src.html', encoding='utf-8').read()
for mcu, mhz in VARIANTS:
    h = open(f'{R}/firmware/midi_muppet_max_config-{mcu}-{mhz}mhz.hex').read()
    ph = f"/*HEX_{mcu.upper()}_{mhz}*/''"
    assert t.count(ph) == 1, ph
    t = t.replace(ph, json.dumps(h))
open(f'{R}/editor/index.html', 'w', encoding='utf-8').write(t)
print('editor/index.html', len(t), 'bytes')
