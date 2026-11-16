#!/usr/bin/env python3
# Part of MIDI Muppet 4btn – Copyright (C) 2026 Jon Solberg – GPL v3 or later, see LICENSE.
"""Tests the MIDI Muppet config firmware in simavr through harness."""
import subprocess, sys

import os
HERE = os.path.dirname(os.path.abspath(__file__))
HARNESS = os.path.join(HERE, 'harness')
BUILD = os.path.join(HERE, '..', 'build')


class Sim:
    def __init__(self, elf, mcu, mhz=16):
        self.p = subprocess.Popen([HARNESS, elf, mcu], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1,
                                  env={**os.environ, 'SIM_MHZ': str(mhz)})
        self.cmd('baud 0')   # read USB and MIDI output alike (the speed check is tested in the browser test)
        self.tx = b''

    def cmd(self, c):
        self.p.stdin.write(c + '\n'); self.p.stdin.flush()

    def run(self, ms):
        self.cmd(f'run {ms}')
        out = b''
        while True:
            l = self.p.stdout.readline()
            if not l: raise RuntimeError('harness died')
            l = l.strip()
            if l.startswith('TX '): out += bytes.fromhex(l[3:])
            elif l == 'DONE': break
            elif l.startswith('CRASH'): raise RuntimeError(l)
        self.tx += out
        return out

    def take(self):
        t, self.tx = self.tx, b''
        return t

    def send(self, text): self.cmd('send ' + text)
    def pin(self, b, v): self.cmd(f'pin {b} {v}')

    def eeprom(self, n):
        self.cmd(f'eeprom {n}')
        while True:
            l = self.p.stdout.readline().strip()
            if l.startswith('EE '): return bytes.fromhex(l[3:])

    def reset(self): self.cmd('reset'); self.tx = b''

    def duty(self, ms):
        self.cmd(f'duty {ms}')
        while True:
            l = self.p.stdout.readline().strip()
            if l.startswith('DUTY '): return float(l[5:])

    def green(self):
        self.cmd('led')
        while True:
            l = self.p.stdout.readline().strip()
            if l.startswith('LED g='): return int(l[6:])

    def close(self): self.cmd('quit'); self.p.wait()


# ---- config blob (must match the firmware and the editor)
NONE, PC, CC, TOGGLE, MOMENTARY, NEXT, PREV, HXTUNER = range(8)


def blob(profile=0, channel=0, flags=3, red=64, long10=80, pos=(3, 2, 1, 0), actions=None):
    b = bytearray(b'M4') + bytes([1, profile, channel, flags, red, long10]) + bytes(pos)
    acts = actions or [(PC, 0, 1, 0, 0), (PC, 0, 5, 0, 0), (PC, 0, 0, 0, 0), (PC, 0, 4, 0, 0),
                       (PC, 0, 3, 0, 0), (PC, 0, 7, 0, 0), (PC, 0, 2, 0, 0), (PC, 0, 6, 0, 0)]
    for a in acts: b += bytes(a)
    assert len(b) == 52
    b.append((0x5A + sum(b)) & 0xFF)
    return bytes(b)


failures = 0


def check(name, got, want):
    global failures
    ok = got == want
    if not ok: failures += 1
    print(('PASS ' if ok else 'FAIL ') + name + ('' if ok else f'\n     got  {got!r}\n     want {want!r}'))


def setup(s):
    """Say hello like the editor does right after opening the port."""
    s.reset()
    for _ in range(10):
        s.send('MM?'); s.run(100)
    t = s.take()
    return t


def press(s, b, ms):
    s.pin(b, 0); s.run(ms); s.pin(b, 1); s.run(60)


