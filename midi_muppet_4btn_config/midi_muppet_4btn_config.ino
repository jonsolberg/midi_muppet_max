/**************************************************************************

  MIDI Muppet 4btn Config - universal firmware for the four button
  MIDI Muppet, set up from the web editor over USB
  ======================================================================

  Based on MIDI Muppet HX by Matthias Wientapper
  https://github.com/mattzzw/midi_muppet_hx_2btn

  Copyright (C) 2026 Jon Solberg

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program. If not, see <https://www.gnu.org/licenses/>.

  ------------------------------------------------------------------------
  The HX Stomp modes and MIDI helpers come from MIDI Muppet HX, used
  under the following licence:

  BSD 3-Clause License

  Copyright (c) 2020, Matthias Wientapper
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

  3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  ------------------------------------------------------------------------

  Hardware (same as the other MIDI Muppet 4btn sketches)
  --------
  - Buttons 1-4      D2, D3, D6, D8 (to ground, internal pull-ups)
  - LED green / red  D4, D5 (bicolor LED, common cathode, red dimmable)
  - MIDI out         TX (D1)
  - Board: Arduino Nano or Pro Mini, ATmega168 or ATmega328P,
    16 MHz (5 V) or 8 MHz (3.3 V Pro Mini) - build with the matching board setting
  - No extra libraries needed.
  - Also runs on the original two button MIDI Muppet HX (switches on D2 = UP
    and D3 = DN, same LED pins); choose "Original MIDI Muppet HX" in the editor.

  How it is set up
  ----------------
  All settings live in EEPROM and are written by the web editor:
  https://jonsolberg.se:8080/MidiMuppet/

  When the editor opens the USB port, the Arduino restarts. For the first
  1.5 seconds after power-up the firmware listens on USB at 115200 baud
  (57600 on 8 MHz boards).
  If the editor says hello it stays in setup mode (LED blinks green/red);
  otherwise it switches the serial port to MIDI speed and works as a
  foot switch. Nothing is sent to the MIDI socket while it listens.

  Setup protocol (text lines, newline terminated)
  -----------------------------------------------
  MM?          -> MM4 <firmware version> <168|328> <config size> <MHz>
  R            -> C <config as hex>
  W <hex>      -> OK | ERR <reason>   (saves to EEPROM)
  D            -> OK                  (back to defaults)
  L <1|2>      -> OK                  (flash green / red)
  Q            -> BYE                 (leave setup mode, start as foot switch)
  While in setup mode the firmware reports button presses as
  P <button 1-4> <1 down | 0 up>

  Profiles
  --------
  Generic:   every button has a short and a long press action.
  HX Stomp:  buttons 1 and 2 work as UP / DN of the original MIDI Muppet HX
             (scroll, footswitch, snapshot, looper and tuner modes, hold
             both to change mode); buttons 3 and 4 use their actions.

  Actions (5 bytes: type, channel, a, b, c; channel 0 = the global one)
  -------
  0 none
  1 Program Change       a = program
  2 Control Change       a = controller, b = value
  3 CC toggle            a = controller, b = first press, c = second press
  4 CC momentary         a = controller, b = on press, c = on release
                         (short press only; the button then has no long press)
  5 Next program         a = first, b = last (wraps around)
  6 Previous program     a = first, b = last (wraps around)
  7 HX tuner             toggles the HX Stomp tuner (CC 68) and, in the
                         HX Stomp profile, the tuner mode

**************************************************************************/

#include <EEPROM.h>

#define FW_VERSION 2

// ---------------------------------------------------------------- pins
const uint8_t BTN_PINS[4] = {2, 3, 6, 8};
#define LED_GRN 4
#define LED_RED 5

// ---------------------------------------------------------------- config
#define CFG_ADDR      0
#define CFG_SIZE      53
#define CFG_VERSION   1
#define HX_MODE_ADDR  100   // last HX Stomp mode, kept between power-ups

enum { P_MAGIC0 = 0, P_MAGIC1, P_VERSION, P_PROFILE, P_CHANNEL, P_FLAGS,
       P_RED, P_LONG, P_POS, P_ACTIONS = 12, P_CHECKSUM = 52 };

