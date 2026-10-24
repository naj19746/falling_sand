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
  of the pile sifts through.

It costs well under 1% of a CPU core when it's idle, and it pauses while a
maximized or fullscreen window covers the desktop.

## Requirements

- Linux with X11. It's built for MATE (Marco and Caja); other X11 desktops
  may work with `--wid` and [xwinwrap](https://github.com/ujjwal96/xwinwrap).
- SDL2, Xlib with Xext, PulseAudio (or PipeWire's Pulse server), D-Bus,
  and `curl`.

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

Any setting can be overridden for a single run with
`--set key=value`.

## Use

Mouse on the desktop: left-drag adds sand, right-drag adds stone,
middle-drag erases.

Keys (when the window has focus): `r` resets, `d` drains now, `s` drops a
seed, `l` strikes lightning, `space` pauses, `q`/`Esc` quits.

```
sandbg --windowed        # run in a normal window, for trying it out
sandbg --weather         # print the current weather and the rain rate it gives
sandbg --meter           # live audio level, as the blaster sees it
sandbg --spotify         # what Spotify is playing, and each finished song
sandbg --strike          # fire one lightning strike at the running copy
sandbg --profile         # print frame timings every 5 seconds
sandbg --help            # all flags
```

## Privacy

Your configured location is sent to Open-Meteo every 10 minutes. Nothing
else leaves the machine: audio levels are read from PulseAudio and Spotify
is read over D-Bus, both locally.

## Limitations

- One grid spans the whole screen, and the "covered" check assumes a single
  monitor.
- It doesn't follow resolution changes; restart it after one.
- Snow falls as water.
