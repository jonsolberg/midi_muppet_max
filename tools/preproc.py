# Part of MIDI Muppet Max – Copyright (C) 2026 Jon Solberg – GPL v3 or later, see LICENSE.

# Mimics the Arduino IDE: adds Arduino.h and inserts prototypes right before the first function.
import re, sys
src = open(sys.argv[1]).read()
lines = src.split('\n')
fn = re.compile(r'^(?:static\s+)?(?:const\s+)?[A-Za-z_][\w]*\s*\**\s*([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*\{')
protos, first = [], None
for i, l in enumerate(lines):
    m = fn.match(l)
    if m and m.group(1) not in ('if', 'while', 'for', 'switch'):
        if first is None: first = i
        if m.group(1) not in ('setup', 'loop'):
            protos.append(m.group(0)[:-1].rstrip() + ';')
out = ['#include <Arduino.h>', '#line 1 "%s"' % sys.argv[1]] + lines[:first] + protos + ['#line %d "%s"' % (first + 1, sys.argv[1])] + lines[first:]
print('\n'.join(out))