#define PROFILE_GENERIC 0
#define PROFILE_HX      1

#define FLAG_LED_FEEDBACK 0x01
#define FLAG_HX_LOOPER    0x02
#define FLAG_TWO_SWITCHES 0x04   // original two button MIDI Muppet HX: only switches 1 and 2

enum { A_NONE = 0, A_PC, A_CC, A_CC_TOGGLE, A_CC_MOMENTARY, A_PC_NEXT, A_PC_PREV, A_HX_TUNER };

uint8_t cfg[CFG_SIZE];

// HX Stomp profile modes (declared here so the Arduino IDE prototypes work)
enum modes_t {SCROLL, SNAPSHOT, FS, LOOPER, TUNER};
modes_t MODE, LAST_MODE, MODE_BEFORE_SNAPSHOT;
enum lmodes_t {PLAY, RECORD, OVERDUB, STOP};
lmodes_t LPR_MODE = STOP;

// one action slot per button and press: slot = button * 2 + (0 short, 1 long)
uint8_t *action(uint8_t slot) { return &cfg[P_ACTIONS + slot * 5]; }

uint8_t checksum(const uint8_t *c) {
  uint8_t s = 0x5A;
  for (uint8_t i = 0; i < P_CHECKSUM; i++) s += c[i];
  return s;
}

void setAction(uint8_t slot, uint8_t type, uint8_t ch, uint8_t a, uint8_t b, uint8_t c) {
  uint8_t *p = action(slot);
  p[0] = type; p[1] = ch; p[2] = a; p[3] = b; p[4] = c;
}

// Defaults: the same presets as the RC-500 / Terraform sketches
void defaultConfig() {
  memset(cfg, 0, CFG_SIZE);
  cfg[P_MAGIC0] = 'M'; cfg[P_MAGIC1] = '4'; cfg[P_VERSION] = CFG_VERSION;
  cfg[P_PROFILE] = PROFILE_GENERIC;
  cfg[P_CHANNEL] = 0;
  cfg[P_FLAGS] = FLAG_LED_FEEDBACK | FLAG_HX_LOOPER;
  cfg[P_RED] = 64;
  cfg[P_LONG] = 80;  // 800 ms
  for (uint8_t i = 0; i < 4; i++) cfg[P_POS + i] = i;
  const uint8_t progs[4][2] = {{1, 5}, {0, 4}, {3, 7}, {2, 6}};
  for (uint8_t b = 0; b < 4; b++) {
    setAction(b * 2, A_PC, 0, progs[b][0], 0, 0);
    setAction(b * 2 + 1, A_PC, 0, progs[b][1], 0, 0);
  }
  cfg[P_CHECKSUM] = checksum(cfg);
}

bool configValid(const uint8_t *c) {
  return c[P_MAGIC0] == 'M' && c[P_MAGIC1] == '4' && c[P_VERSION] == CFG_VERSION &&
         c[P_CHANNEL] <= 15 && c[P_PROFILE] <= PROFILE_HX && c[P_CHECKSUM] == checksum(c);
}

void loadConfig() {
  for (uint8_t i = 0; i < CFG_SIZE; i++) cfg[i] = EEPROM.read(CFG_ADDR + i);
  if (!configValid(cfg)) defaultConfig();
}

void saveConfig() {
  for (uint8_t i = 0; i < CFG_SIZE; i++) EEPROM.update(CFG_ADDR + i, cfg[i]);
}

// ---------------------------------------------------------------- MIDI
void midiProgChange(uint8_t ch, uint8_t p) {
  Serial.write(0xc0 | (ch & 0x0f));
  Serial.write(p & 0x7f);
}

void midiCtrlChange(uint8_t ch, uint8_t c, uint8_t v) {
  Serial.write(0xb0 | (ch & 0x0f));
  Serial.write(c & 0x7f);
  Serial.write(v & 0x7f);
}

uint8_t globalCh() { return cfg[P_CHANNEL]; }
void cc(uint8_t c, uint8_t v) { midiCtrlChange(globalCh(), c, v); }

