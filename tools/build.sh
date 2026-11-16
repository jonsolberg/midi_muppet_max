#!/bin/bash
# Part of MIDI Muppet 4btn – Copyright (C) 2026 Jon Solberg – GPL v3 or later, see LICENSE.
# Compile an Arduino sketch with plain avr-gcc against ArduinoCore-avr.
# usage: [FCPU=8000000L] build.sh <sketch.ino> <mcu> [output name]   mcu: atmega168 / atmega328p
set -e
INO=$1; MCU=$2
DEPS=${DEPS:-$(dirname "$0")/../deps}   # ArduinoCore-avr, OneButton, JC_Button checked out here
CORE=$DEPS/ArduinoCore-avr/cores/arduino
VAR=$DEPS/ArduinoCore-avr/variants/eightanaloginputs
EEP=$DEPS/ArduinoCore-avr/libraries/EEPROM/src
OUT=$(mktemp -d)
FLAGS="-mmcu=$MCU -DF_CPU=${FCPU:-16000000L} -DARDUINO=10819 -DARDUINO_AVR_NANO -DARDUINO_ARCH_AVR -Os -ffunction-sections -fdata-sections -Wall -Wextra -Wno-unused-parameter"
INC="-I$CORE -I$VAR -I$EEP -I$DEPS/OneButton/src -I$DEPS/JC_Button/src"
# sketch: prepend Arduino.h and prototypes like the IDE does (ctags-ish via grep)
python3 -I "$(dirname "$0")/preproc.py" "$INO" > $OUT/sketch.cpp
OBJS=""
for f in $CORE/*.c; do avr-gcc $FLAGS $INC -c $f -o $OUT/$(basename $f).o 2>/dev/null; OBJS="$OBJS $OUT/$(basename $f).o"; done
for f in $CORE/*.cpp $DEPS/OneButton/src/*.cpp $DEPS/JC_Button/src/*.cpp; do avr-g++ -std=gnu++17 -fno-exceptions -fno-threadsafe-statics $FLAGS $INC -c $f -o $OUT/$(basename $f).o 2>/dev/null; OBJS="$OBJS $OUT/$(basename $f).o"; done
avr-g++ -std=gnu++17 -fno-exceptions -fno-threadsafe-statics $FLAGS $INC -c $OUT/sketch.cpp -o $OUT/sketch.o
avr-gcc -mmcu=$MCU -Os -Wl,--gc-sections $OUT/sketch.o $OBJS -lm -o $OUT/sketch.elf
avr-size -C --mcu=$MCU $OUT/sketch.elf | grep -E "Program|Data"
[ -n "$3" ] && cp $OUT/sketch.elf "$3.elf" && avr-objcopy -O ihex -R .eeprom $OUT/sketch.elf "$3.hex"
