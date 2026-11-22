/**************************************************************************

  MIDI Muppet Max - MIDI foot switch for the BOSS RC-500 Loop Station
  ===================================================================

  Four button MIDI foot switch that recalls presets with Program Change
  messages. A short press recalls one preset, a long press another.

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
  Parts of this code (MIDI and LED helpers) come from MIDI Muppet HX and
  are used under the following licence:

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
  - Buttons ONE..FOUR   D2, D3, D6, D8 (to ground, internal pull-ups)
  - LED green / red     D4, D5   (bicolor LED, common cathode)
  - MIDI out            TX (D1)
  - Board: Arduino Nano or Pro Mini (ATmega168 or ATmega328P)

  Library
  -------
  - OneButton  https://github.com/mathertel/OneButton
    OneButton detects a long press without detecting a short press first,
    so the short press action is sent when the button is released.

  Pedal notes
  -----------
  On the RC-500, Program Change 0-98 selects memory 1-99. Make sure the
  RC-500 receives on the same MIDI channel as MIDI_CHANNEL below
  (set in the RC-500's MIDI settings).

  LED feedback
  ------------
  Short press flashes green, long press flashes red. The number of flashes
  is the preset's position in its bank of four (PC 0 and 4 = 1 flash,
  PC 1 and 5 = 2 flashes, and so on).

**************************************************************************/

#include <OneButton.h>

// GPIO pins used
#define BTN_ONE   2
#define BTN_TWO   3
#define BTN_THREE 6
#define BTN_FOUR  8
#define LED_GRN   4
#define LED_RED   5

// on/off delay when we flash a LED
#define LED_FLASH_DELAY 100
// time the buttons wait before reporting a click (ms)
#define CLICK_MS        50

/* ------------------------------------------------- */
/* ---       User configuration                   ---*/
/* ------------------------------------------------- */

// MIDI channel the pedal sends on, 0-15 (0 = MIDI channel 1)
#define MIDI_CHANNEL 0

// Program Change number sent for each button: {short press, long press}
// PC 0 = preset/memory 1 on the pedal.
const uint8_t PROGRAMS[4][2] = {
  {1, 5},  // button ONE   -> preset 2 / preset 6
  {0, 4},  // button TWO   -> preset 1 / preset 5
  {3, 7},  // button THREE -> preset 4 / preset 8
  {2, 6},  // button FOUR  -> preset 3 / preset 7
};

/* ------------------------------------------------- */
/* ---       No changes needed below this point   ---*/
/* ------------------------------------------------- */

OneButton buttons[4] = {
  OneButton(BTN_ONE, true),
  OneButton(BTN_TWO, true),
  OneButton(BTN_THREE, true),
  OneButton(BTN_FOUR, true),
};

void setup() {

  // LEDs
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GRN, OUTPUT);

  // Buttons: the button index is passed to the callbacks
  for (uint8_t i = 0; i < 4; i++) {
    buttons[i].setClickMs(CLICK_MS);
    buttons[i].attachClick(onClick, (void *)(uintptr_t)i);
    buttons[i].attachLongPressStart(onLongPressStart, (void *)(uintptr_t)i);
  }

  // Set MIDI baud rate
  Serial.begin(31250);

  // Show that we are alive
  flashLED(3, LED_GRN, LED_FLASH_DELAY);
}

void loop() {
  for (uint8_t i = 0; i < 4; i++)
    buttons[i].tick();
}

/* ------------------------------------------------- */
/* ---       OneButton callback routines          ---*/
/* ------------------------------------------------- */

void onClick(void *param) {
  uint8_t program = PROGRAMS[(uintptr_t)param][0];
  midiProgChange(program);
  flashLED(program % 4 + 1, LED_GRN, LED_FLASH_DELAY);
}

void onLongPressStart(void *param) {
  uint8_t program = PROGRAMS[(uintptr_t)param][1];
  midiProgChange(program);
  flashLED(program % 4 + 1, LED_RED, LED_FLASH_DELAY);
}

/* ------------------------------------------------- */
/* ---       MIDI                                 ---*/
/* ------------------------------------------------- */

// Send a Program Change message
void midiProgChange(uint8_t p) {
  Serial.write(0xc0 | MIDI_CHANNEL);  // PC message
  Serial.write(p & 0x7f);             // program
}

// Send a Control Change message (not used yet, handy for CC actions)
void midiCtrlChange(uint8_t c, uint8_t v) {
  Serial.write(0xb0 | MIDI_CHANNEL);  // CC message
  Serial.write(c & 0x7f);             // controller
  Serial.write(v & 0x7f);             // value
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