// ---------------------------------------------------------------- LED
// Flashes run in the background so buttons never wait for the LED.
uint8_t flashLed = 0, flashLeft = 0;     // flashLeft counts on+off halves
unsigned long flashNext = 0;
#define FLASH_MS 70

void ledRed(bool on) {
  if (on) analogWrite(LED_RED, cfg[P_RED]);
  else digitalWrite(LED_RED, LOW);
}

void ledGreen(bool on) { digitalWrite(LED_GRN, on ? HIGH : LOW); }

void flash(uint8_t led, uint8_t times) {
  if (!(cfg[P_FLAGS] & FLAG_LED_FEEDBACK) || times == 0) return;
  flashLed = led;
  flashLeft = times * 2;
  flashNext = millis();
}

bool flashing() { return flashLeft > 0; }

void updateFlash() {
  if (!flashLeft || (long)(millis() - flashNext) < 0) return;
  bool on = (flashLeft % 2) == 0;
  ledGreen(false); ledRed(false);
  if (on) {
    if (flashLed == LED_RED) ledRed(true); else ledGreen(true);
  }
  flashLeft--;
  flashNext = millis() + FLASH_MS;
}

// Blocking flash, used only at power-up
void flashBlocking(uint8_t led, uint8_t times, uint16_t ms) {
  for (uint8_t i = 0; i < times; i++) {
    if (led == LED_RED) ledRed(true); else ledGreen(true);
    delay(ms);
    ledGreen(false); ledRed(false);
    delay(ms);
  }
}

// ---------------------------------------------------------------- buttons
#define DEBOUNCE_MS 20

struct Button {
  bool down;            // debounced state
  bool raw;
  unsigned long changed;
  unsigned long downAt;
  bool longDone;        // long press already handled (or skipped) for this press
};
Button btn[4];

uint16_t longMs() { return cfg[P_LONG] < 20 ? 200 : cfg[P_LONG] * 10; }

void (*onDown)(uint8_t b);
void (*onUp)(uint8_t b);
void (*onLong)(uint8_t b);

uint8_t switches() { return (cfg[P_FLAGS] & FLAG_TWO_SWITCHES) ? 2 : 4; }

void initButtons() {
  for (uint8_t i = 0; i < 4; i++) {
    pinMode(BTN_PINS[i], INPUT_PULLUP);
    btn[i].raw = btn[i].down = (digitalRead(BTN_PINS[i]) == LOW);
    btn[i].changed = millis();
    btn[i].longDone = true;   // a button held at power-up does nothing
  }
}

void readButtons() {
  unsigned long now = millis();
  for (uint8_t i = 0; i < switches(); i++) {
    Button &b = btn[i];
    bool raw = digitalRead(BTN_PINS[i]) == LOW;
    if (raw != b.raw) { b.raw = raw; b.changed = now; }
    if (raw != b.down && now - b.changed >= DEBOUNCE_MS) {
      b.down = raw;
      if (raw) { b.downAt = now; b.longDone = false; onDown(i); }
      else onUp(i);
    }
    if (b.down && !b.longDone && now - b.downAt >= longMs()) {
      b.longDone = true;
      onLong(i);
    }
  }
}

// ---------------------------------------------------------------- actions
uint8_t program = 0;            // last program sent, for next/previous
bool toggled[8];                // state of CC toggle actions

void hxToggleTuner();

