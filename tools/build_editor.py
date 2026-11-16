#!/usr/bin/env python3
# Part of MIDI Muppet 4btn – Copyright (C) 2026 Jon Solberg – GPL v3 or later, see LICENSE.
"""Builds firmware hex files and editor/index.html with the firmware embedded."""
import json, os, subprocess, shutil
R = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
B = os.path.join(R, 'build'); os.makedirs(B, exist_ok=True)
for mcu in ('atmega168', 'atmega328p'):
    subprocess.run(['bash', os.path.join(R, 'tools', 'build.sh'), f'{R}/midi_muppet_4btn_config/midi_muppet_4btn_config.ino', mcu,
                    os.path.join(B, f'mm4config-{mcu}')], check=True, capture_output=True)
    shutil.copy(os.path.join(B, f'mm4config-{mcu}.hex'), f'{R}/firmware/midi_muppet_4btn_config-{mcu}.hex')
t = open(f'{R}/editor/editor.src.html', encoding='utf-8').read()
for mcu, tag in (('atmega168', 'HEX_ATMEGA168'), ('atmega328p', 'HEX_ATMEGA328P')):
    h = open(f'{R}/firmware/midi_muppet_4btn_config-{mcu}.hex').read()
    ph = f"/*{tag}*/''"
    assert t.count(ph) == 1
    t = t.replace(ph, json.dumps(h))
open(f'{R}/editor/index.html', 'w', encoding='utf-8').write(t)
print('editor/index.html', len(t), 'bytes')
