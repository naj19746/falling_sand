# Critters: notes for review

Everything in CRITTERS.md is built and committed on the `critters` branch,
except step 16 (keyboard, optional), which I skipped. Each step has its own
commit, except step 15, which is spread over the balance commit, docs, and
the two kana-app branches. The build has no `-Wall -Wextra` warnings. The
headless checks for every step pass (list at the end).

**Main problem: the balance targets are not met.** In the realistic
headless sandbox (480x270, two emitters, ledges, real drains), critters
almost never eat. Nearly every critter starves at its starve time, in
every scenario I tried. The cause isn't a tunable number: the sandbox
doesn't supply the water and ground the plan's economy assumes. Details
and options are in "Balance" below. You need to make a design call there
before this is fun to watch.

## Status by step

| Step | State |
|---|---|
| 1 groundwork | done; byte-identical grid before/after |
| 2 cell types, BUILT | done |
| 3 bodies, movement, `c` key | done (`c` key from the rewards track) |
| 4 supercharge | done; measured 3.00x speed, extends without stacking |
| 5 food, starving, bodies | done; large dies at 39.9 min, small at 66.6 min, 3/2 bodies, rot to dirt |
| 6 carrying, digging, burial | done |
| 7 wheat | done; poured levels = field + bites + rotted, exactly; rain never makes wheat |
| 8 homes, wells | done (deviations below) |
| 9 huts, iron | done (hut shape changed, see below) |
| 10 drain | done |
| 11 hold, tossing | done; farm alone holds 24.0 min, 12 charged tossers 84 min, always ends by overfill |
| 12-14 config, signals, rewards | done by a subagent in a worktree, merged |
| 15 kana apps | `lastSession` added in both apps on a `sandbg-last-session` branch (not merged, not pushed) |
| 15 balance | harness built and run; **targets not met** (see Balance) |
| 16 keyboard | not done (optional) |

## Things to check first

1. **Don't run `sandbg --critter` against your current desktop copy.**
   It sends SIGUSR2 to every process named `sandbg`. The installed copy is
   the old build with no SIGUSR2 handler, so it would be killed. Install
   the new build first (`make install` or the setup script).
2. **The kana apps:** `../hiriagna-p1` (commit `6f6a25f`) and
   `../katakana-p1` (commit `111c6bf`), each on a branch
   `sandbg-last-session` made from `main`. Typecheck and vitest pass in both
   (375->388 and 423->436 tests). `lastSession` is
   `{ endedAt: <epoch ms>, minutes, activeMs, endReason }`, and it's only
   written when at least one card was answered, the same rule as the
   `sessionsCompleted` increment. It needs merging and deploying before
   early quits stop paying. Until then progress.c falls back to counting
   every increment at `settings.sessionMinutes`. kana-platform needed no
   change: its server only checks that `state` is an object.
3. **Pixelorama:** the subagent couldn't save a drawing by hand. From file
   times on this machine (`Saw.pxo` saved 4 ms before `config.ini`, while
   Pixelorama was running), the config is written on save. But
   `recent_projects` isn't updated on save, so progress.c relies on
   `last_project_path` plus a scan of that project's folder for new
   `.pxo` files written since the last config write. Please save one new
   drawing and check `sandbg --progress`.
4. **Catch-up rule for kana while the service was down:** I followed the
   plan, so if the count went up by n, the latest set pays by its own end
   reason and the other n-1 pay at its length whatever their end reason
   was. Three early quits while sandbg was off would pay for two.
   Possibly too generous.
5. **A `git pull`** that brings new `.pxo` files into Pixelorama's current
   folder pays for them on the next config write.
6. During testing, one `--windowed` run drove your real Keychron LEDs for
   about 100 s (`keyboard = auto`). After that I ran with
   `--set keyboard=off`. Nothing persistent changed.
7. The rewards worktree is still at `.claude/worktrees/agent-...`, with an
   untracked `REWARDS_NOTES.md` (its notes, summarized above). Its branch
   `worktree-agent-ac91d54003b707c83` is merged and can be deleted.

## Where I changed the plan, and why