// Runs an action. 'released' is true when a momentary button goes up.
void runAction(uint8_t slot, bool released) {
  uint8_t *p = action(slot);
  uint8_t ch = p[1] ? p[1] - 1 : globalCh();
  uint8_t led = (slot & 1) ? LED_RED : LED_GRN;

  switch (p[0]) {
    case A_PC:
      program = p[2];
      midiProgChange(ch, program);
      flash(led, program % 4 + 1);
      break;
    case A_CC:
      midiCtrlChange(ch, p[2], p[3]);
      flash(led, 1);
      break;
    case A_CC_TOGGLE:
      toggled[slot] = !toggled[slot];
      midiCtrlChange(ch, p[2], toggled[slot] ? p[3] : p[4]);
      flash(toggled[slot] ? LED_GRN : LED_RED, 1);
      break;
    case A_CC_MOMENTARY:
      midiCtrlChange(ch, p[2], released ? p[4] : p[3]);
      if (!released) flash(led, 1);
      break;
    case A_PC_NEXT:
    case A_PC_PREV: {
      uint8_t lo = min(p[2], p[3]), hi = max(p[2], p[3]);
      if (program < lo || program > hi) program = (p[0] == A_PC_NEXT) ? lo : hi;
      else if (p[0] == A_PC_NEXT) program = (program >= hi) ? lo : program + 1;
      else program = (program <= lo) ? hi : program - 1;
      midiProgChange(ch, program);
      flash(led, program % 4 + 1);
      break;
    }
    case A_HX_TUNER:
      if (cfg[P_PROFILE] == PROFILE_HX) hxToggleTuner();
      else { midiCtrlChange(ch, 68, 0); flash(led, 2); }
      break;
  }
}

bool isMomentary(uint8_t b) { return action(b * 2)[0] == A_CC_MOMENTARY; }
bool hasLong(uint8_t b) { return !isMomentary(b) && action(b * 2 + 1)[0] != A_NONE; }

// Generic button behaviour:
//  - momentary: on press and on release
//  - no long press action: acts at once on press
//  - otherwise: short action on release, long action after the long press time
void genericDown(uint8_t b) {
  if (isMomentary(b) || !hasLong(b)) {
    btn[b].longDone = true;
    runAction(b * 2, false);
  }
}

void genericUp(uint8_t b) {
  if (isMomentary(b)) runAction(b * 2, true);
  else if (hasLong(b) && !btn[b].longDone) {
    btn[b].longDone = true;
    runAction(b * 2, false);
  }
}

void genericLong(uint8_t b) {
  if (hasLong(b)) runAction(b * 2 + 1, false);
}

// ---------------------------------------------------------------- HX Stomp profile
// Buttons 1 and 2 are UP and DN of the original MIDI Muppet HX.
#define BTN_UP 0
#define BTN_DN 1


bool withLooper() { return cfg[P_FLAGS] & FLAG_HX_LOOPER; }

void hxEnterMode(modes_t m) {
  MODE = m;
  EEPROM.update(HX_MODE_ADDR, MODE);
  cc(71, MODE == SNAPSHOT ? 3 : 0);    // snapshot or stomp footswitch mode on the HX
  MODE_BEFORE_SNAPSHOT = MODE;
  LPR_MODE = STOP;
  if (MODE == LOOPER) {
    for (uint8_t i = 0; i < 6; i++) { ledGreen(i & 1); ledRed(!(i & 1)); delay(75); }
  } else {
    flashBlocking(MODE == SCROLL ? LED_RED : LED_GRN, 5, 30);
  }
}

void hxToggleTuner() {
  cc(68, 0);
  flash(LED_RED, 2);
  if (MODE == TUNER) MODE = LAST_MODE;
  else { LAST_MODE = MODE; MODE = TUNER; }
}

void hxLooperDn() {
  switch (LPR_MODE) {
    case STOP:    LPR_MODE = RECORD;  cc(60, 127); break;  // record
    case RECORD:  LPR_MODE = PLAY;    cc(61, 127); break;  // play
    case PLAY:    LPR_MODE = OVERDUB; cc(60, 0);   break;  // overdub
    case OVERDUB: LPR_MODE = PLAY;    cc(61, 127); break;  // play
  }
}

void hxClick(uint8_t b) {
  bool up = (b == BTN_UP);
  switch (MODE) {
    case SCROLL:   cc(72, up ? 127 : 0); flash(LED_RED, 2); break;   // next / previous preset
    case SNAPSHOT: cc(69, up ? 8 : 9);   flash(LED_RED, 2); break;   // next / previous snapshot
    case FS:       cc(up ? 53 : 52, 0);  flash(LED_RED, 2); break;   // FS5 / FS4
    case TUNER:    hxToggleTuner(); break;
    case LOOPER:
      if (up) {
        if (LPR_MODE == STOP) { LPR_MODE = PLAY; cc(61, 127); }
        else                  { LPR_MODE = STOP; cc(61, 0); }
      }
      break;
  }
}

