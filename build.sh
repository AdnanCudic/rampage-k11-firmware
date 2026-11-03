#!/bin/bash
# Build the K11 firmware (main.c, or the source given); output k11min_pad.bin (padded to 64-byte blocks,
# which this chip's bootloader requires).
set -e
cd "$(dirname "$0")"
# SDCC 4.4.0 unpacked here is used if present; otherwise sdcc from PATH
SDCC_DIR=${SDCC_DIR:-$HOME/.local/opt/sdcc-4.4.0}
[ -d "$SDCC_DIR/bin" ] && export PATH="$SDCC_DIR/bin:$PATH"
SRC=${1:-main.c} # pass pinfinder.c to build the matrix mapper instead
sdcc -mmcs51 --model-small --iram-size 256 --xram-loc 0x0100 --xram-size 0x0C00 --code-size 0xF000 -o main.ihx "$SRC"
makebin -p main.ihx k11min.bin
python3 -c "
d = open('k11min.bin', 'rb').read()
n = (len(d) + 63) // 64 * 64
open('k11min_pad.bin', 'wb').write(d + b'\x00' * (n - len(d)))
print('k11min_pad.bin:', len(d), '->', n, 'bytes')"