- **Hut shape.** The plan's hut has a wall and a 2-cell door. Large
  critters are 3 cells tall, and critters walk straight at what they want
  (no pathfinding), so with a wall they kept getting stuck inside or
  behind it. The hut is now a 5-cell iron/stone roof over a 5-cell floor,
  open at both ends, 3 cells high inside: 10 cells instead of 12. The hut
  sits 3 columns from the well, because with a 1-column gap a large
  critter got wedged under the roof's edge.
- **Climbing.** Critters climb stone, iron, glass, earth banks
  (dirt/moss/wheat) and each other, a cell per move, clinging to the wall.
  The plan only had one-cell steps. Without this, a 3-cell rock or a
  well wall was a permanent barrier, and two critters nose to nose on a
  roof deadlocked.
- **Well access.** The well's roof row, and the row under it during
  deepening, is walkable even where a cell is missing. Small critters kept
  falling through the mouth or a gap and getting sealed in. Roof cells
  aren't laid while anyone is inside, and a critter that does end up
  inside climbs out first.
- **Deepening** raises the roof: a new roof is built on the old one, then
  the old roof is taken out through the mouth.
- **Swapping stone for iron**: only cells a critter can get at are
  swapped. The well's inner floor is swapped "through the mouth", like
  drawing water.
- **Wheat rot** is per cell, refreshed by any pour touching it, not per
  home. `since[]` holds sim seconds for wheat (set_cell handles it).
- **Digging**: a blocking grain is thrown over the shoulder, not carried
  past HOME_RADIUS. Under sand, a critter wriggles up a row at a time
  (swapping places with the sand) and keeps working unless it's 2+ deep.
- **Dropped material**: stone a critter drops becomes loose debris, so it
  doesn't hang in the air. Iron is only dropped onto something solid.
- **Detours**: after 3 failed goals in a row, a critter walks 10-20 cells
  the other way, to get round walls greedy walking can't.
- **Toss cap** uses grains the emitters actually got in, not grains
  tried. With "tried", blocked attempts let tossing stretch the hold to
  161 min (more than 6x).
- **Balance-pass additions** (see below): catching falling raindrops,
  planting a seed, looking across the whole screen for food when there's
  none in sight, and streams that don't pour right on top of a critter.
  Because of that last one, the plan's check "an emitter pouring on an
  unsheltered critter kills it in about BURY_STEPS" no longer holds.
  Critters still die under deep piles.
- **Colors**: pale head, violet body. My first browns disappeared into
  the desert palette.

## Balance

The harness (`bal.c`, see "Harness" below) runs days of 8 visible hours on
the real sim. Each day has one 15-minute set (one small and one large
critter at a random time), 4 commits (strikes from a random blaster
spot), 30 Spotify seeds, 3 stray drops a minute, and a 20-minute shower
at 400 drops/min with 40% chance (scenarios: `nocommit`, `noshower`,
`showers` = a shower every day).

### Results (8 seeds x 3 days per row)

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

The targets: 1 (alive at the end of the week in half the runs) fails, 2
(no commits: colonies don't outlast their day) passes trivially, 3 (the
day-1 colony lasts 2 days) fails, 4 (5-8 with daily showers) fails, and 5
(a third to a half of the time on water) fails: almost all their time
goes to digging out and wandering.

### Why

These come from traces and snapshots (`bal ... snapdir` writes PPMs):

1. **The fill buries everything.** With no hold, the screen fills in
   about 9 minutes and drains: 49 drains in an 8-hour day. A critter rides
   the rising pile, digging itself out. Any moss, planted dirt or field on
   the surface is covered by sliding sand within seconds to minutes. The
   hold only starts once the screen is full, and only with wheat, which
   needs the farming that can't happen first.
2. **There's no water.** The plan's clear-weather income ("3 stray drops
   a minute, critters catch 3-4 an hour") assumes drops sit where they
   land. On sand a drop seeps in within about a third of a second, so
   there's nothing to fetch. I added catching drops as they fall (a
   critter runs under a drop within 8 columns). That's at most about one
   drop every 9 minutes per critter, and only while it has somewhere to
   pour.
