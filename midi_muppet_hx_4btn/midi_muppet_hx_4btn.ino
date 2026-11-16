/**************************************************************************

  MIDI Muppet HX 4btn - four button MIDI foot switch for the Line 6 HX Stomp
  =========================================================================

  Based on MIDI Muppet HX (2 buttons) by Matthias Wientapper
  https://github.com/mattzzw/midi_muppet_hx_2btn

  Four button version and changes:
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
  Original MIDI Muppet HX code, used under the following licence:

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

  Hardware
  --------
  - Button UP / DN      D2, D3   (the two buttons from the original MIDI Muppet)
  - Button TAP          D6       click: tap tempo, long press: tuner
  - Button FOUR         D8       click: toggle CC 6, long press: toggle all bypass
  - LED green / red     D4, D5   (bicolor LED, common cathode)
  - MIDI out            TX (D1)
  - Board: Arduino Nano or Pro Mini (ATmega168 or ATmega328P)

  Libraries
  ---------
  - OneButton  https://github.com/mathertel/OneButton
  - JC_Button  https://github.com/JChristensen/JC_Button

  Two different button libraries are used on purpose:
    - OneButton detects a long press without detecting a short press first.
      The click action is therefore evaluated when the button is released,
      which is fine for most cases but bad for timing critical actions
      like the looper.
    - JC_Button detects a press as soon as the button goes down.
      It is only used for the DN button in looper mode.

  Modes (UP / DN buttons)
  -----------------------
  SCROLL Mode:   up/dn: next/previous preset
                 long press dn: TUNER
                 long press up: SNAPSHOT mode
  FS Mode:       up/dn emulate FS5/FS4
                 long press dn: TUNER
                 long press up: SNAPSHOT mode
  SNAPSHOT Mode: up/dn: next/previous snapshot
                 long press dn: TUNER
                 long press up: back to SCROLL or FS mode
  TUNER Mode:    up, dn or long press TAP: back to previous mode
  LOOPER Mode:   dn toggles record/overdub
                 up toggles play/stop
                 long press up toggles undo/redo

  Hold up + dn:  cycle SCROLL -> FS -> LOOPER (if enabled) -> SCROLL

  Configuration at power-up
  -------------------------
  Hold dn while powering up: set MIDI channel (up/dn = +/-, hold both to exit)
  Hold up while powering up: enable/disable looper mode (up = on, dn = off,
                             hold both to exit)

**************************************************************************/

#include <OneButton.h>
#include <JC_Button.h>
#include <EEPROM.h>

// GPIO pins used
#define BTN_UP   2
#define BTN_DN   3
#define LED_GRN  4
#define LED_RED  5
#define BTN_TAP  6
#define BTN_FOUR 8

// EEPROM addresses
#define OP_MODE_ADDR      0  // stores the mode of operation
#define LOOPER_MODE_ADDR  1  // stores if looper control is enabled
#define MIDI_CHANNEL_ADDR 2  // stores the MIDI channel

// Adjust red LED brightness 0-255 (full on was way too bright)
#define LED_RED_BRIGHTNESS 1
// on/off delay when we flash a LED
#define LED_FLASH_DELAY    30
// time the buttons wait before reporting a click (ms)
#define CLICK_MS           50

// HX Stomp MIDI CC numbers
#define CC_FS4          52  // emulate footswitch 4
#define CC_FS5          53  // emulate footswitch 5
#define CC_LOOPER_REC   60  // 64-127: record, 0-63: overdub
#define CC_LOOPER_PLAY  61  // 64-127: play, 0-63: stop
#define CC_LOOPER_UNDO  63  // undo/redo
#define CC_TAP_TEMPO    64
#define CC_TUNER        68  // any value toggles the tuner
#define CC_SNAPSHOT     69  // 8: next snapshot, 9: previous snapshot
#define CC_ALL_BYPASS   70  // 0-63: bypass, 64-127: on
#define CC_FS_MODE      71  // 0: stomp mode, 3: snapshot mode
#define CC_PRESET       72  // 64-127: next preset, 0-63: previous (firmware 3.10+)
#define CC_FOUR_TOGGLE  6   // free CC, assign it to a block/parameter on the HX Stomp