void hxLong(uint8_t b) {
  if (b == BTN_DN) {
    if (btn[BTN_UP].down) return;           // both held: handled by UP
    if (MODE == SCROLL || MODE == SNAPSHOT || MODE == FS) hxToggleTuner();
    return;
  }
  if (btn[BTN_DN].down) {
    // both held: next mode
    btn[BTN_DN].longDone = true;
    switch (MODE) {
      case SCROLL:   hxEnterMode(FS); break;
      case FS:
      case SNAPSHOT: hxEnterMode(withLooper() ? LOOPER : SCROLL); break;
      case LOOPER:   cc(61, 0); hxEnterMode(SCROLL); break;   // stop the looper first
      case TUNER:    break;
    }
    return;
  }
  switch (MODE) {
    case SCROLL:
    case FS:
      MODE_BEFORE_SNAPSHOT = MODE;
      cc(71, 3);
      flash(LED_RED, 5);
      MODE = SNAPSHOT;
      EEPROM.update(HX_MODE_ADDR, MODE);
      break;
    case SNAPSHOT:
      cc(71, 0);
      flash(LED_RED, 5);
      MODE = (MODE_BEFORE_SNAPSHOT == FS) ? FS : SCROLL;
      EEPROM.update(HX_MODE_ADDR, MODE);
      break;
    case LOOPER:
      if (LPR_MODE == PLAY || LPR_MODE == STOP) { cc(63, 127); flash(LED_RED, 3); }  // undo/redo
      break;
    case TUNER:
      break;
  }
}

void hxDown(uint8_t b) {
  if (b >= 2) {                                   // buttons 3 and 4
    if (MODE == TUNER && action(b * 2)[0] != A_HX_TUNER) {
      btn[b].longDone = true;                     // any short press leaves the tuner
      hxToggleTuner();
      return;
    }
    genericDown(b);
    return;
  }
  if (b == BTN_DN && MODE == LOOPER && !btn[BTN_UP].down) {
    btn[b].longDone = true;                       // looper record reacts on press
    hxLooperDn();
  }
}

void hxUp(uint8_t b) {
  if (b >= 2) { genericUp(b); return; }
  if (!btn[b].longDone) {
    btn[b].longDone = true;
    hxClick(b);
  }
}

void hxLongPress(uint8_t b) {
  if (b >= 2) genericLong(b);
  else hxLong(b);
}

void hxLeds() {
  static unsigned long lastBlink = 0;
  switch (MODE) {
    case SCROLL:   ledGreen(false); ledRed(true); break;
    case SNAPSHOT:
    case FS:       ledRed(false); ledGreen(true); break;
    case TUNER:
      ledRed(false);
      if (millis() - lastBlink >= 500) { lastBlink = millis(); ledGreen(!digitalRead(LED_GRN)); }
      break;
    case LOOPER:
      ledGreen(LPR_MODE == PLAY || LPR_MODE == OVERDUB);
      ledRed(LPR_MODE == RECORD || LPR_MODE == OVERDUB);
      break;
  }
}

// ---------------------------------------------------------------- setup mode (USB)
#if F_CPU == 8000000L
#define SETUP_BAUD    57600     // 115200 is too far off at 8 MHz
#else
#define SETUP_BAUD    115200
#endif
#define MIDI_BAUD     31250
#define LISTEN_MS     1500

char line[2 * CFG_SIZE + 8];
uint8_t lineLen = 0;

// Reads a line from USB; returns true when a whole line is in 'line'
bool readLine() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { line[lineLen] = 0; lineLen = 0; return true; }
    if (lineLen < sizeof(line) - 1) line[lineLen++] = c;
    else lineLen = 0;            // too long: drop it
  }
  return false;
}

int8_t hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void printHex(uint8_t v) {
  const char *d = "0123456789ABCDEF";
  Serial.write(d[v >> 4]);
  Serial.write(d[v & 15]);
}

