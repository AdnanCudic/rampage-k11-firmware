#!/bin/bash
# Flash k11min_pad.bin to the K11 (LZ901/CH555). First enter the factory
# bootloader: unplug, short the topmost test hole left of the MCU (P1.5) to
# GND, plug in, release. The bootloader answers one session per entry, and
# can't be entered from the firmware (see README.md, quirk 6).
#
# CH55X_FULL_UID: key checksum over all 8 UID bytes (ch55xtool uses 4).
# Images must be padded to 64-byte blocks (build.sh does this) or the last
# block is silently dropped.
set -e
cd "$(dirname "$0")"
CH55X_FULL_UID=1 CH55X_ZERO_KEY=1 CH55X_WCHUNK=32 CH55X_VCHUNK=32 \
    python3 ch55x_flash.py -f k11min_pad.bin --verify_flash k11min_pad.bin -r