OneButton btnUp(BTN_UP, true);
OneButton btnDn(BTN_DN, true);
OneButton btnTap(BTN_TAP, true);
OneButton btnFour(BTN_FOUR, true);

Button jc_btnDn(BTN_DN);   // used in looper mode only

enum modes_t {SCROLL, SNAPSHOT, FS, LOOPER, TUNER, CHANNEL_CFG, LOOPER_CFG};  // modes of operation
static modes_t MODE;                  // current mode
static modes_t LAST_MODE;             // mode to return to (from tuner or configuration)
static modes_t MODE_BEFORE_SNAPSHOT;  // mode to return to from snapshot mode

enum lmodes_t {PLAY, RECORD, OVERDUB, STOP};  // looper modes
static lmodes_t LPR_MODE;

bool with_looper = true;
uint8_t midi_channel = 0;

// Jump to address 0 to restart the sketch in the newly selected mode
void (*Reset)(void) = 0;

void setup() {

  // LEDs
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GRN, OUTPUT);

  // Buttons
  btnUp.setClickMs(CLICK_MS);
  btnUp.attachClick(upClick);
  btnUp.attachLongPressStart(upLongPressStart);

  btnDn.setClickMs(CLICK_MS);
  btnDn.attachClick(dnClick);
  btnDn.attachLongPressStart(dnLongPressStart);

  btnTap.setClickMs(CLICK_MS);
  btnTap.attachClick(tapClick);
  btnTap.attachLongPressStart(tapLongPressStart);

  btnFour.setClickMs(CLICK_MS);
  btnFour.attachClick(fourClick);
  btnFour.attachLongPressStart(fourLongPressStart);

  // Set MIDI baud rate
  Serial.begin(31250);

  // Restore looper config
  with_looper = (EEPROM.read(LOOPER_MODE_ADDR) == 1);

  // Restore mode
  MODE = (modes_t) EEPROM.read(OP_MODE_ADDR);
  if (MODE > LOOPER || (MODE == LOOPER && !with_looper)) {
    // no valid value in EEPROM (first power up?) or looper disabled
    MODE = SCROLL;
  }

  // Restore MIDI channel
  midi_channel = EEPROM.read(MIDI_CHANNEL_ADDR);
  if (midi_channel > 15) {
    midi_channel = 0;
    EEPROM.update(MIDI_CHANNEL_ADDR, midi_channel);
  }

  // Configuration modes, selected by holding a button while powering up.
  // Remember the normal mode so we can go back to it afterwards.
  LAST_MODE = MODE;

  if (digitalRead(BTN_DN) == LOW && digitalRead(BTN_UP) == HIGH) {
    // dn pressed: configure MIDI channel
    MODE = CHANNEL_CFG;
    flashLED(20, LED_GRN, LED_FLASH_DELAY);
    delay(1000);
    // show the current channel: 1 flash = channel 1 ... 16 flashes = channel 16
    flashLED(midi_channel + 1, LED_GRN, 500);
    return;
  }

  if (digitalRead(BTN_DN) == HIGH && digitalRead(BTN_UP) == LOW) {
    // up pressed: enable/disable looper control
    MODE = LOOPER_CFG;
    flashLED(20, LED_RED, LED_FLASH_DELAY);
    delay(1000);
    flashLED(1, with_looper ? LED_GRN : LED_RED, 500);
    return;
  }

  // Put the HX Stomp in the matching footswitch mode
  if (MODE == SNAPSHOT)
    midiCtrlChange(CC_FS_MODE, 3);  // snapshot mode
  else
    midiCtrlChange(CC_FS_MODE, 0);  // stomp mode

  MODE_BEFORE_SNAPSHOT = MODE;

  // Indicate mode via LEDs
  if (MODE == LOOPER) {
    flashRedGreen(10);
    // Looper mode: DN reacts on press instead of release
    jc_btnDn.begin();
  }
  else if (MODE == SCROLL)
    flashLED(10, LED_RED, LED_FLASH_DELAY);
  else if (MODE == FS)
    flashLED(10, LED_GRN, LED_FLASH_DELAY);

  // Looper default state
  LPR_MODE = STOP;
}