def suite(elf, mcu, mhz):
    print(f'===== {mcu} {mhz} MHz')
    s = Sim(elf, mcu, mhz)

    # --- foot switch mode without the editor, default config
    s.run(2100); s.take()
    press(s, 1, 100)
    check('default: short press button 1 sends PC 1', s.take(), b'\xC0\x01')
    press(s, 1, 900)
    check('default: long press button 1 sends PC 5', s.take(), b'\xC0\x05')
    press(s, 4, 100)
    check('default: short press button 4 sends PC 2', s.take(), b'\xC0\x02')
    s.run(1000); s.take()
    check('nothing sent while idle', s.run(500), b'')

    # --- setup mode
    hello = setup(s)
    check('hello reply', hello.split(b'\r\n')[0], b'MM4 4 %s 53 %d' % (b'168' if '168' in mcu else b'328', mhz))
    s.send('R'); s.run(50)
    check('read default config', s.take().strip(), b'C ' + blob().hex().upper().encode())
    press(s, 3, 100)
    check('button events reported in setup mode', s.take(), b'P 3 1\r\nP 3 0\r\n')

    bad = bytearray(blob()); bad[20] ^= 1
    s.send('W ' + bad.hex()); s.run(50)
    check('bad checksum refused', s.take(), b'ERR invalid\r\n')
    s.send('W 1234'); s.run(50)
    check('short write refused', s.take(), b'ERR size\r\n')

    cfg = blob(channel=2, long10=50, actions=[
        (MOMENTARY, 0, 52, 127, 0), (CC, 0, 99, 1, 0),          # 1: momentary FS4, long ignored
        (TOGGLE, 5, 6, 127, 0), (NONE, 0, 0, 0, 0),             # 2: toggle CC6 on channel 5, no long -> acts on press
        (NEXT, 0, 0, 3, 0), (PREV, 0, 0, 3, 0),                 # 3: next / prev program in 0..3
        (CC, 0, 64, 127, 0), (HXTUNER, 0, 0, 0, 0)])            # 4: tap, tuner
    s.send('W ' + cfg.hex()); s.run(400)
    check('valid config saved', s.take(), b'OK\r\n')
    check('EEPROM holds the config', s.eeprom(53), cfg)
    s.send('R'); s.run(50)
    check('read back saved config', s.take().strip(), b'C ' + cfg.hex().upper().encode())
    s.send('Q'); s.run(700)
    check('leave setup mode', s.take().startswith(b'BYE'), True)

    # --- new config in action (channel 3 = 0x02)
    s.pin(1, 0); s.run(60)
    check('momentary: press sends on value at once', s.take(), b'\xB2\x34\x7F')
    s.run(1000)
    check('momentary: no long press action', s.take(), b'')
    s.pin(1, 1); s.run(60)
    check('momentary: release sends off value', s.take(), b'\xB2\x34\x00')
    s.pin(2, 0); s.run(40)
    check('toggle without long action acts on press, own channel 5', s.take(), b'\xB4\x06\x7F')
    s.pin(2, 1); s.run(60); press(s, 2, 50)
    check('toggle second press', s.take(), b'\xB4\x06\x00')
    out = b''
    for _ in range(5): press(s, 3, 80); out += s.take()
    check('next program wraps 0..3 (starts from program 0)', out, b'\xC2\x01\xC2\x02\xC2\x03\xC2\x00\xC2\x01')
    press(s, 3, 600)
    check('long press (500 ms) runs previous program', s.take(), b'\xC2\x00')
    press(s, 4, 100)
    check('short press with long action acts on release', s.take(), b'\xB2\x40\x7F')
    press(s, 4, 600)
    check('HX tuner action in generic profile sends CC 68', s.take(), b'\xB2\x44\x00')

    # --- config survives a restart
    s.reset(); s.run(2100); s.take()
    s.pin(1, 0); s.run(60)
    check('config kept after power cycle', s.take(), b'\xB2\x34\x7F')
    s.pin(1, 1); s.run(100); s.take()

    # --- HX Stomp profile
    setup(s)
    hx = blob(profile=1, channel=0, actions=[
        (NONE, 0, 0, 0, 0), (NONE, 0, 0, 0, 0), (NONE, 0, 0, 0, 0), (NONE, 0, 0, 0, 0),
        (CC, 0, 64, 127, 0), (HXTUNER, 0, 0, 0, 0), (TOGGLE, 0, 6, 127, 0), (TOGGLE, 0, 70, 0, 127)])
    s.send('W ' + hx.hex()); s.run(400); s.send('Q'); s.run(1000)
    check('HX: start in scroll mode sets stomp footswitch mode', s.take().split(b'BYE\r\n')[-1], b'\xB0\x47\x00')
    press(s, 1, 100)
    check('HX scroll: UP = next preset', s.take(), b'\xB0\x48\x7F')
    press(s, 2, 100)
    check('HX scroll: DN = previous preset', s.take(), b'\xB0\x48\x00')
    press(s, 2, 900)
    check('HX: long DN opens tuner', s.take(), b'\xB0\x44\x00')
    press(s, 4, 100)
    check('HX tuner: any short press closes tuner', s.take(), b'\xB0\x44\x00')
    press(s, 3, 900)
    check('HX: long TAP (tuner action) opens tuner', s.take(), b'\xB0\x44\x00')
    press(s, 1, 100)
    check('HX tuner: UP closes tuner', s.take(), b'\xB0\x44\x00')
    press(s, 3, 100)
    check('HX: TAP short = tap tempo', s.take(), b'\xB0\x40\x7F')
    press(s, 4, 900)
    check('HX: FOUR long = all bypass (bypass first)', s.take(), b'\xB0\x46\x00')
    press(s, 4, 900)
    check('HX: FOUR long again = all bypass on', s.take(), b'\xB0\x46\x7F')
    press(s, 1, 900); s.run(300)
    check('HX: long UP = snapshot mode', s.take(), b'\xB0\x47\x03')
    press(s, 1, 100)
    check('HX snapshot: UP = next snapshot', s.take(), b'\xB0\x45\x08')
    press(s, 1, 900); s.run(300)
    check('HX: long UP again = back to scroll', s.take(), b'\xB0\x47\x00')
    # both held -> FS mode
    s.pin(2, 0); s.run(50); s.pin(1, 0); s.run(1000); s.pin(1, 1); s.pin(2, 1); s.run(500)
    check('HX: hold UP+DN = FS mode', s.take(), b'\xB0\x47\x00')
    press(s, 1, 100)
    check('HX FS: UP = FS5', s.take(), b'\xB0\x35\x00')
    s.pin(2, 0); s.run(50); s.pin(1, 0); s.run(1000); s.pin(1, 1); s.pin(2, 1); s.run(800)
    check('HX: hold UP+DN again = looper mode', s.take(), b'\xB0\x47\x00')
    s.pin(2, 0); s.run(40)
    check('HX looper: DN records on press', s.take(), b'\xB0\x3C\x7F')
    s.pin(2, 1); s.run(60); press(s, 2, 60)
    check('HX looper: DN again = play', s.take(), b'\xB0\x3D\x7F')
    press(s, 1, 100)
    check('HX looper: UP = stop', s.take(), b'\xB0\x3D\x00')
    s.reset(); s.run(1450)
    check('power-up: nothing sent to MIDI while listening for the editor', s.take(), b'')
    s.run(850)
    check('HX: mode remembered after power cycle (looper)', s.take(), b'\xB0\x47\x00')
    s.pin(2, 0); s.run(40)
    check('HX: still in looper after restart', s.take(), b'\xB0\x3C\x7F')
    s.pin(2, 1); s.run(100)

    # --- original two button MIDI Muppet HX: switches 3 and 4 are not read
    setup(s)
    two = blob(profile=1, flags=3 | 4, actions=[(NONE, 0, 0, 0, 0)] * 4 + [(CC, 0, 64, 127, 0)] * 4)
    s.send('W ' + two.hex()); s.run(400); s.send('Q'); s.run(1000); s.take()
    press(s, 3, 100); press(s, 4, 900)
    check('two switches: switches 3 and 4 are ignored', s.take(), b'')
    press(s, 2, 100)
    check('two switches: DN still works (looper record or previous preset)', s.take() in (b'\xB0\x3C\x7F', b'\xB0\x48\x00'), True)
    # --- LED: light while held, common anode LED (lit when the pin is LOW)
    def led_setup(flags, red=255):
        setup(s)
        s.send('W ' + blob(flags=flags, red=red).hex()); s.run(400); s.send('Q'); s.run(900); s.take()
    led_setup(0x10)                       # hold mode, common cathode
    check('hold LED: off when idle (no start-up blinks)', s.green(), 0)
    s.pin(1, 0); s.run(100)
    check('hold LED: green while switch 1 is held', s.green(), 1)
    s.run(900)
    check('hold LED: green goes off once the long press starts (turns red)', s.green(), 0)
    s.pin(1, 1); s.run(100)
    check('hold LED: off after release', s.green(), 0)
    led_setup(0x10 | 0x08)                # hold mode, common anode
    check('common anode: green pin high (= off) when idle', s.green(), 1)
    s.pin(2, 0); s.run(100)
    check('common anode: green pin low (= lit) while held', s.green(), 0)
    s.pin(2, 1); s.run(100)
    check('common anode: off again after release', s.green(), 1)
    s.reset(); s.run(2100)
    check('common anode: off right after power-up', s.green(), 1)
    # green dimming: brightness 64 of 255 = 25 %
    led_setup(0x10, red=64)
    s.pin(1, 0); s.run(60)
    d = s.duty(100)
    check(f'green dimmed to 25 % while held (measured {d:.2f})', 0.20 < d < 0.30, True)
    s.pin(1, 1); s.run(60)
    check('dimmed green off after release', s.duty(50), 0.0)
    led_setup(0x10 | 0x08 | 0x20, red=64)    # common anode + idle glow
    s.pin(1, 0); s.run(60)
    d = s.duty(100)
    check(f'common anode: green pin low 25 % of the time while held (high {d:.2f})', 0.70 < d < 0.80, True)
    s.pin(1, 1); s.run(60)
    check('idle glow: green stays off (pin high on common anode)', s.duty(50), 1.0)
    s.take(); s.close()


if __name__ == '__main__':
    for mcu in ('atmega168', 'atmega328p'):
        for mhz in (16, 8):
            suite(os.path.join(BUILD, f'mm4config-{mcu}-{mhz}mhz.elf'), mcu, mhz)
    print('\nFAILURES:', failures)
    sys.exit(1 if failures else 0)