3. **Moss needs two drops on the same seed patch within 10 minutes.** In
   clear weather that basically never happens before the patch is buried.
   Under a shower, moss forms fast (hundreds of cells), but mostly far
   from the critters or under the pile, and the next drain wipes it.
4. Everything not joined to iron is wiped every cycle, so a colony
   restarts from nothing every ~10 minutes, with 40 minutes of food.

I tried the plan's knobs and some of my own. None moved the result: every
row above is still "starves at its starve time":
- `critter_starve_minutes` 40/120/240: critters just live 0.9/2.8/5.5 h,
  eating nothing.
- `wheat_per_pour` 4: no pours happen, so no effect.
- A 25-column stream gap around critters: no change.
- Emitters at 1/4 and 1/10 of their rate whenever any critter is alive
  (an experiment outside the repo): still no food.
- Catching every reachable drop whenever a critter's hands are empty: no
  food.

### Options (you need to choose)

These are design calls, so I didn't make them:

- **A. Give critters ground that survives.** For example, a new critter
  arrives with a small patch of moss on a little iron "raft" or ledge, so
  its first field survives drains and the hold logic can kick in. That's
  closest to the plan's "iron decides who survives" story.
- **B. Make water reachable.** Raindrops landing on built roofs and
  ledges already pool (stone doesn't swallow them). Let rain landing on a
  hut or well roof run into the well (gutters). Or let critters collect
  water from wet sand or wet dirt (seeping water is still a WATER cell
  just under the surface).
- **C. Hold the fill for any living colony,** not only for a farm, e.g.
  emitters trickle while a home has residents. This changes how the
  wallpaper looks most of the time once critters are around.
- **D. Lower the stakes.** Treat critters as short-lived visitors: about an
  hour of activity per finished set, with no expectation of colonies. The
  current code already behaves like this.

My guess is A+B gives the plan's intended feel with the least change to
how the sandbox looks.

### Follow-up: gutters, wide wells, sprinting

Asked for after the first review: option B's gutters, wells as wide as
the ground allows, and a sprint when chasing drops.

- **Gutters:** rain landing on a built hut or well roof goes straight into
  the lowest open cell of that well (`gutter()` in critter.c, called from
  sim.c where a drop lands). It moves at once; nothing animates along the
  roof. Check whether that looks right to you.
- **Wide wells:** a new home takes the widest well (21, 19, ... 7 with
  walls) whose footprint is level. A wider well costs more stone, so
  critters now look anywhere on screen for building stone when none is in
  sight. They also dig loose earth away from outside an unfinished well
  wall: a 13-wide well in t9 had stalled against a dirt bank.
- **Sprint:** chasing a drop, a critter moves at twice its pace. It only
  goes after a drop it can get under before the drop reaches its head,
  and it checks every move, not every second.

Measured in isolation (t12/t13 in the harness):

| Well width (walls included) | Rain into the well at 3 drops/min |
|---|---|
| before gutters (mouth only) | ~0.4/h |
| 7 | 3.2/h |
| 11 | 5.2/h |
| 15 | 6.0/h |
| 21 | 9.2/h |

- **Catching:** a small critter now catches a drop up to 12 columns out
  from a standing start (it was about 8), and misses at 16 or more.
- **Build time:** a 21-wide home on flat ground with stone nearby is
  fully built in 65 minutes.

**Realistic matrix (same 8 seeds x 3 days): no change.** typical and
showers: 0/8 alive, mean life 0.9 h. starve240: 6/8, 5.3-6.2 h. Nobody
eats in any run. No critter ever plans a home, so `guttered` is 0 in
every run. Each run catches 0-14 drops in 3 days, because a critter only
wants water when it has earth to pour on or a well. A home site needs
moss within 20 cells, and the 9-minute fill buries moss first. So these
changes only pay off once something like option A gives critters
ground that lasts: a starting patch, or relaxing the site's food rule.

### Follow-up: clearing ledges, sand handled 4x faster

Asked for next: critters clear the sand off the nearest stone ledge, and
move sand four times as fast.

- `TOSS_SECS` 3 s -> 0.75 s. This also speeds up tossing off the screen
  edge: 12 charged large tossers now hold the fill 87 min (was 84), still
  ending by overfill.
- A critter with no home scoops sand lying on natural (unbuilt) stone,
  at most 12 deep, and flings it past the nearer end of that ledge. The
  sand reappears there at once as falling grains; there's no arc drawn.
- Alone on a bare test ledge with 843 grains on it, a small critter
  clears it in about 23 min and a large one in about 17 min. That's about
  35 and 56 grains a minute: walking to each scoop and the once-a-second
  think limit it, not the throw time. Rain then pools on the bare stone:
  93-105 cells from 600 drops in 10 min.
- Before building, I measured ledges with no critters: within 1 minute
  of a fill starting, 89-98% of ledge columns are under sand, and from
  about 2 minutes ~90% are more than 3 deep. The sand streams (about 140
  grains a second together) sweep across the ledges.

**Realistic matrix (same 8 seeds x 3 days): no change.** Critters now
spend about 30% of their time clearing ledges and fling 2,400-7,000
grains a run (22,000-35,000 with starve240). typical and showers: 0/8
alive, mean life 0.9-1.0 h. starve240: 6/8, 5.3-6.6 h. Across all 24
runs, 3 bites were eaten (one showers run) and no home was ever planned.
Ledges refill as fast as they're cleared and crumble at every drain, so
nothing stays put long enough to farm.

`git revert` of this commit takes both changes back out if you don't
want them.

## Harness and checks

The throwaway harness is in the session scratchpad,
`/tmp/claude-1000/-home-nick-projects-falling-sand/bbc4c7f7-2fe6-45a2-a94d-90c9248aa406/scratchpad/h/`
(it may not survive; it's outside the repo, as AGENTS.md asks).
`check.h` has the running-total recount, the critter-rectangle invariant
and helpers. The `t2`...`t11` checks are per step, and `bal.c` is the
balance harness (`bal <typical|nocommit|noshower|showers> <seed> <days>
[snapdir]`, with env `STARVE`, `WPP`, `SEEDS`, `STRAY`, `TRACE`). All of
t2-t11 pass on the final code. They cover:
- totals vs a full recount, and the critter cells vs live rectangles
- sand piles on critters, water flows around them and the total holds,
  idle critters let chunks sleep
- strikes on top of critters, 3x speed, 10-minute charge, extending
- starving on schedule, body counts, rot to dirt, living on moss in rain
- digging out of thin sand, burial under 75 rows, digging through a dune,
  dropping what's carried on death
- the wheat bookkeeping and "only pours make wheat", with and without
  critters, under heavy rain
- well water vs a puddle in a dry spell (10 -> 10 over 3 h, while a
  20-cell puddle dried in 1 min), rain through the mouth (8 of 10 in 20
  min of heavy rain), and a pair farming from a full well through 3 dry
  hours (well 25 -> 18, 7 pours, both alive)
- a colony building a full hut and well from stone (19-57 min), turning
  it 26 of 31 to iron once iron is around, and small critters never
  mining iron
- an iron home keeping its hut, field and well water through a drain,
  homeless critters leaving and coming back with their food, stone homes
  being forgotten, and 12 real fill/drain cycles in 2 h with critters
- the hold times above

Cost: the critter code is cheap next to the sand. A 3-day run is about 6
minutes of CPU, the same as with no critters. I didn't separately measure
the plan's "24 critters < 0.1 ms per step" at 480x270.

## Smaller notes

- `sim_paint(WATER)` gives water random color bits, including
  `SEEPING`, so painted water sitting on stone vanishes. This was there
  before; only the harness hit it.
- Without rain within 25 cells, moss dries in 30 minutes. So in a fully
  dry spell, critters with no wheat starve within about an hour even if
  they're standing in moss.
- I couldn't drive the live window with xdotool (keys didn't reach SDL),
  so the visual checks were harness snapshots plus one early screenshot.
  Please look at it with `make run` and `c`/`s`/`l`, especially the hut,
  the well, critter colors, the carried-cell and toss overlays, and the
  spark flicker.