void loop() {

  if (MODE == LOOPER) {
    jc_btnDn.read();                // DN handled by JC_Button (acts on press)
    if (jc_btnDn.wasPressed() && digitalRead(BTN_UP) == HIGH)
      looperRecordOverdub();
  } else {
    btnDn.tick();                   // DN handled by OneButton (acts on release)
  }
  btnUp.tick();
  btnTap.tick();
  btnFour.tick();

  handle_leds();
}

/* ------------------------------------------------- */
/* ---       OneButton Callback Routines          ---*/
/* ------------------------------------------------- */

// TAP button: tap tempo, long press toggles the tuner
void tapClick() {
  if (isConfigMode())
    return;
  if (MODE == TUNER) {
    toggleTuner();          // any button gets us out of the tuner
    return;
  }
  midiCtrlChange(CC_TAP_TEMPO, 127);
}

void tapLongPressStart() {
  if (isConfigMode())
    return;
  toggleTuner();
}

// FOUR button: toggles CC 6 on/off, long press toggles all bypass
void fourClick() {
  static bool toggle_state = false;

  if (isConfigMode())
    return;
  toggle_state = !toggle_state;
  midiCtrlChange(CC_FOUR_TOGGLE, toggle_state ? 127 : 0);
  flashLED(1, toggle_state ? LED_GRN : LED_RED, LED_FLASH_DELAY);
}

void fourLongPressStart() {
  static bool bypassed = false;

  if (isConfigMode())
    return;
  bypassed = !bypassed;
  midiCtrlChange(CC_ALL_BYPASS, bypassed ? 0 : 127);
  flashLED(3, LED_RED, LED_FLASH_DELAY);
}

// DN button
void dnClick() {
  switch (MODE)
  {
    case SCROLL:
      patchDown();
      flashLED(2, LED_RED, LED_FLASH_DELAY);
      break;
    case TUNER:
      toggleTuner();
      break;
    case SNAPSHOT:
      midiCtrlChange(CC_SNAPSHOT, 9);  // previous snapshot
      flashLED(2, LED_RED, LED_FLASH_DELAY);
      break;
    case FS:
      midiCtrlChange(CC_FS4, 0);       // emulate FS4
      flashLED(2, LED_RED, LED_FLASH_DELAY);
      break;
    case LOOPER:
      // handled by JC_Button in loop()
      break;
    case CHANNEL_CFG:
      if (midi_channel > 0)
        midi_channel--;
      flashLED(midi_channel + 1, LED_GRN, 500);
      EEPROM.update(MIDI_CHANNEL_ADDR, midi_channel);
      break;
    case LOOPER_CFG:
      with_looper = false;
      flashLED(1, LED_RED, 500);
      EEPROM.update(LOOPER_MODE_ADDR, with_looper);
      break;
  }
}

