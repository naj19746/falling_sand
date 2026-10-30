# sandbg keymap for the Keychron Q4

sandbg drives the Q4's 61 per-key LEDs through the keyboard's raw HID
interface. Stock QMK/VIA firmware only offers whole-keyboard effects, so
this keymap adds a small custom RGB matrix effect and a handler for
sandbg's color reports.

The keymap is Keychron's stock Q4 keymap (`keymaps/keychron`: Mac and
Windows layers plus the function layers) with the LED protocol added, and
with the remaps that were saved on this keyboard through VIA: IJKL arrows
and Print Screen on Fn+. in the Windows Fn layer, screenshot on Fn+L in
the Mac Fn layer, and NKRO toggle on Fn+N in both. Flashing it replaces whatever
keymap is on the keyboard; if you have a custom one, copy the `sandbg_leds`,
`raw_hid_receive` and `housekeeping_task_user` code into it instead.

## Build and flash

1. Set up the Keychron QMK tree:

   ```
   python3 -m pip install --user qmk    # or: pipx install qmk
   qmk setup Keychron/qmk_firmware
   ```

2. Copy this directory into the shared ANSI keymap slot:

   ```
   cp -r firmware/keychron-q4-sandbg \
       ~/qmk_firmware/keyboards/keychron/q4/ansi/keymaps/sandbg
   ```

3. Compile and flash, using `v1` or `v2` depending on the PCB revision
   printed on yours (both use the same LED layout and keymap):

   ```
   qmk compile -kb keychron/q4/ansi/v1 -km sandbg
   qmk flash  -kb keychron/q4/ansi/v1 -km sandbg
   ```

   If `qmk flash` can't find the keyboard, put it in DFU mode first: hold
   Esc while plugging in the cable (or press the reset button on the PCB).

4. If the colors are scrambled, the PCB is probably the other revision:
   repeat with `ansi/v2` instead of `ansi/v1`.

VIA is disabled in this build so it doesn't compete for the raw HID
interface. That also means VIA and Keychron Launcher can't configure the
keyboard, and any keymap changes made with them are replaced by the
keymap here.

Keychron's common build normally supplies `keychron_raw_hid.c`, which
defines its own `raw_hid_receive` when VIA is off. `rules.mk` drops that
one file, adds `sandbg_raw_hid.c` for the helper the rest of the common
code calls, and leaves the rest of Keychron's common code alone. If you
merge this into another keymap, copy those two lines and the file as well.

## Protocol

The host writes 32-byte reports to the raw HID interface (usage page
`0xFF60`, usage `0x61`). There is no report ID. Every message has the form:

| Byte | Meaning |
|---|---|
| 0-1 | `"SB"` |
| 2 | protocol version, currently 1 |
| 3 | command: `P` ping, `C` colors, `R` release |
| 4 | first LED index |
| 5 | LED count (0-8) |
| 6..29 | `count` RGB triples |
| 30 | unused |
| 31 | checksum: sum of bytes 0..30, modulo 256 |

A ping gets the same header back with command `P`, byte 4 = protocol
version, byte 5 = `RGB_MATRIX_LED_COUNT`, byte 6 = `SB_CHUNK` (8).

The first color report takes over the LEDs and switches to the custom
`sandbg` effect. Reports are not saved to EEPROM. If none arrives for three
seconds, or a release report arrives, the previous effect, brightness and
enabled state are restored. The host sends a release when it exits, so a
normal `q` or Esc gives the keyboard straight back.

## Troubleshooting

- Check what the host sees: `sandbg --keyboard` (from the sandbg repo).
  It should list the keyboard's raw HID interface and print
  `sandbg keymap: yes (protocol 1, 61 LEDs)`.
- If it says permission denied, install `70-sandbg-keyboard.rules` from the
  sandbg repo (`sandbg-setup` does it) and replug the keyboard.
- If it says no response, the keymap isn't flashed.
- If only some LEDs light or the colors are in the wrong places, check that
  the layout table in `keyboard.c` matches the keyboard, and that you built
  for the right PCB revision (`ansi/v1` or `ansi/v2`). It currently covers
  the Q4 ANSI only.
