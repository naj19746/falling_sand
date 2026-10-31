# Plan: critters that live in the sandbox

## What it does
Finishing things you practice or make drops tiny critters into the sandbox:

| You finish | Critters |
|---|---|
| A 15-minute kana practice set (hiragana or katakana) | 2 |
| A 10-minute set | 1 |
| A 5-minute set | 1, half the time |
| Saving a new Pixelorama drawing | 1 |

Critters live off the land and look after their patch of it:

| Need or job | What they do | What happens if it goes wrong |
|---|---|---|
| **Food** | Eat moss (a small meal) or wheat. Each bite of wheat uses some of it up | Food runs out after about 40 minutes without eating: they starve |
| **Farming** | Pour water on moss to turn it into a wheat field, and keep pouring to grow more wheat on it. Only critters can do this: rain and puddles never turn moss into wheat or grow it | Eaten-out wheat goes back to moss, and wheat left unwatered for hours slowly rots |
| **Water** | Carry rain and puddle water into a covered well, where it never evaporates | Without a well, puddles dry up within minutes of the rain stopping, and the field stops growing |
| **Building** | Mine iron (and stone when there's no iron), carry it home, and build a hut and a well | An iron hut, floor included, survives the drain with its critters inside. A stone hut crumbles like the ledges |
| **Keeping clear** | Dig sand off themselves, their hut door, their well's mouth and their field. Critters near a side edge toss sand off the screen, which holds off the next drain | Buried too deep for too long, they suffocate |
| **Dying** | They crumple over into a small grey body, which falls like sand and slowly rots into dirt | |
| **Lightning** | A strike within 20 cells supercharges them for 10 minutes: three times as fast at everything | |

Critters come in two kinds, fixed for life: **small** (1×2 cells) and
**large** (2×3). At the default `scale = 4` that's 4×8 and 8×12 screen
pixels. They don't have young; only finished practice sets and drawings
add critters.

Critters are slow: they amble about 2 cells a second. A lightning strike
near them **supercharges** them for a while, and they move and work three
times as fast. So git commits help twice: they bring the iron and the
energy.

## Approach
- **Entities over cells, like trees.** Each critter is a slot in a fixed
  array in `Sim` (position, size, food, what it's carrying, its goal). Its
  body is written into the grid as `CRITTER` cells, so sand piles on it,
  water flows around it, and the renderer draws it with no special overlay.
  Moving a critter lifts its cells and places them again through
  `set_cell()`, so totals and chunk wakes stay right.
- **Cheap behavior.** No pathfinding. A critter decides on a goal about once
  a second by scanning a small window around itself, then walks greedily
  toward it, climbing one-cell steps and digging through loose material.
  If it makes no progress for a while, it gives up and picks another goal.
- **Their time is the simulation's time.** Hunger and burial count sim
  steps, and so does wheat's slow rot. Critters don't starve, and fields
  don't rot, while the desktop is covered or the computer is off. Only
  bodies rotting into dirt use the wall clock, the way moss already dries.
- **Rewards come from a background thread, like the weather.** A new
  `progress.c` watches the kana database and Pixelorama's config, and
  hands the main loop a count of critters earned. The main loop drops them
  in one at a time while the desktop is visible.
- **New code in `critter.c`.** `sim.c` is already ~1700 lines. The helpers
  critters need from it (`set_cell`, `rnd`, `load_on`, ...) move to a small
  internal header, `sim_internal.h`. Neither file depends on SDL or X11.

## 1. New cell types (`sim.h`)
```c
enum { EMPTY, STONE, IRON, WATER, GLASS, WOOD, LEAF, ROOT, CRITTER,     /* static */
       SAND, DEBRIS, SEED, DIRT, WET_DIRT, MOSS, WHEAT, SHARD, BODY };  /* fall */
```
- **Ordering:** `CRITTER` doesn't fall on its own, so it goes before
  `SAND`. `WHEAT` and `BODY` fall, so they go after it.
- **`CRITTER`:** a critter's body cell. The color byte holds the slot
  (low 5 bits, `MAX_CRITTERS` ≤ 32) and the body part (top 3 bits: head,
  eye, body, leg, leg-alt), so the sim can find a critter from any of its
  cells and the renderer can draw parts differently. Not `SOLID` (it
  doesn't crumble or hold up the drain), not `LOOSE` (the blaster spares
  it), and `sand_can_enter()` refuses it, so sand rests on it.
- **`WHEAT`:** a field cell that was moss. It behaves exactly like moss:
  held in place over small gaps, part of `DAMP()` and `EARTH()`, joined
  to iron by `HELD_BY_IRON()`, and spared by the blaster. The color byte
  holds a stock level, 0–7 (low 3 bits): how much eating is left in the
  cell. The renderer shades it from pale green at 0 to deep gold at 7.
  Fields form because one pour of water converts all the moss within
  `FIELD_SPOT` (3) cells, and critters keep pouring next to what's already
  wheat.
- **`BODY`:** what a critter leaves when it dies. Grey, falls and drains
  like sand, and rots into `DIRT` after `BODY_ROT_MINUTES` (30) of wall-clock
  time. Use the existing `since[]` timestamp, and have `age_damp()` (or a
  sibling sweep) check bodies the same way.
- **Built cells:** iron or stone a critter placed has a new color flag
  `BUILT = 0x80`, so critters don't mine their own or each other's
  buildings. `place_ledges()`, the lightning strike and the other iron and
  stone writers must clear that bit (`rnd(s) & 0x7F`). `rust_iron()` keeps
  the color byte, so a rusted wall stays theirs, as built stone they can
  replace. The renderer can shade the upper half of the stone and iron
  tables slightly so built walls read as bricks.
- **Running totals:** add `wheat` and `bodies` next to `glass` and `iron`
  in `set_cell()`. `CRITTER` cells aren't counted in `grains` or `solids`,
  so the drain's end check doesn't wait for them.
- **Renderer table:** `render.c` indexes `lut[CELL_TYPE(c) & 15]`, and
  this takes the type count to 18. Grow it to `lut[32][256]` and change the
  mask to `& 31`.

## 2. The critter (`sim.h`, `critter.c`)
```c
#define MAX_CRITTERS 24
enum { CR_FREE, CR_FALLING, CR_IDLE, CR_WALK, CR_EAT, CR_FETCH, CR_CARRY,
       CR_MINE, CR_BUILD, CR_DIG, CR_DYING, CR_AWAY };
typedef struct {
    int state;
    int x, y;               /* bottom-left cell of the body */
    int kind;               /* CR_SMALL (1x2) or CR_LARGE (2x3), for life */
    int w, h;               /* from kind */
    int8_t face;            /* -1 left, 1 right */
    float food;             /* 0..1; starves at 0 */
    uint32_t charged_until; /* sim frame; supercharged until then */
    uint16_t carry;         /* the cell it's holding, or 0 */
    int goal, tx, ty;       /* what it's doing and where */
    int stuck;              /* steps without progress toward the goal */
    int home;               /* index into s->home[], or -1 */
    uint32_t buried_steps;  /* steps spent buried past BURY_DEPTH */
    uint32_t next_think;    /* sim frame of the next decision */
    uint32_t next_move;
} Critter;
```
`Sim` gains `Critter crit[MAX_CRITTERS]`, `int critters` (slots in use),
`Home home[MAX_HOMES]` (anchor, hut and well plans and their progress)
and the config values below.

**Public entry points:**
- `int sim_add_critter(Sim *s)`: drop a new critter from the top of the
  screen. Its kind is a coin flip, except that a reward of two critters
  (a 15-minute set) brings one of each. It prefers a column near moss, wheat or another critter's home,
  and falls back to a random clear column. Returns -1 when every slot is
  full.
- `void update_critters(Sim *s)`: called from `sim_step()` after
  `update_trees()`, only when `s->critters > 0`.

**Body and movement:**
- **Slow by default:** a critter moves at most once every
  `CRITTER_MOVE_STEPS` (30) steps, about 2 cells a second at the default 60
  steps a second (`fps` 20 × `steps_per_frame` 3). Falling is one cell
  every 3 steps, so a critter falls at sand's pace.
- **Supercharged:** while `frame < charged_until`, every timing in this
  plan is divided by `CHARGE_SPEEDUP` (3): moving (6 cells a second),
  eating, mining, tossing, thinking. Falling isn't affected.
- **Fits:** every cell of the target rectangle is `EMPTY`, `WATER` or its
  own body. Water it steps into is moved into the cells it just left, so
  the water total doesn't change.
- **Gravity:** if nothing under its feet holds it up (every cell below is
  `EMPTY` or `WATER`), it falls.
- **Walking:** step one cell toward the target. If that's blocked, try one
  cell up (climb). If it's still blocked by loose material, dig (pick up the
  blocking cell). If it's blocked by unbuilt iron or stone and the goal
  needs building material, mine it. Otherwise turn around and count a
  stuck step.
- **Drawing it:** the large body is a head row (with an eye on the side
  it's facing), a body row and a leg row. While walking, the leg cells swap
  between `leg` and `leg-alt` each step. The small body is just head and
  body. Idle critters stand still and don't touch their cells, so their
  chunk can sleep.
- **Drawing a charge:** a supercharged critter's body cells flicker
  between `body` and a `spark` part (an electric blue-white) every move,
  and while it's idle too, so a charged critter keeps its chunk awake until
  the charge runs out.
- **The two kinds:**

  | | Small (1×2) | Large (2×3) |
  |---|---|---|
  | Food burn | 0.6× | 1× |
  | Mining | stone and debris only | iron and stone |
  | Tossing | 1 cell per toss | `TOSS_SCOOP` (3) |
  | Fits through | 1-wide, 2-tall gaps | 2-wide, 3-tall |

  Both carry one cell at a time. Small critters are cheap to feed and do
  most of the water fetching; large ones do the iron work and the tossing.
  A colony needs both to thrive, which is why a 15-minute set brings one
  of each.

**Carrying:** the carried cell is taken out of the grid with `set_cell()`
(so totals drop) and written back when it's put down. The renderer draws it
above the critter's head as an overlay, the way it draws the bolt. A critter
that dies drops what it was carrying where it stands.

## 3. Deciding what to do (`critter.c`)
About once a second (`THINK_STEPS` = 60, staggered by slot), and whenever a
goal finishes or fails, a critter scans a window of ±`SENSE` (40) cells
around itself and takes the first of these that applies:

1. **Dig out.** If loose cells sit on its head, or its hut door is blocked,
   pick up the top one and drop it outside the home area.
2. **Eat.** If `food < 0.5`, walk to the nearest wheat with stock in it,
   or moss if there's none. Each bite takes `EAT_STEPS` (120, 2 s):
   - **wheat:** one stock level, for `WHEAT_BITE` food (0.33). A cell
     eaten down to 0 goes back to moss.
   - **moss:** 0.25 food, and the moss turns to wet dirt.

   A critter keeps taking bites until `food` is above 0.9. Critters eat
   the fullest wheat cells first, which keeps a field even.
3. **Clear.** Loose cells on the critter's field, in its well's mouth, or on
   its hut roof edge get carried out past `HOME_RADIUS` (12 cells).
4. **Farm.** If the field's total stock is below what the home's critters
   eat in `FIELD_RESERVE_HOURS` (2), fetch a water cell (from puddles
   first while there are any, then the well), carry it to the field, and
   pour it. Pour on the emptiest part of the field, or on moss next to it
   to make the field bigger, or on any moss if there's no field yet.
   Section 4 says what a pour does. A pour is the only way moss becomes
   wheat, and the only way wheat grows.
5. **Stockpile water.** Puddles near home, and a well with room in it:
   carry the water to the well and pour it into the mouth. This comes
   ahead of building whenever there's open water around, because puddles
   are gone within minutes (see Balance). It includes single stray drops:
   a critter that sees a fresh drop land within `HOME_RADIUS` (from the
   `rain_at` map) runs to grab it before it dries. Once the hut is finished, a
   critter with nothing else to do deepens the well by one row, up to
   `WELL_MAX_ROWS` (5).
6. **Build.** No home yet: pick a site (ground within ±1 cell of level over
   7 cells, near food, at least `HOME_RADIUS` from other homes) and plan a
   hut, then a well beside it. With a plan, fetch material in this order:
   - unbuilt `IRON` anywhere on screen (`MINE_STEPS` = 270, 4.5 s; iron
     is hard). Iron is rare, so this is the one search that isn't limited
     to `SENSE`. A critter will make a long trip for it
   - unbuilt `STONE` (`MINE_STEPS` = 180, 3 s), or a fallen `DEBRIS` grain

   Then place it as built (`IRON | BUILT` or `STONE | BUILT`) in the lowest
   plan cell that has something solid next to it. Floor cells go in first.
   If the hut has any stone in it and iron turns up later, the critters
   swap the stone out one cell at a time, so a hut becomes all iron.
7. **Toss.** While the screen is full and being held (section 6), a
   critter within `TOSS_RANGE` (30 cells) of a side edge tosses sand off
   the screen. Critters further in clear their home area as usual.
8. **Idle.** Stand by the hut, and now and then wander a few cells.

Building plans (`#` iron or stone, `.` must be empty, `_` ground):
```
hut (5x5, door on the side away from the well)    well (7x4 to start)
 .###.                                             ###.###   <- roof, 1-cell mouth
 #...#                                             #.....#
 #....                                             #.....#   <- holds 10; each extra
 #....                                             #######      row adds 5
 #####   <- floor: an iron floor is what holds     _______
 _____      the hut up during the drain
```
**Wells:**
- **Water in a well never evaporates.** `Sim` keeps each well's interior
  rectangle in its `Home`. `evaporate()` skips a water cell inside any of
  them. That's a check against at most `MAX_HOMES` rectangles, and only for
  the few cells it samples. Without this rule, the water right under the
  mouth would be open to the air and dry up.
- **Rain collects on its own:** drops that fall through the mouth stay.
  It's only one cell wide, so this tops a well up slowly; carrying fills it.
- **Drawing water:** a critter standing on the roof next to the mouth takes
  the top water cell anywhere in the interior, like a bucket on a rope.
- **The drain:** water inside an iron well is kept through the drain, like
  the well itself. `mark_kept()` marks it from the well rectangles.
  `set_cell()` already keeps the kept mask right as the water moves within
  the well.

## 4. Wheat fields (`critter.c`, swept from `sim.c`)
Wheat is stored food. Water goes in as pours, and food comes out as
bites.
- **A pour:** uses up one water cell and adds `WHEAT_PER_POUR` (3) stock
  levels to the field around where it lands (within `FIELD_SPOT`, 3
  cells). Moss in that spot turns into wheat at level 0 first. Then the
  levels go one at a time to the lowest cells, up to 7 each. A pour with
  no moss or room in reach isn't made; the critter takes its water to the
  well instead.
- **The exchange rate:** one pour is worth `WHEAT_PER_POUR × WHEAT_BITE` =
  **1.0 food**: a full stomach, about 40 minutes of a critter's life. This
  is the main balancing number (see Balance).
- **Eating:** a bite takes one level (section 3). A cell at 0 goes back to
  moss.
- **Rot:** a wheat cell loses one level every `WHEAT_ROT_HOURS` (3) of
  sim time since its last pour, so a forgotten field slowly fades back to
  moss. Sim time doesn't pass overnight, so a stocked field is still there
  in the morning. Keep a per-home step count of the last pour rather than
  using `since[]`, which is wall-clock seconds.
- **Only pours, ever.** Whether or not there are critters on the map, and
  even when rain lands right next to a critter or on a field, rain landing
  on moss or wheat, puddles flowing onto it, and water seeping through it
  (`soak()`, `through_moss()`) keep doing what they do now. They never
  make wheat or grow it. To make sure of that, conversion and growth live
  in `critter.c` as part of the pour itself: the critter uses up the water
  cell it's carrying and changes the cells directly. It never puts a water
  cell into the grid that the water code could act on later. Nothing in
  `sim.c`'s water handling learns about wheat.
- **Sweeping:** fold wheat's rot into `age_damp()`, which already visits
  every damp cell about once a second, comparing against sim steps for
  wheat.

## 5. Dying (`critter.c`)
- **Starving:** food drops by `1 / (critter_starve_minutes * 60 *
  steps_per_sec)` each step. `Sim` gains `steps_per_sec`, which `main.c`
  sets to `fps * steps_per_frame` (60 by default), so every timing here can
  be written in seconds. At 0 the critter dies.
- **Buried:** when `load_on()` above its head is more than `BURY_DEPTH`
  (10 cells) in every column, or it hasn't been able to move or dig for a
  while, `buried_steps` counts up. Past `BURY_STEPS` (60 s worth) it dies.
  Digging itself out resets the count.
- **Crumpling:** `CR_DYING` takes about 1.5 s. Over three moves the body
  folds: a large critter ends as a 3×1 line of `BODY` cells (2×1 for a
  small one), placed where it fits, falling back to the critter's own column. The
  slot is freed and the home is left for the others to use.

## 6. The drain (`sim.c`)
- **Iron huts survive.** `mark_kept()` already keeps every iron cell and
  the earth joined to it, and `WHEAT` is earth, so a field touching an
  iron hut or well is kept too, and so is the water in an iron well
  (section 3).
- **`sim_start_drain()`:** a critter stays if it's inside or on a hut whose
  walls and floor are all iron, or if it's standing on any kept cell. It
  goes to that hut and waits there (`CR_IDLE`, no wandering) until the next
  fill.
- **Everyone else leaves.** Critters without an iron hut set `CR_AWAY`,
  walk to the nearer side of the screen, and leave when they get there.
  Any still on screen when `clear_unkept()` runs are lifted out then.
- **`clear_unkept()`:** away critters come back at the start of the next
  fill, dropping in from the side they left by, with the food they had.
  Homes that didn't survive are forgotten. Critters whose iron hut
  survived keep it.
- **Where iron comes from:** only lightning, which only comes from git
  commits. That's intended. A stretch without commits means critters build
  in stone and their huts don't survive drains, so practice sets and
  drawings bring critters, and commits keep them safe.
- **Rust:** `rust_iron()` turns some iron back to stone after each drain,
  so huts slowly weaken. Critters repair them: a hut with stone in it is
  back on the build list (step 6 of section 3).
- **Holding off the drain:** a living farm holds the fill the way a
  living tree already does. In `sim_step()`, the hold becomes
  `full && (s->trees > 0 || s->farms > 0)`, where `farms` counts homes
  with at least one wheat cell. While held, the emitters pour at the
  `HOLD_POUR_DIV` trickle until the screen reaches `drain_overfill`.
  Measured at 480×270 with the default 2 emitters: the screen fills in
  about 9 minutes, the trickle is about 18 grains a second, and the hold
  lasts about 24 minutes. A cycle goes from about 10 minutes to about 33.
- **Tossing sand off the screen:** while held, critters within
  `TOSS_RANGE` of a side edge pick up `TOSS_SCOOP` (3) loose cells at a
  time and toss them off the edge. One toss takes `TOSS_STEPS` (180, 3 s):
  1 grain a second for a large critter, a third of that for a small one,
  and three times as much while supercharged. The toss is drawn as a short arc overlay, and the cells
  are removed with `set_cell()`. `s->tossed` counts them.
- **Tossing slows the fill, but can't stop it.** Tossed grains count as
  poured for the stall check, so critters keeping up doesn't look like a
  stalled fill and trigger a drain. They can offset at most `TOSS_SHARE`
  (75%) of the trickle: once tossing in the current stall window has
  removed that share of what was poured, further tosses only move sand
  around. A hold therefore lasts from about 24 minutes (no tossers) up to
  about 95 (enough tossers), and the screen always drains in the end.
  Reaching the cap takes about 14 grains a second of tossing: about 14
  large critters at their normal pace, or 5 supercharged. In practice a
  long hold needs a recent commit.
- **Edge homes:** this makes a home near a side edge worth more, so site
  choice (section 3, step 6) gives a bonus to sites within `TOSS_RANGE` of
  an edge.
- **Drain end check:** unchanged. `CRITTER` cells aren't in `grains` or
  `solids`. `BODY` cells are grains and drain out with the sand.
- **Lightning supercharges critters.** A strike never targets or changes
  `CRITTER` cells: `strike_target()` skips them, and the struck disc leaves
  them alone. Instead, every critter within `CHARGE_RADIUS` (20 cells) of
  the impact, or touching the bolt's path, gets
  `charged_until = frame + CHARGE_MINUTES` (10 minutes of sim time). A
  second strike extends it rather than stacking the speed. A bolt that hits
  a stone hut turns it to iron like any other stone, and the `BUILT` bit
  stays.

## 7. Earning critters (`progress.c/.h`, background thread)
A new thread, started like the weather thread, polls both sources. It
counts critters earned, under a mutex. The main loop reads that count the
way it reads `spotify_completed()`.

**Kana practice.** The hiragana and katakana apps (`../hiriagna-p1`,
`../katakana-p1`, served by `../kana-platform`) keep their state in the
database service. Each finished session increments `state.sessionsCompleted`,
and the chosen length is in `state.settings.sessionMinutes`.
- **Poll:** every 30 s, run
  `curl -s --fail -m 5 http://127.0.0.1:8080/api/db/hiragana-practice` and
  the same for `katakana-practice`, the way `weather.c` runs curl. Parse
  `sessionsCompleted`, `settings.sessionMinutes` and `lastSession` (below)
  with the same small hand-rolled field lookup weather.c uses.
- **A small change in both kana apps.** `sessionsCompleted` also goes up
  when a session is quit early (`endReason 'ended'`) or runs out of kana
  (`'nothingLeft'`), so it can't tell a completed set from an abandoned
  one. In `Session.finish()`, next to the `sessionsCompleted` increment,
  also save:
  ```ts
  lastSession: { endedAt: now, minutes: this.minutes, activeMs, endReason }
  ```
  as an optional field in the state document. Add it to `types.ts`, and
  check that the state validator and import/export accept it. This goes in
  both apps' repos as its own commit there.
- **Counting:** when `sessionsCompleted` goes up and `lastSession.endedAt`
  is newer than the last one handled, the set counts as completed if
  `endReason` is `'time'` or `'cards'`. Then:
  - `minutes >= 15`: 2 critters
  - `minutes >= 10`: 1 critter
  - `minutes >= 5`: 1 critter, half the time
  - shorter: none

  If `lastSession` is missing (an app without the change), fall back to
  `settings.sessionMinutes` and count every increment.
- **Service down:** a failed poll changes nothing. When the service comes
  back, the counter catches up, and sets finished in the meantime still
  count, but only the latest one's length is known. Count each missed set
  at that length.

**Pixelorama.** Pixelorama rewrites
`~/.local/share/pixelorama/config.ini` with `last_project_path` and
`recent_projects` when a project is saved.
- **Poll:** every 5 s, `stat()` the config. When its mtime changes, read
  `last_project_path` and the `recent_projects` list.
- **New drawing:** a `.pxo` path in either one that sandbg hasn't seen
  before, which exists on disk. Saving an existing drawing again doesn't
  count; "Save as" under a new name does. Exports (`.png`) don't count.
- **Seen paths:** kept in the state file below. On the very first run,
  everything already in the config is marked seen, so old drawings don't
  pay out.
- **To check before building:** confirm by saving a new drawing that
  Pixelorama really writes the config on every save, and not only on exit.
  If it only writes on exit, watch the directories of the recent projects
  for new `.pxo` files with inotify instead.

**State file:** `$XDG_STATE_HOME/sandbg/progress` (default
`~/.local/state/sandbg/progress`). It holds the last `sessionsCompleted`
and `lastSession.endedAt` per kana database, the seen `.pxo` paths, and
critters earned but not yet dropped in. Rewrite it (to a temp file, then
rename) whenever it changes. Critters earned while sandbg isn't running
are picked up on its next start, since the counters are compared then.

**Main loop:** critters waiting = earned − dropped. Drop at most one every
2 s, only while the desktop is visible and not paused, and only while there
are free slots. Earned critters wait (and stay in the state file) until
there's room, rather than being lost.

**For testing:** a `c` key drops a critter, and `sandbg --critter` makes the
running copy drop one (sending `SIGUSR2`, the way `--strike` sends
`SIGUSR1`). Add both to `usage()`. `SIGUSR2`'s default action kills the
process, so it must always be handled, or set to `SIG_IGN` with
`critters = false`. `sandbg --progress` prints what the thread sees (both
kana counters, the last session, the newest Pixelorama path) and exits,
like `--weather`.

## 8. Config (`config.c/.h`, `config.ini.example`, README)
| Key | Default | Meaning |
|---|---|---|
| `critters` | true | Turn critters on |
| `critter_max` | 24 | Most critters alive at once (1–32) |
| `critter_starve_minutes` | 40 | Minutes of sim time from full to starving |
| `wheat_per_pour` | 3 | Wheat stock levels one pour of water adds; each bite is 1/3 of a meal |
| `kana_url` | `http://127.0.0.1:8080` | The kana platform; empty turns kana rewards off |
| `pixelorama_config` | `~/.local/share/pixelorama/config.ini` | Empty turns drawing rewards off |

Follow the "Adding a config setting" checklist in AGENTS.md. Also add
`critter.c`, `sim_internal.h` and `progress.c/.h` to the layout list
there, and note under Privacy in the README that sandbg reads the kana
database and Pixelorama's config, both locally.

## 9. Keyboard (`keyboard.c`, optional)
Living critters give their key a faint warm white, and a death flashes the
key grey for a second. Leave this for after the rest works.

## Balance
**The goal:** a typical day for you gives an OK chance of keeping a few
critters around over time. Food is plentiful when there's water, water is
what runs out, and iron is what lets a colony outlast the drains.

### Your typical day
This is what the balance is tuned against. The harness simulates it.
- **Visible hours:** sandbg is visible about 8 hours. Sim time only passes
  then, and every critter timer runs on sim time, so the rest of the day
  doesn't count.
- **Kana:** one 15-minute hiragana set, which brings **2 critters a day**.
- **Code:** a little development, about 4 commits spread through the day.
  That's 4 lightning strikes, the only source of iron.
- **Weather:** mostly clear. Clear skies still give 2–5 stray drops a
  minute (weather.c), plus maybe one short shower.

### What the sim already sets
At the default 60 steps a second and 480×270 grid:
- **Evaporation:** water open to the air lasts about 2.3 minutes in dry
  weather and about 18 while raining. Covered water and water in a well
  never evaporates.
- **Fill and drain cycle:** without a hold, about 10 minutes, so 48 drains
  a day. With a farm holding the fill, 33 minutes, or up to about 105 with
  critters tossing at an edge: 5–15 drains a day.
- **Rust:** after each drain, 5% of the time all iron rusts, and 10% of the
  time each iron piece has a half chance. That's about a 10% chance per
  drain that a given hut rusts. Over 5–15 drains, most iron huts need
  some repair each day, which is what the next day's commits are for.
- **Iron needed:** a hut is 12 cells and a starter well 17. Each strike
  that hits stone turns everything in a 7-cell radius to iron; check in the
  harness how much a typical strike yields and how often strikes hit
  stone.

### Demand
- **Burn:** a large critter burns 1.5 food per visible hour (a full
  stomach in 40 minutes): **12 food a day.** A small one burns 0.6× that:
  about 7. A mixed pair needs about 19.
- **Wheat:** one bite is 0.33 food, so that's 36 bites a day.

### The exchange rate: one pour = 1.0 food
`WHEAT_PER_POUR` 3 levels × `WHEAT_BITE` 0.33. A critter needs **12 pours
of water a day.** The reasoning:
- **Clear-weather income:** at ~3 stray drops a minute across the screen,
  about 5% land within a home's `HOME_RADIUS`: 9 an hour. Each lasts about
  2 minutes, which is plenty of time to reach one at 2 cells a second.
  Critters busy with other jobs catch maybe a third to a half, so about
  3–4 an hour, or 25–30 a day.
- **Speed limits hauling, not catching:** a 40-cell round trip to a puddle
  takes about 40 s at normal pace (13 s supercharged). A shower's puddles
  last about 18 minutes, so one critter hauls ~25 cells from a shower, or
  ~80 supercharged. A well fills from one shower when two or three
  critters work it, or one after a commit.
- **A shower** fills the well (10, up to 25 once deepened), and an iron
  well keeps it through drains.
- **Supply vs demand:** together that's about 35–55 pours a day for a home
  that's working well, enough for 2–3 mixed pairs. With a pair arriving a
  day, the population should hover around 2–4, losing some to burial, bad sites and
  dry spells. That's the "OK chance".
- **Drains:** a colony without iron loses its field and well water at the
  first drain, comes back with at most a full stomach, and usually starves
  within the hour unless seeds and rain have made moss nearby. So the
  commits matter: they decide whether a colony lasts a few drains or a few
  days.

### Targets
Run each scenario over 20 seeds, for 7 simulated days of 8 visible hours.
1. **Typical day:** in at least half the runs, critters are alive at the
   end of the week. On most evenings the population is 1–4, and it never
   reaches `critter_max`.
2. **No commits** (no iron): colonies rarely last past the day they
   arrived.
3. **Commits, no showers all week:** the population shrinks, but the
   colony that was there on day 1 lasts at least 2 days.
4. **A set every day plus daily showers:** the population grows to about
   5–8 and levels off there, rather than growing without limit.
5. **Busy, not idle:** in the typical day, critters spend roughly a third
   to a half of their time fetching, carrying or pouring water.

### Knobs, in the order to reach for them
- `wheat_per_pour` (the exchange rate): 2 makes it harder (0.67 food a
  pour), 4 easier.
- How eagerly critters chase stray drops: the share of clear-weather
  income they catch.
- `TOSS_SHARE`: how long a farm can hold off a drain, and so how often
  rust and drains hit.
- `critter_starve_minutes`.
- `WELL_MAX_ROWS`.

Leave the evaporation rates, rust and the emitters alone. They set how the
rest of the sandbox looks, and wells and tossing are the levers critters
get over them.

## Implementation steps
Each step is one commit on the `critters` branch. Each leaves sandbg
building cleanly with `-Wall -Wextra` and running normally, and adds its
checks to the headless harness (see Testing) before the next step starts.
Every step keeps the standing checks passing too: totals match a full
recount, the grid settles, and fill/drain cycles complete.

There are two tracks:
- **Sim track:** steps 1–11, in order.
- **Rewards track:** steps 12–14. They don't touch the sim, so they can
  be done any time after step 3 (they need `sim_add_critter()`).

Step 15 needs both tracks done, and step 16 is optional.

### Sim track

**1. Groundwork, with no behavior change**
- **`sim_internal.h`:** move the helpers critter code will need out of
  `sim.c`: `set_cell`, `rnd`, `rndf`, `mark`, `load_on`, `sand_can_enter`,
  `move`. Static inline helpers go into the header as `static inline`.
- **`steps_per_sec`:** add it to `Sim`, set from `fps * steps_per_frame` in
  `main.c`.
- **Makefile:** add `critter.c` (an empty file for now) to the build.
- **Done when:** a fixed-seed headless run produces a grid identical,
  byte for byte, to one before the change.

**2. New cell types**
- **`sim.h`:** add `CRITTER`, `WHEAT` and `BODY` in the enum positions from
  section 1, and update `DAMP()`, `EARTH()` and `HELD_BY_IRON()` for
  wheat.
- **Totals:** add the `wheat` and `bodies` running totals to
  `set_cell()`.
- **`BUILT`:** add the flag, and clear it in every stone and iron writer
  (`place_ledges()`, the strike, `sim_paint()`).
- **`render.c`:** grow the table to `lut[32][256]` with a `& 31` mask, and
  add colors for critter parts (including `spark`), wheat levels 0–7,
  bodies, and the brick shading for `BUILT`.
- **Temporary paint keys:** add debug-only keys to paint wheat and bodies,
  so the colors can be checked in `make run`. Remove them in step 11.
- **Done when:**
  - recounts match with wheat and bodies painted in
  - no ledge or strike ever makes a `BUILT` cell
  - wheat behaves like moss: it's held over gaps and kept by iron

**3. Bodies on the grid and movement** (`critter.c`)
- **Critters:** the `Critter` struct, both kinds, `sim_add_critter()`, and
  `update_critters()` hooked into `sim_step()`.
- **Moving:** lift and place the body rectangle, moving water into the
  cells it leaves. Gravity, walking, climbing one cell, turning around,
  and an idle wander.
- **Drawing:** parts, the eye on the facing side, and leg animation.
- **The `c` key:** drops a critter.
- **Done when:**
  - every `CRITTER` cell belongs to a live critter's rectangle, and every
    rectangle is fully `CRITTER` cells
  - sand piles up on a critter's head
  - water flows around critters and the water total holds
  - with only idle critters, chunks sleep
  - neither kind ever changes size

**4. Supercharge**
- **Strikes:** `sim_strike()` skips critter cells as targets and in the
  struck disc, and charges every critter within `CHARGE_RADIUS` or on the
  bolt's path.
- **Speed:** every critter timing goes through one helper that applies
  `CHARGE_SPEEDUP`.
- **Drawing:** the spark flicker.
- **Done when:**
  - critter cells survive a strike right on top of them
  - a charged critter covers about 3× the distance in the same time
  - the charge wears off after `CHARGE_MINUTES`
  - a second strike extends it but doesn't add more speed

**5. Food and starving**
- **Food:** burn by kind on sim time, and eating moss (the walk to it and
  the bite).
- **Dying:** starving, `CR_DYING`, and crumpling into a 3×1 or 2×1 line of
  `BODY` cells.
- **Bodies:** rot into dirt by wall clock, using `since[]` and a sweep
  alongside `age_damp()`.
- **Done when:**
  - with no food anywhere, a large critter dies on schedule (40 minutes
    of sim time) and a small one at 1/0.6 of that
  - each death leaves the right number of bodies, and they rot to dirt
  - a critter next to moss stays alive while the moss lasts

**6. Carrying, burial and digging**
- **Carrying:** pick up and put down through `set_cell()`, the
  carried-cell overlay in `render.c`, and dropping the cell on death.
- **Digging:** dig through loose cells while walking, dig out from under
  sand, and dump it outside `HOME_RADIUS` (around the critter for now,
  until homes exist).
- **Burial:** `buried_steps` and death by burial.
- **Done when:**
  - totals stay right while things are carried
  - an emitter pouring on an unsheltered critter kills it in about
    `BURY_STEPS`
  - a critter under a thin layer digs itself out

**7. Wheat**
- **The pour action:** fetch a water cell from a puddle, carry it, and
  pour. The pour converts moss to wheat at level 0 and spreads
  `WHEAT_PER_POUR` levels within `FIELD_SPOT`.
- **Eating wheat:** fullest cells first, until `food` is above 0.9. A cell
  eaten to 0 goes back to moss.
- **Rot:** on sim time, with a per-home last-pour step. Until homes exist,
  keep it per field spot.
- **Debug counter:** cells converted or grown by pours, for the harness.
- **Done when:**
  - pours × 3 = levels on the field + bites eaten + levels rotted
  - "only pours make wheat" passes both ways, with and without critters
    under heavy rain

**8. Homes and wells**
- **Homes:** the `Home` struct and site choice (level ground, near food,
  spaced out, with an edge bonus).
- **Building wells:** from stone and debris only for now.
- **Well water:** the no-evaporation check in `evaporate()`, stockpiling
  water into the mouth, drawing it out, chasing stray drops via
  `rain_at`, and deepening the well.
- **Priorities:** the full priority list from section 3, except iron,
  tossing and huts.
- **Done when:**
  - well water lasts hours in dry weather while a puddle beside it dries
    in minutes
  - rain through the mouth collects
  - critters keep a field stocked from the well through a dry spell

**9. Huts and iron**
- **Huts:** the hut plan and floor-first placement.
- **Iron:** mining anywhere on screen, by large critters only. Prefer
  iron, swap stone for iron as it turns up, and repair rusted cells.
- **Clearing:** loose cells off the roof, the door, the well mouth and the
  field, out past `HOME_RADIUS`.
- **Done when:**
  - given stone, a colony builds a full hut and well
  - after strikes on nearby stone, the hut becomes all iron
  - small critters never mine iron

**10. The drain**
- **Staying:** `sim_start_drain()` keeps critters in all-iron huts, and
  `mark_kept()` adds well water from the well rectangles.
- **Leaving:** everyone else goes `CR_AWAY`. They're removed in
  `clear_unkept()` if they're still on screen, and come back at the next
  fill.
- **Homes:** forget homes that didn't survive.
- **Done when:**
  - several cycles in a row complete
  - an iron-hut colony keeps its hut, field and well water across drains
  - other critters leave and come back with their food

**11. Holding the fill and tossing**
- **Holding:** count `farms` and extend the hold rule in `sim_step()`.
- **Tossing:** the action, its arc overlay, and `s->tossed`, by kind and
  charge. Tossed grains count toward the stall check, up to the
  `TOSS_SHARE` cap.
- **Cleanup:** remove step 2's debug paint keys.
- **Done when:**
  - a farm alone holds the fill for about 24 minutes
  - charged tossers stretch the hold to no more than about 4× that
  - tossing never triggers the stall drain

### Rewards track

**12. Config, signals and flags**
- **Config:** `critters`, `critter_max`, `critter_starve_minutes`,
  `wheat_per_pour`, `kana_url` and `pixelorama_config`, following the
  AGENTS.md checklist.
- **Signals:** a `SIGUSR2` handler (or `SIG_IGN` when critters are off),
  and `--critter` to send it. With `critters = false`, `update_critters()`
  never runs and nothing spawns.
- **Done when:**
  - `--critter` drops one into the running copy
  - the config validates its ranges
  - with `critters = false`, `SIGUSR2` doesn't kill the process

**13. Pixelorama rewards** (`progress.c/.h`)
- **First check:** confirm by hand that saving a new drawing rewrites
  `config.ini` straight away. If it only writes on exit, use the inotify
  fallback from section 7 instead.
- **The thread:** started like the weather thread, with the state file
  (written to a temp file, then renamed) and the seen-paths set. The first
  run marks everything already in the config as seen.
- **Main loop:** spawn with the earned-minus-dropped count, the 2 s gap,
  and waiting for a free slot. `--progress` prints what the thread sees.
- **Done when:**
  - with a temp config: a new path pays one critter, a re-save pays
    nothing, and nothing pays twice across a restart

**14. Kana rewards**
- **Polling:** both databases via curl. Add a `json_str` helper next to
  weather.c's `json_num`.
- **Counting:** the fallback counting from `sessionMinutes`, then
  `lastSession` once the apps send it. A 15-minute set brings one small
  and one large critter.
- **Service down:** catch up when it comes back.
- **Done when:**
  - against a fake server serving canned JSON, the pay-outs follow the
    table (the 5-minute set about half the time over many runs)
  - early quits pay nothing once `lastSession` exists

### Then

**15. The kana apps, and balance** (both tracks needed)
- **The kana apps:** add `lastSession` in `../hiriagna-p1` and
  `../katakana-p1`, a commit in each repo. Check that their state
  validators and export/import accept it, and run their own test suites.
- **Balance:** the typical-day harness from the Balance section, 20 seeds
  × 7 days, for all five targets. Tune with the knobs in the order listed
  there, and record the final values and results in this file.
- **Done when:**
  - the five targets pass
  - `make run` with `c`, `s`, `l` and `rain_override` looks right
  - the README and AGENTS.md are updated

**16. Keyboard (optional)**
- A faint warm glow on keys over living critters, and a grey flash on a
  death.

## Testing
A throwaway headless harness, as AGENTS.md describes. Remember to advance
`s->clock` every step: wheat, moss and bodies age by it, and nothing grows
with it stuck at 0. Moss needs dirt, which only seeds make, so drop seeds
first.
- **Totals:** after every step, a full recount matches `grains`, `solids`,
  `water`, `wheat`, `bodies`.
- **Bodies match slots:** every `CRITTER` cell belongs to a live critter's
  rectangle, and every live critter's rectangle is fully `CRITTER` cells.
- **Settling:** with idle critters and no emitters, every chunk eventually
  sleeps.
- **Drains:** several fill/drain cycles in a row complete with critters
  alive. A critter in a finished iron hut is still in it after the drain;
  critters without one leave and come back each time.
- **Starving:** a critter with no food anywhere dies in about
  `critter_starve_minutes` of sim time and leaves 3 (or 2) `BODY` cells.
- **Burial:** pouring an emitter onto an unsheltered critter kills it, and
  a critter in a finished hut survives the same pour.
- **Farming:** moss, a puddle and one critter produce a field. Each pour
  adds `wheat_per_pour` levels, and each bite removes one, so the totals
  balance exactly: pours × 3 = levels on the field + bites eaten + levels
  rotted.
- **Supercharge:** a strike within `CHARGE_RADIUS` of a critter leaves
  its cells untouched and makes it 3× faster for `CHARGE_MINUTES`, then it
  goes back to its normal pace. A critter outside the radius is
  unaffected.
- **Kinds:** a 15-minute set reward brings one small and one large
  critter. Neither kind ever changes size.
- **Holding:** a farm holds the fill like a tree. With critters tossing at
  an edge, the hold lasts longer, but never past about 4× the plain hold,
  and the stall check never fires because of tossing.
- **Wells:** water poured into a well is still there after hours of dry
  weather, while a puddle beside it dries up within minutes. Water in an
  iron well survives a drain.
- **Balance targets:** the five targets in the Balance section, each as a
  harness scenario. The harness drives the typical day itself: it advances
  `s->clock`, calls `sim_add_critter()` twice at a random time each day,
  calls `sim_strike()` 4 times a day, and sets `water_rate` for stray drops
  and a daily shower.
- **Only pours make wheat:** keep a debug count of cells a pour converted.
  After every step, the `wheat` total must never have gone up by more than
  the pours converted that step. Run it twice:
  - no critters, hours of heavy rain over moss, with puddles sitting on it
    and water seeping through it: `wheat` stays at 0
  - critters living on the moss under heavy rain: every new wheat cell
    comes from a pour
- **Rewards:** with a fake kana server (a few lines of Python serving
  canned JSON) and a temp Pixelorama config, check the counts: 15, 10 and 5
  minute sets (the 5 over many runs should be about half), early quits
  paying nothing, a re-save paying nothing, a new drawing paying one, and
  nothing paying twice across a restart.
- **Cost:** with 24 critters, `update_critters()` stays under 0.1 ms a
  step at 480×270.

Then watch it with `make run`, using `c` to add critters, `s` for seeds,
`l` for lightning (it makes the iron they build with and supercharges
critters near it), and
`--set rain_override=…` for rain.

## Results (as built)

Steps 1-15 are built on the `critters` branch; step 16 (keyboard) isn't.
HUMAN_REVIEW.md lists where the build differs from this plan and why. The
main differences: the hut is a roof over a floor, open at both ends;
critters climb; they catch falling raindrops and plant seeds; and
streams don't pour right on top of them.

**Final values:** `wheat_per_pour` 3, `critter_starve_minutes` 40,
`WELL_MAX_ROWS` 5, `TOSS_SHARE` 0.75 (of grains actually poured),
`TOSS_SECS` 0.75, `LEDGE_PILE` 12, `DIG_IN_PLACE` 24, `SEED_IRON` 20, showers of `SHOWER_RATE` 30 drops/min over `SHOWER_W`
21 columns for `SHOWER_SECS` 120, `PATCH_RANGE` 12, `CLIFF_DROP` 4, `STARVING` 0.15, `DRAIN_WARN` 0.08,
`STACK_SECS` 1, sand bricks crumble after
`SAND_BRICK_SECS` 20 min,
newcomers join a patch within `JOIN_SPREAD` 4 columns and the second in
a colony is a sand-mover (`ROLE_SAND`), homes are a `PATCH_WELL_W` 3
well on the patch (`PATCH_WELL_W` is 5 now), `WELL_KEEP` 0.8 of draws leave
the water in the well, wells pull water within
`WELL_PULL` 6 and soak it through their roofs, idle critters press
sand into dirt (`IDLE_PRESS` 0.7 of the time, a grain per `PRESS_SECS` 30), critters hop up to
`JUMP` 3 cells and stair steps rise `STAIR_RISE` 3,
`CATCH_RANGE` 24 (capped by what it can reach in time), `SPRINT` 2,
`PLANT_RADIUS` 5, `PLANT_SECS` 20 min. Rain on hut and well roofs runs
into the well (gutters).

**Measured:** a farm alone holds the fill 24.0 min. Twelve charged large
tossers hold it 87 min, and it always ends by overfill, not the stall
check.

**Balance targets: not met.** In the typical day, critters almost never
eat. Raindrops sink into sand at once, moss rarely forms in clear weather,
and the 9-minute fill buries whatever does. Every critter starves at its
starve time, whatever the knobs. The numbers and options are in
HUMAN_REVIEW.md.

| Scenario | Runs alive at end | Max alive | Mean life (h) | Lived past arrival day | Deaths: starved/buried | Drains/day | Water time | Ate (food time) |
|---|---|---|---|---|---|---|---|---|
| typical | 0/8 | 2 | 0.9 | 0/48 | 48/0 | 50 | 0% | 0% |
| nocommit | 0/8 | 2 | 0.9 | 0/48 | 48/0 | 50 | 0% | 0% |
| noshower | 0/8 | 2 | 0.9 | 0/48 | 48/0 | 50 | 0% | 0% |
| showers | 0/8 | 2 | 0.9 | 0/48 | 48/0 | 49 | 0% | 0% |
| starve240 | 6/8 | 4 | 5.5 | 20/36 | 36/0 | 49 | 0% | 0% |
| wpp4showers | 0/8 | 2 | 0.9 | 0/48 | 48/0 | 49 | 0% | 0% |

`starve240` is the typical day with `critter_starve_minutes = 240`. Its
"alive at end" comes only from critters lasting 5.5 h without eating. In
every row the time spent eating or on water rounds to 0%. Drains stay at
about 50 a day because no farm ever forms to hold the fill.