// UP button
void upClick() {
  switch (MODE)
  {
    case SCROLL:
      patchUp();
      flashLED(2, LED_RED, LED_FLASH_DELAY);
      break;
    case TUNER:
      toggleTuner();
      break;
    case SNAPSHOT:
      midiCtrlChange(CC_SNAPSHOT, 8);  // next snapshot
      flashLED(2, LED_RED, LED_FLASH_DELAY);
      break;
    case FS:
      midiCtrlChange(CC_FS5, 0);       // emulate FS5
      flashLED(2, LED_RED, LED_FLASH_DELAY);
      break;
    case LOOPER:
      switch (LPR_MODE) {
        case STOP:
          LPR_MODE = PLAY;
          midiCtrlChange(CC_LOOPER_PLAY, 127);  // play
          break;
        case PLAY:
        case RECORD:
        case OVERDUB:
          LPR_MODE = STOP;
          midiCtrlChange(CC_LOOPER_PLAY, 0);    // stop
          break;
      }
      break;
    case CHANNEL_CFG:
      if (midi_channel < 15)
        midi_channel++;
      flashLED(midi_channel + 1, LED_GRN, 500);
      EEPROM.update(MIDI_CHANNEL_ADDR, midi_channel);
      break;
    case LOOPER_CFG:
      with_looper = true;
      flashLED(1, LED_GRN, 500);
      EEPROM.update(LOOPER_MODE_ADDR, with_looper);
      break;
  }
}

void dnLongPressStart() {
  if (digitalRead(BTN_UP) == LOW)
    return;  // both buttons held, handled in upLongPressStart()

  switch (MODE)
  {
    case SCROLL:
    case SNAPSHOT:
    case FS:
      toggleTuner();
      break;
    default:
      break;
  }
}

void upLongPressStart() {

  if (digitalRead(BTN_DN) == LOW) {
    // Both buttons held: switch to the next mode, or leave configuration
    switch (MODE) {
      case SCROLL:
        MODE = FS;
        break;
      case FS:
      case SNAPSHOT:
        MODE = with_looper ? LOOPER : SCROLL;
        break;
      case LOOPER:
        midiCtrlChange(CC_LOOPER_PLAY, 0);  // make sure the looper is stopped
        MODE = SCROLL;
        break;
      case TUNER:
        return;
      case CHANNEL_CFG:
      case LOOPER_CFG:
        // back to the mode we had before entering configuration
        MODE = LAST_MODE;
        if (MODE == LOOPER && !with_looper)
          MODE = SCROLL;
        break;
    }
    EEPROM.update(OP_MODE_ADDR, MODE);
    // restart the device in the new mode
    Reset();
  }

  // Regular long press
  switch (MODE)
  {
    case SCROLL:
    case FS:
      // remember where we entered snapshot mode from
      MODE_BEFORE_SNAPSHOT = MODE;
      midiCtrlChange(CC_FS_MODE, 3);  // snapshot mode
      flashLED(5, LED_RED, LED_FLASH_DELAY);
      MODE = SNAPSHOT;
      EEPROM.update(OP_MODE_ADDR, MODE);
      break;
    case SNAPSHOT:
      midiCtrlChange(CC_FS_MODE, 0);  // stomp mode
      flashLED(5, LED_RED, LED_FLASH_DELAY);
      MODE = (MODE_BEFORE_SNAPSHOT == FS) ? FS : SCROLL;
      EEPROM.update(OP_MODE_ADDR, MODE);
      break;
    case LOOPER:
      if (LPR_MODE == PLAY || LPR_MODE == STOP) {
        midiCtrlChange(CC_LOOPER_UNDO, 127);  // undo/redo
        flashLED(3, LED_RED, LED_FLASH_DELAY);
      }
      break;
    default:
      break;
  }
}

/* ------------------------------------------------- */
/* ---       Actions                              ---*/
/* ------------------------------------------------- */

// Looper: DN cycles record -> play -> overdub -> play ...
void looperRecordOverdub() {
  switch (LPR_MODE) {
    case STOP:
      LPR_MODE = RECORD;
      midiCtrlChange(CC_LOOPER_REC, 127);   // record
      break;
    case RECORD:
      LPR_MODE = PLAY;
      midiCtrlChange(CC_LOOPER_PLAY, 127);  // play
      break;
    case PLAY:
      LPR_MODE = OVERDUB;
      midiCtrlChange(CC_LOOPER_REC, 0);     // overdub
      break;
    case OVERDUB:
      LPR_MODE = PLAY;
      midiCtrlChange(CC_LOOPER_PLAY, 127);  // play
      break;
  }
}

