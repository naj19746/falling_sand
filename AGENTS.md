# AGENTS.md

Notes for coding agents working on sandbg. See [README.md](README.md) for
what it is and how to use it.

## Build and run

```
make                     # builds ./sandbg; must stay free of -Wall -Wextra warnings
make run                 # ./sandbg --windowed: a normal window, safe for testing
make clean
```

Don't run `make install`, `sandbg-setup`, or bare `./sandbg` unless asked.
They change the user's real desktop: install to `~/.local`, turn off desktop
icons, write autostart entries, and set the global `core.hooksPath`. Use
`--windowed` to try changes.

A copy of sandbg is probably already running as the user's wallpaper. A
lock file stops a second desktop copy, but `--windowed` runs alongside it.
`sandbg --strike` and git commits signal the running copy (SIGUSR1).

## Layout

```
main.c      startup, flags (usage()), config load, main loop, input, pause when covered
sim.c/.h    the cellular automaton: all cell types, emitters, blaster, drain cycle,
            seeds, lightning, glass, trees, sleeping chunks
sim_internal.h  helpers shared by sim.c and critter.c (set_cell, rnd, move, ...)
critter.c   critters: bodies on the grid, food, wheat, homes and wells (see CRITTERS.md)
render.c/.h draws changed chunks into the X window through MIT-SHM
x11.c/.h    desktop window properties, the "is the desktop covered" check
audio.c/.h  PulseAudio loudness of the output (background thread)
weather.c/.h  Open-Meteo via curl → drops per minute (background thread)
spotify.c/.h  Spotify over D-Bus MPRIS → count of songs finished (background thread)
progress.c/.h  new Pixelorama drawings → critters waiting to drop in (background thread)
config.c/.h ini parsing, defaults, validation
keyboard.c/.h  Keychron Q4 per-key LEDs: screen -> keys, raw HID protocol
firmware/keychron-q4-sandbg/  the QMK keymap the keyboard needs for per-key control
sandbg-setup  install/undo script; git-hook  the global hook dispatcher it installs
```

`sim.c` has no SDL or X11 dependencies, so it can be driven headlessly.

## Invariants in sim.c

- Cells are `uint16_t`: low byte type, high byte color. Some types reuse
  high color bits as flags (`WITHERED`, `RAIN_FRESH`, `SEEPING` in sim.h).
- Type order in the enum matters: everything from `SAND` on falls
  (`FALLS()`). Add new falling types after `SAND` and new static types
  before it, and update the classification macros.
- Any write that changes a cell's type must go through `set_cell()`, which
  keeps the running totals (`grains`, `solids`, `water`, ...) right and
  wakes the chunk. The drain cycle and the fill check rely on those totals;
  the grid is never recounted. Swaps of two cells, which don't change
  the totals, use `move()` or mark both cells' chunks directly.
- Chunks (16×16) sleep after a couple of steps without changes. Anything
  that changes a cell must wake its chunk, or the change won't be simulated
  or drawn.
- The scan is bottom-up and alternates direction each step. Per-cell stamps
  stop a cell moving twice in one step.

## Adding a config setting

Change all of these together:

1. the field in `Config` (config.h) and its default in `config_defaults()`
2. parsing in `config_set()` and range checks in `config_validate()`
3. a commented entry in `config.ini.example`
4. the table in README.md, if the setting is one users will commonly change

## Testing

There's no test suite in the repo. To check simulation changes, write a
throwaway headless program outside the repo that links `sim.c`, runs many
steps, and checks that:

- running totals match a full recount of the grid
- the grid settles: no falling grain is left with room to fall, and every
  chunk eventually sleeps
- several fill/drain cycles in a row complete

Then look at it with `make run`. For the external inputs, use
`--set rain_override=<drops/min>` for rain, the `s` and `l` keys for seeds
and lightning, and `--meter`, `--weather` and `--spotify` to check what
sandbg sees.

For the keyboard, `--keyboard-preview` draws the computed LED colors in the
terminal (it needs no hardware), and `--keyboard` checks whether the
Keychron's raw HID interface is visible and running the sandbg keymap. The
LED order and key geometry are in `keyboard.c`; the firmware protocol is
documented in `firmware/keychron-q4-sandbg/README.md`.

## Style

- C11, 4-space indent, braces on the same line except for function bodies.
- Short comments that explain why, in plain sentences, above the code
  they describe. Match the surrounding density.
- Keep the idle CPU cost low: don't scan or redraw chunks that haven't
  changed, and keep blocking work (network, D-Bus, audio) on the
  background threads.
- Don't commit a real `config.ini` or personal coordinates.
  `config.ini.example` uses Santa Fe, NM as its sample location.
