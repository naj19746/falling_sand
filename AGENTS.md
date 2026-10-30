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
progress.c/.h  finished kana sets (curl) and new Pixelorama drawings → critters
            waiting to drop in, kept in ~/.local/state/sandbg/progress (background thread)
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

## Invariants in critter.c

- A live critter's body is exactly its `w`x`h` rectangle of `CRITTER`
  cells, whose color byte holds its slot, and no other cell is
  `CRITTER`. Nothing else may write over a critter cell: `sim_paint()`,
  `clear_unkept()` and the strike skip them, and every critter move goes
  through `relocate()`. A critter that's away (`CR_AWAY`, off screen
  during a drain) has no cells.
- A carried cell is out of the grid (the totals already dropped) until
  it's put down with `set_cell()`.
- Wheat's `since[]` is in sim seconds (`frame / steps_per_sec`), not the
  wall clock: it rots on sim time. `set_cell()` handles that.
- Only a pour in critter.c makes or grows wheat; nothing in sim.c's water
  code knows about it.
- Every critter timing goes through `csecs()` (supercharge) except
  falling.

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
- with critters: every `CRITTER` cell belongs to a live critter's
  rectangle and every rectangle is whole, and `s->wheat` never rises by
  more than the pours converted that step (CRITTERS.md, Testing)

Then look at it with `make run`. For the external inputs, use
`--set rain_override=<drops/min>` for rain, the `s` and `l` keys for seeds
and lightning, and `--meter`, `--weather` and `--spotify` to check what
sandbg sees. For critters, the `c` key drops one, and `--progress` shows the
kana counters and drawings that earn them. Test rewards with a temporary
`XDG_STATE_HOME`, a fake kana server for `kana_url`, a temporary
`pixelorama_config`, and `SANDBG_PROGRESS_POLL_MS` to poll faster. Don't
run `--critter` while the desktop copy is up unless you mean to: it signals
every process named sandbg.

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