// Turn the tuner on or off and keep MODE in sync, whichever button was used
void toggleTuner() {
  midiCtrlChange(CC_TUNER, 0);
  flashLED(2, LED_RED, LED_FLASH_DELAY);
  if (MODE == TUNER) {
    MODE = LAST_MODE;
  } else {
    LAST_MODE = MODE;
    MODE = TUNER;
  }
}

bool isConfigMode() {
  return MODE == CHANNEL_CFG || MODE == LOOPER_CFG;
}

void patchUp() {
  midiCtrlChange(CC_PRESET, 127);  // next preset
}

void patchDown() {
  midiCtrlChange(CC_PRESET, 0);    // previous preset
}

/* ------------------------------------------------- */
/* ---       MIDI                                 ---*/
/* ------------------------------------------------- */

void midiProgChange(uint8_t p) {
  Serial.write(0xc0 | midi_channel);  // PC message
  Serial.write(p);                    // program
}

void midiCtrlChange(uint8_t c, uint8_t v) {
  Serial.write(0xb0 | midi_channel);  // CC message
  Serial.write(c);                    // controller
  Serial.write(v);                    // value
}

/* ------------------------------------------------- */
/* ---       LEDs                                 ---*/
/* ------------------------------------------------- */

void flashLED(uint8_t i, uint8_t led, uint16_t del) {
  uint8_t last_state = digitalRead(led);

  for (uint8_t j = 0; j < i; j++) {
    digitalWrite(led, HIGH);
    delay(del);
    digitalWrite(led, LOW);
    delay(del);
  }
  digitalWrite(led, last_state);
}

void flashRedGreen(uint8_t i) {
  uint8_t last_state_r = digitalRead(LED_RED);
  uint8_t last_state_g = digitalRead(LED_GRN);

  for (uint8_t j = 0; j < i; j++) {
    digitalWrite(LED_RED, LOW);
    digitalWrite(LED_GRN, HIGH);
    delay(75);
    analogWrite(LED_RED, LED_RED_BRIGHTNESS);
    digitalWrite(LED_GRN, LOW);
    delay(75);
  }
  digitalWrite(LED_RED, last_state_r);
  digitalWrite(LED_GRN, last_state_g);
}

void handle_leds() {
  static unsigned long last_blink = 0;

  switch (MODE) {
    case SCROLL:
      // solid red
      digitalWrite(LED_GRN, LOW);
      analogWrite(LED_RED, LED_RED_BRIGHTNESS);
      break;
    case SNAPSHOT:
    case FS:
      // solid green
      digitalWrite(LED_RED, LOW);
      digitalWrite(LED_GRN, HIGH);
      break;
    case TUNER:
      // blink green
      digitalWrite(LED_RED, LOW);
      if (millis() - last_blink >= 500) {
        last_blink = millis();
        digitalWrite(LED_GRN, !digitalRead(LED_GRN));
      }
      break;
    case LOOPER:
      switch (LPR_MODE) {
        case STOP:
          digitalWrite(LED_GRN, LOW);
          digitalWrite(LED_RED, LOW);
          break;
        case PLAY:
          // green
          digitalWrite(LED_GRN, HIGH);
          digitalWrite(LED_RED, LOW);
          break;
        case RECORD:
          // red
          digitalWrite(LED_GRN, LOW);
          analogWrite(LED_RED, LED_RED_BRIGHTNESS);
          break;
        case OVERDUB:
          // yellow
          digitalWrite(LED_GRN, HIGH);
          analogWrite(LED_RED, LED_RED_BRIGHTNESS);
          break;
      }
      break;
    default:
      break;
  }
}