void sayHello() {
  Serial.print(F("MM4 "));
  Serial.print(FW_VERSION);
#if defined(__AVR_ATmega168__)
  Serial.print(F(" 168 "));
#else
  Serial.print(F(" 328 "));
#endif
  Serial.print(CFG_SIZE);
  Serial.print(' ');
  Serial.println(F_CPU / 1000000L);
}

// Handles one setup command. Returns false when setup mode should end.
bool setupCommand(const char *l) {
  if (!strcmp(l, "MM?")) { sayHello(); return true; }
  if (!strcmp(l, "R")) {
    Serial.print(F("C "));
    for (uint8_t i = 0; i < CFG_SIZE; i++) printHex(cfg[i]);
    Serial.println();
    return true;
  }
  if (l[0] == 'W' && l[1] == ' ') {
    uint8_t buf[CFG_SIZE];
    const char *h = l + 2;
    if (strlen(h) != 2 * CFG_SIZE) { Serial.println(F("ERR size")); return true; }
    for (uint8_t i = 0; i < CFG_SIZE; i++) {
      int8_t a = hexDigit(h[2 * i]), b = hexDigit(h[2 * i + 1]);
      if (a < 0 || b < 0) { Serial.println(F("ERR hex")); return true; }
      buf[i] = (a << 4) | b;
    }
    if (!configValid(buf)) { Serial.println(F("ERR invalid")); return true; }
    memcpy(cfg, buf, CFG_SIZE);
    saveConfig();
    Serial.println(F("OK"));
    flashBlocking(LED_GRN, 2, 80);
    return true;
  }
  if (!strcmp(l, "D")) { defaultConfig(); saveConfig(); Serial.println(F("OK")); return true; }
  if (l[0] == 'L' && l[1] == ' ') {
    flashBlocking(l[2] == '2' ? LED_RED : LED_GRN, 3, 120);
    Serial.println(F("OK"));
    return true;
  }
  if (!strcmp(l, "Q")) { Serial.println(F("BYE")); return false; }
  if (l[0]) Serial.println(F("ERR unknown"));
  return true;
}

void reportDown(uint8_t b) { Serial.print(F("P ")); Serial.print(b + 1); Serial.println(F(" 1")); }
void reportUp(uint8_t b)   { Serial.print(F("P ")); Serial.print(b + 1); Serial.println(F(" 0")); }
void reportLong(uint8_t b) { }

// Listens for the editor right after power-up. Returns true if it said hello.
bool editorPresent() {
  Serial.begin(SETUP_BAUD);
  unsigned long start = millis();
  while (millis() - start < LISTEN_MS) {
    if (readLine() && !strcmp(line, "MM?")) { sayHello(); return true; }
  }
  return false;
}

void setupMode() {
  onDown = reportDown; onUp = reportUp; onLong = reportLong;
  unsigned long lastBlink = 0;
  bool phase = false;
  for (;;) {
    if (readLine() && !setupCommand(line)) break;
    readButtons();
    if (millis() - lastBlink >= 400) {        // green/red blink: setup mode
      lastBlink = millis();
      phase = !phase;
      ledGreen(phase); ledRed(!phase);
    }
  }
  ledGreen(false); ledRed(false);
  Serial.flush();
}

// ---------------------------------------------------------------- main
void startFootSwitch() {
  Serial.end();
  Serial.begin(MIDI_BAUD);
  memset(toggled, 0, sizeof(toggled));
  initButtons();
  if (cfg[P_PROFILE] == PROFILE_HX) {
    onDown = hxDown; onUp = hxUp; onLong = hxLongPress;
    uint8_t m = EEPROM.read(HX_MODE_ADDR);
    if (m > LOOPER || (m == LOOPER && !withLooper())) m = SCROLL;
    hxEnterMode((modes_t)m);
  } else {
    onDown = genericDown; onUp = genericUp; onLong = genericLong;
    flashBlocking(LED_GRN, 3, 60);
  }
}

void setup() {
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GRN, OUTPUT);
  loadConfig();
  initButtons();
  if (editorPresent()) setupMode();
  startFootSwitch();
}

void loop() {
  readButtons();
  if (flashing()) updateFlash();
  else if (cfg[P_PROFILE] == PROFILE_HX) hxLeds();
}
