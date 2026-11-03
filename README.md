# Rampage Radiant K11 firmware

Replacement firmware for the **Rampage Radiant K11**, a 60% RGB keyboard. My
board lost its firmware (it only showed up as a WCH bootloader, `4348:55e0`)
and Rampage publishes none, so this is written from scratch: about 2.3 KB of
C for SDCC, with keys, an Fn layer and the RGB backlight.

The MCU is labelled **"LZ901"** but is a rebranded **WCH CH555** (8051 core,
LQFP48). The flashing notes below should help with other LZ901 / CH555
keyboards too.

> Flashing can't brick the board: the chip's factory bootloader can't be
> overwritten, and the strap described below always gets back into it.
> Still, this comes with no warranty.

## Using the keyboard

| Keys | Does |
|---|---|
| Fn + 1 … = | F1 … F12 |
| Fn + Backspace | Delete |
| Fn + Esc | `` ` `` / `~` |
| Fn + I / J / K / L | ↑ ← ↓ → |
| Fn + P | Print Screen |
| RGB + 1 … 9 | backlight brightness |
| RGB + 0 | backlight off |
| RGB + R / G / B | toggle red / green / blue |

"RGB" is the Rampage key between Fn and right Ctrl. While it is held, nothing
is typed. The backlight starts as dim red on every plug-in; settings are not
saved.

## Files

| File | What it is |
|---|---|
| `main.c` | the keyboard firmware: USB, matrix scan, keymap, backlight |
| `pinfinder.c` | matrix-mapping firmware: each key press types the two pins it connects |
| `ch555.h` | WCH's CH555 register header (via [qmk-8051](https://github.com/vuhuycan/qmk-8051)) |
| `build.sh` | builds `main.c` (or the file given) into `k11min_pad.bin` |
| `flash.sh` | flashes `k11min_pad.bin` and verifies it |
| `ch55x_flash.py` | runs `ch55xtool` with this chip's fixes (see below) |

## Setup (Linux)

- **SDCC 4.4.0.** `build.sh` uses `$SDCC_DIR` (default
  `~/.local/opt/sdcc-4.4.0`) if it exists, otherwise `sdcc` from `PATH`. The
  [prebuilt 4.4.0](https://sourceforge.net/projects/sdcc/files/sdcc-linux-amd64/4.4.0/)
  can be unpacked straight into `~/.local/opt`. SDCC 4.5.0 crashed on
  qmk-8051's code; this firmware hasn't been tried with it.
- **ch55xtool:** `pip install --user ch55xtool` (pulls in pyusb). No patching
  needed; `ch55x_flash.py` applies the fixes.
- **USB access without sudo**, `/etc/udev/rules.d/99-wch.rules`:

  ```
  SUBSYSTEM=="usb", ATTR{idVendor}=="4348", ATTR{idProduct}=="55e0", MODE="0666"
  ```

## Building and flashing

```bash
./build.sh                 # -> k11min_pad.bin
# put the keyboard in its bootloader (below); lsusb shows 4348:55e0
./flash.sh                 # erase, flash, verify, restart into the firmware
```

**Entering the bootloader** needs the case open: unplug, short the
**topmost test hole left of the MCU** to the **G** hole under the USB-C port
(or the USB-C shell), plug in, then release. That hole is P1.5 (LQFP48
pin 48), which the bootloader checks at power-up. There is no keyboard
shortcut for this: see quirk 6 below.

`flash.sh` ends with a `USBError` (`No such device` or `Input/Output Error`):
that is the chip restarting before the tool finishes, and is expected. Check
for `Verifying flash. Done.` above it.

### Changing the keymap

The keymap is the `keymap` table in `main.c`: layer 0 is the base layer,
layer 1 is Fn, and `____` in the Fn layer falls through to layer 0. Keycodes
are the `KC_*` defines just above it (USB HID usage IDs).

## Hardware

- **MCU:** "-Line LZ901 410119B50", LQFP48 = WCH CH555: 64 KB flash, 8 KB
  xRAM, USB full-speed. Chip ID 0x55, sub-ID 0x11, bootloader version 2.30.
- **Config bits** (read with `ch55xtool -p`, left unchanged): P7.1 reset
  disabled, boot from bootloader, code read-protection on.

### Key matrix

Found with `pinfinder.c`. Columns are driven low one at a time, rows read back
low through a diode on every key.

| Row \ Col | P0.0 | P0.1 | P0.2 | P0.3 | P0.4 | P0.5 | P0.6 | P0.7 | P3.0 | P3.1 | P3.2 | P3.3 | P3.4 | P3.5 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **P4.1** | Esc | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 0 | - | = | Bksp |
| **P4.2** | Tab | Q | W | E | R | T | Y | U | I | O | P | [ | ] | |
| **P4.3** | Caps | A | S | D | F | G | H | J | K | L | ; | ' | Enter | \\ |
| **P4.4** | LShift | Z | X | C | V | B | N | M | , | . | / | RShift | | |
| **P4.5** | LCtrl | Win | LAlt | Space | RAlt | Fn | RGB | RCtrl | | | | | | |

If your board differs, build and flash `pinfinder.c` (`./build.sh
pinfinder.c`), open a text editor and press every key once: each press types
`DdRr` pairs (driven port/bit, read port/bit), e.g. `4002`.

### RGB backlight

The LEDs share the matrix (WCH's reference design for the CH555):

- A column pin low turns on that column's transistor (Q1–Q15), which powers
  the column's LEDs.
- Each row has three cathode lines: **red P4.1–P4.5, green P2.1–P2.5,
  blue P1.1–P1.5**. Pulling one low lights that colour in that row.
- P4 has a current-limited LED mode (`P4_LED_KEY` + push-pull), used for red.
  P2/P1 have no on-chip limit; the board's own parts limit them.

Two things learned the hard way, both handled in `matrix_scan()`:

- **LED switching must be above ~20 kHz.** At a few kHz the 100 µF
  electrolytic capacitor on the 5 V rail buzzed audibly. Column slots are
  timed with Timer0 (0.5 µs ticks, 72 ticks per column ≈ 24 kHz).
- **Key reads must be very short.** A pressed key's LED current flows through
  the key diode into the column pin, which has no current limit, so holding a
  column low for microseconds makes pressed keys flash red. Reads hold the
  column low for under 1 µs, every 4th LED frame.

## Flashing quirks of this chip

`ch55x_flash.py` works around all of these; they are why stock tools fail.

1. **Bulk IN endpoint is 0x83**, not the 0x82 ch55xtool expects. `wchisp`
   fails with "USB Endpoints not found" for the same reason.
2. **Encryption key uses all 8 UID bytes.** ch55xtool sums only the first 4
   for sub-ID 0x11 chips; data then lands scrambled and verify fails at
   address 0. (`CH55X_FULL_UID=1`)
3. **Erasing takes longer than 5 s**, ch55xtool's USB timeout. The wrapper
   allows 30 s.
4. **Only complete 64-byte flash blocks are written.** A partial last block is
   silently dropped, so every image must be padded to a multiple of 64 bytes.
   `build.sh` does this. Unpadded images verify fine up to the last full block
   and then crash at boot.
5. **One tool session per power-up.** After any session, even a successful
   one, the bootloader stops answering; re-enter it before the next command.
6. **The bootloader only works after a real power-up.** Firmware can jump to
   it (`ljmp 0xF400` after resetting USB, clock and timer), and it enumerates
   as 4348:55e0, but it answers every command with status `0xD5` (detect
   reply `a1 d5 02 00 f1 00`, which ch55xtool reports as "Chip ID: f1").
   It requires the `bBOOT_LOAD` flag, which only power-on / brown-out reset
   sets; software and watchdog resets restart at address 0 with the flag
   clear. So a "reflash" key combo is impossible with this bootloader. The
   options would be a wired P1.5 button reachable from outside the case, or a
   self-written USB updater in the firmware.

## Why not QMK

[qmk-8051](https://github.com/vuhuycan/qmk-8051), a QMK port for CH555
keyboards, builds and enumerates on this board, but USB setup failed at random
with `-71` errors. Disabling sleep, leaving the LED pins alone, running at
12 MHz and removing the non-reentrant `memcpy` from the USB interrupt didn't
fix it. This firmware uses the same WCH USB register sequence without QMK and
has been stable.

## License

MIT, see [LICENSE](LICENSE). `ch555.h` is WCH's register header and keeps its
own copyright notice.
