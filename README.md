# sandbg

An animated falling-sand desktop background for MATE on X11.

Sand pours from the top in random bursts and piles up on stone ledges. When
the screen fills, the floor opens, everything drains out, and it starts
over. A few things outside the sandbox feed into it:

- **Music:** a blaster follows whatever is playing and blasts the pile
  apart in time with it.
- **Weather:** rain falls at a rate set by the current weather at your
  location, from [Open-Meteo](https://open-meteo.com/). Water pools,
  seeps through sand, and slowly evaporates.
- **Spotify:** each song played to the end drops a seed that turns the sand
  where it lands into dirt. Rain soaks dirt, and dirt rained on twice grows
  moss.
- **Git:** each commit, in any repo, fires a bolt of lightning. Sand it
  hits turns to glass, dirt turns to stone, stone turns to iron, and moss
  sprouts a tree that lives as long as it gets water. Iron survives the
  drain: it and the dirt, moss or trees touching it stay, while the rest
  of the pile sifts through. Now and then some or all of it rusts back to
  stone.
- **Practice and drawing:** finishing a kana practice set on the kana
  platform (5 minutes: one critter half the time, 10: one, 15: two) or
  saving a new [Pixelorama](https://pixelorama.org/) drawing drops a
  critter into the sandbox. Quitting a set early, re-saving and exporting
  don't count.
- **Keyboard:** on a Keychron Q4 running the sandbg keymap, the per-key
  LEDs mirror the sandbox: piles glow amber and brighten as they grow,
  drizzle flashes single keys while steady rain glows across the board,
  moss grows as green spots, and lightning flashes the board.

It costs well under 1% of a CPU core when it's idle, and it pauses while a
maximized or fullscreen window covers the desktop.

## Requirements

- Linux with X11. It's built for MATE (Marco and Caja); other X11 desktops
  may work with `--wid` and [xwinwrap](https://github.com/ujjwal96/xwinwrap).
- SDL2, Xlib with Xext, PulseAudio (or PipeWire's Pulse server), D-Bus,
  and `curl`.
- Optional: a Keychron Q4 for the per-key LEDs; see
  [Keyboard LEDs](#keyboard-leds).

On Debian or Ubuntu:

```
sudo apt install build-essential libsdl2-dev libx11-dev libxext-dev \
    libpulse-dev libdbus-1-dev curl
```

## Install

```
make && make install   # installs to ~/.local/bin (set PREFIX to change)
sandbg-setup           # make it the wallpaper
```

`sandbg-setup` turns off Caja's desktop icons, so that clicks reach the
sand. It also creates `~/.config/sandbg/config.ini`, adds a login autostart
entry, starts `sandbg`, and installs a global git hook for the lightning.
The hook passes through to each repo's own hooks. If you already have a
global `core.hooksPath`, it leaves that alone and prints the one line to
add to your own `post-commit` hook.

It also installs a udev rule (with `sudo`) that lets your user open the
Keychron's raw HID interface. That rule is only needed for the keyboard
LEDs; skip it and everything else still works.

To undo all of it:

```
sandbg-setup --undo
make uninstall
```

## Configure

Edit `~/.config/sandbg/config.ini`, then restart with
`pkill sandbg; sandbg &`. Every setting is described in
[config.ini.example](config.ini.example). The main ones:

| Key | Default | Meaning |
|---|---|---|
| `latitude` / `longitude` | unset | Your location for the weather; no rain while unset |
| `scale` | 4 | Screen pixels per grain; bigger is chunkier and cheaper |
| `fps` | 20 | Frames per second |
| `palette` | desert | desert, pastel, neon, mono |
| `audio` / `blast_strength` | true / 1.0 | Music-driven blaster and how hard it hits |
| `spotify_seeds` | true | Drop a seed for each song Spotify finishes |
| `lightning` | true | Strike on each git commit |
| `fill_reset` | 0.6 | Drain when this fraction of the screen is sand |
| `keyboard` | auto | Keychron LEDs: `auto`, `off` or `preview` |
| `keyboard_brightness` | 1.0 | Overall LED brightness, 0-1 |
| `critters` | true | Critters that farm, fetch water and build |
| `critter_max` | 24 | Most critters alive at once (1-32) |
| `kana_url` | `http://127.0.0.1:8080` | The kana practice platform; empty turns kana rewards off |
| `pixelorama_config` | `~/.local/share/pixelorama/config.ini` | Pixelorama's settings; empty turns drawing rewards off |

Any setting can be overridden for a single run with
`--set key=value`.

## Use

Mouse on the desktop: left-drag adds sand, right-drag adds stone,
middle-drag erases.

Keys (when the window has focus): `r` resets, `d` drains now, `s` drops a
seed, `l` strikes lightning, `c` drops a critter, `space` pauses, `q`/`Esc` quits.

```
sandbg --windowed        # run in a normal window, for trying it out
sandbg --weather         # print the current weather and the rain rate it gives
sandbg --meter           # live audio level, as the blaster sees it
sandbg --spotify         # what Spotify is playing, and each finished song
sandbg --strike          # fire one lightning strike at the running copy
sandbg --critter         # drop a critter into the running copy
sandbg --progress        # what earns critters: kana sets, drawings, critters waiting
sandbg --keyboard        # show the Keychron LED interface and keymap status
sandbg --keyboard-preview  # run with the LED colors drawn in the terminal
sandbg --profile         # print frame timings every 5 seconds
sandbg --help            # all flags
```

## Critters

Finished kana sets and new Pixelorama drawings drop tiny critters in from
the top (`c` drops one by hand). Small ones are 1x2 cells and cheap to
feed; large ones are 2x3 and do the heavy work: they mine iron, and toss
more sand. They live off the land:

- **Food:** they eat moss and wheat. A full stomach lasts about 40 minutes
  of visible time (small ones last longer); empty, they crumple into grey
  bodies that rot into dirt.
- **Farming:** water poured on moss turns it into wheat and stocks it with
  food. Only critters do this; rain never makes wheat. With no earth
  around, a critter plants a seed it carries, and pours water on dirt to
  grow moss the way rain does.
- **Water:** sand swallows raindrops at once, so critters sprint under
  falling drops to catch them, carry puddle water, and keep it in a
  covered well, where it never dries up. Rain on a hut's or well's roof
  runs down a gutter into the well, so wells are built as wide as the
  ground allows.
- **Building:** with nothing else to do on its patch, a critter usually
  presses sand into dirt, a grain every half minute (otherwise it
  strolls). New critters drop in by an existing critter's patch, live
  there, and plant their seed at its end to widen it; a colony's second
  critter then only moves sand. Dirt and fallen debris are its bricks, and large critters
  mine iron; nobody digs into a ledge's stone. They build a small well on
  their patch's iron, before most other jobs. Built on iron, it comes
  through the drain with its water. A well is a cistern: water resting
  on its roof seeps in, and loose water nearby slowly drifts toward it.
  Sand that gets in falls on through its floor. New critters drop in over stone or
  iron when there is some. Each critter arrives with a seed, and sprints to
  catch any that falls near it. It plants a seed on a stone ledge,
  turning a short stretch of it to iron that never rusts, with dirt on
  top, and a short shower falls there to grow moss on it. Critters stay
  near their patch and keep back from its edges; when the screen is
  nearly full they get onto their patch's iron, which comes through the
  drain, and wait. Stranded below its floating ledge, they build brick
  stairs up to it out of sand, a hop apart (critters hop up to 3 cells), which crumble back into sand after about
  20 minutes. Brick pressed from dirt lasts until the drain. Otherwise
  iron comes only from
  lightning, so from git commits. An all-iron hut keeps its critters, and
  the field and well water joined to it, through the drain; everyone else
  walks off the screen and comes back at the next fill.
- **Holding the fill:** a home with wheat holds the fill like a living
  tree, and critters near a side edge toss sand off the screen to make
  the hold last longer, up to about four times as long. A critter with
  no home clears the sand off the nearest stone ledge, flinging it past
  the ledge's end.
- **Lightning:** a strike near a critter supercharges it for ten minutes:
  three times as fast at everything.

Sand streams part around critters, so a stream can't pour straight onto
one, but a pile can still bury them.

## Keyboard LEDs

On a Keychron Q4 (ANSI), sandbg can drive the 61 per-key LEDs. Each key
samples the part of the screen above it. The board is dark where the
screen is empty. Sand piles glow amber from the bottom row up, brighter as
each key's patch fills and as the whole pile grows; falling sand leaves a
warm streak across the rows. In a drizzle each drop is a sharp blue-white
flash on the key it's passing; as the rain gets heavier the drops fade into
a soft blue glow over the whole board. Moss shows as green spots that
brighten as they grow and spread to the next key once they're large, and a
lightning strike flashes the whole board. Colors are gamma-corrected for
the LEDs, so `--keyboard-preview` shows them as they look on the keys.

Stock Keychron firmware speaks VIA, which only exposes whole-keyboard
effects. Per-key control needs the small keymap in
[firmware/keychron-q4-sandbg](firmware/keychron-q4-sandbg/README.md), which
adds a 32-byte raw HID protocol and keeps Keychron's default Q4 layers.
Build and flash it once with QMK; the keymap README has the exact commands.
VIA is disabled in that build. When sandbg exits, or stops talking for
three seconds, the firmware restores the effect, brightness and enabled
state it found.

The raw HID interface is root-only by default. `sandbg-setup` installs
this udev rule (with `sudo`) and reloads udev:

```
SUBSYSTEM=="hidraw", KERNEL=="hidraw*", ATTRS{idVendor}=="3434", TAG+="uaccess"
```

Check what sandbg sees with:

```
sandbg --keyboard                       # is the interface there, and answering?
sandbg --windowed --keyboard-preview    # draw the LED colors in the terminal
```

Set `keyboard = off` to leave the keyboard alone. While the desktop is
covered, sandbg only sends a keepalive once a second; when no keyboard is
found it doesn't scan the grid at all. The mapping is for the Q4 ANSI;
other models would need their LED layout added to `keyboard.c`.

## Privacy

Your configured location is sent to Open-Meteo every 10 minutes. Nothing
else leaves the machine: audio levels are read from PulseAudio and Spotify
is read over D-Bus, the kana platform's database is read over HTTP at
`kana_url` (localhost by default), and Pixelorama's config.ini is read for
newly saved drawings, all locally. What's been counted is kept in
`~/.local/state/sandbg/progress`.

## Limitations

- One grid spans the whole screen, and the "covered" check assumes a single
  monitor.
- It doesn't follow resolution changes; restart it after one.
- Snow falls as water.
- Keyboard LEDs are mapped for the Keychron Q4 ANSI only.
