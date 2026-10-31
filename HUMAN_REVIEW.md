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

### Follow-up: cheaper homes, brick, dropping in over stone

Asked for next: do homes take too much stone? Try something minimal at
first, dirt as brick, and new critters dropping in over stone or iron.

**Stone wasn't what stopped them.** I instrumented site choice over four
realistic 8-hour days. A homeless critter had a level, unblocked 15-wide
spot within reach about a third of the time. There was moss within 20 of
it 0% of the time, and any moss anywhere on screen under 1% of the time.
A site needed food nearby, so no home was ever planned.

Changes:
- **Food is a bonus at a site** (`FOOD_BONUS` 40 cells of distance), not
  a requirement.
- **The well comes first;** the hut only joins the plan once the well is
  built.
- **Brick:** bare dry dirt open to the sky (never dirt under moss) is
  building material, laid as built stone. Stone and debris are still
  preferred.
- **Dropping in, in this order:** over stone or iron with earth within
  20 columns, then over earth, then over stone or iron.
- Two old navigation traps the new drop spots exposed:
  - A hungry critter eats anything in reach before going after better
    food it can't reach. A small one sunk in a field had starved going
    back and forth between two wheat patches with moss beside it.
  - Digging in place while walking now counts as stuck after
    `DIG_IN_PLACE` digs. One critter had dug forever at sand sliding back
    into its pit.

**Realistic matrix (same 8 seeds x 3 days): homes are now planned in
every run, but almost nothing gets built and survival doesn't change.**
Critters spend 15-22% of their time building and lay 0-12 cells a day
(12-26 with starve240). typical and showers: 0/8 alive, mean life
0.9-1.0 h. starve240: 6/8, 5.1-6.6 h. No food eaten.

A trace shows why. Homes get planned on whatever is level when a critter
lands: a ledge, or the empty floor just after a drain. The pile then
rises about 15 rows a minute. The builders ride it up, and the plan cells
fill with sand faster than they can be cleared. Whatever gets built
crumbles at the drain about every 9 minutes. The fill cycle is the
limit, not stone.

**Known regression:** t9 (a colony building a home from stone on a
field strewn with rocks) now fails. The two critters drop in on
different rocks, plan two separate homes, and the large one's 21-wide
well on loose sand gets 10 of 55 cells in 4 hours. It passed before this
change.

### Follow-up: seeds make iron

Asked for next: a critter turns stone to iron with a seed.

- A critter sprints under a falling seed the way it does for a raindrop,
  ahead of anything but eating. It only goes for one (seed or drop) that
  falls clear all the way down to it. Before this change it would also
  chase drops about to land on a ledge over its head.
- It plants the seed on the nearest stone ledge: up to 9 cells along the
  ledge's top turn to built iron, and the sand on them (or the open cell
  above) turns to dirt. With no ledge anywhere, it lets the seed go to
  sprout where it lands.
- A home site with iron under it counts 40 cells nearer.
- Seeded iron is `BUILT` and outside any plan, so nobody mines it. It
  rusts like other iron: I didn't add an exemption.
- t15 checks it end to end: the seed is caught, 9 ledge cells turn to
  iron with dirt on 7, and after a drain all 9 iron cells and 6 of the
  dirt are still there.

**Realistic matrix (same 8 seeds x 3 days): no change.** Seeds fall at
random, about one every 16 minutes, and a critter can reach a few percent
of the screen's columns. typical and showers: no seed was caught in any
run. starve240: 0-2 caught per run, 0-2 planted. Survival is the same as
before: 0/8, 0/8, 6/8.

**Experiment, not committed:** every new critter arrives carrying a
seed. Every critter then plants one (6-7 per run), but:
- seeded iron is mostly gone by evening (0-18 cells left). With ~49
  fills a day, rust takes a given piece in about 10 fills (~90 min).
- dirt on the iron rarely turns to moss (0-5 cells at evening). It needs
  two pours on the same spot within 10 minutes.
- critters spend 1-11% of their time trying to eat, and no bites in any
  run. Survival: typical 0/8 (0.9 h), starve240 6/8 (5.3-6.0 h).

The next levers would be exempting seeded iron from rust and moss from a
single pour, or arriving with a seed. Those are design calls.

### Follow-up: arrive with a seed, rust-proof seeded iron, planting shower

Asked for next: merge "a seed brings a shower" into seeds making iron.
Now:
- Every new critter arrives carrying a seed.
- Seeded iron never rusts (a `SEEDED` color bit on iron that
  `rust_iron()` skips).
- Planting starts a 2-minute shower, 30 drops/min over the 21 columns
  around the patch.

t16: a critter plants its seed within a minute. Within 3 minutes the
shower turns all 7 dirt cells on the 9 iron cells to moss. After 40
drains, the iron and the moss are all still there.

**Realistic matrix (same 8 seeds x 3 days): the first real movement, but
still no survival.**
- Every newcomer plants (4-8 per run). Iron builds up through the
  days: about 18, 36, 54 cells by each evening.
- There's moss on screen most evenings (0-27 cells), and wheat forms in
  some runs (up to 31 cells).
- Farms now hold the fill sometimes: up to 0.8 h of hold over 3 days in
  typical, and up to 6.5 h with starve240. Before, there was never any.
- Critters spend about 20% of their time going for food, up from 0, but
  get 0-2 bites a run.

| Scenario | Runs alive at end | Mean life (h) |
|---|---|---|
| typical | 1/8 | 0.8-1.1 |
| showers | 1/8 | 0.8-1.3 |
| starve240 | 8/8 | 4.0-10.1 |

**Why they still don't eat** (traced over 4 realistic days): the food
is there but out of reach. When a critter is going for food, the target
is on average about 150 cells away. 80-90% of the time it's above the
critter's head: seeded patches are on floating ledges. Critters land on
the floor or the pile, and can only get up to a ledge once the pile
reaches it, by which time the patch is buried. Roughly half the time a
hungry critter isn't going for food at all: everything it knows of is
avoided or unreachable.

Next levers: after a drain, drop a critter back in over its own patch
instead of anywhere on its side; plant on the ledge it's standing on, or
low ledges first; or let critters climb up to a ledge some other way.

### Follow-up: staying near the patch, brick stairs

Asked for next: critters stay closer to their ledges, and stack sand to
get back up when they can't walk there.

- **Staying near:** a critter remembers the patch it planted. It strolls
  around it, and heads back whenever it's more than 12 columns from it
  (and from its home) or below it. Back on its patch, it flings any sand
  piled on it over the edge. After a drain it drops back in over its
  patch.
- **Stairs:** below a floating ledge, it builds stairs of brick from the
  nearer end of the ledge down and out to the ground: step k is k cells
  out and k down, floating like the ledge. It fetches a grain of sand for
  each step and lays it from the step below, bottom up, then walks up.
  The steps are built stone, so they crumble at the drain.
  - I tried a straight pillar first. A critter that ended up on the
    ledge side of it was trapped under the ledge, and it kept digging a
    pit at the pillar's foot. The diagonal stairs avoid both.
- **Climbing:** critters can now reach one cell above their heads for a
  hold when climbing.
- **Patch dirt:** they no longer dig through dirt sitting on iron while
  walking. They had been kicking their own patch's dirt away.
- t17: a small and a large critter start on the ground, 31 rows below a
  floating ledge with their patch on it. Each builds 35 steps and is on
  its patch in 12-13 minutes, then stays within range for the next 10
  minutes.

**Realistic matrix (same 8 seeds x 3 days, plus nocommit): the best so
far, still short of the targets.**

| Scenario | Runs alive at end | Mean life (avg) | Bites per run | Fill held over 3 days |
|---|---|---|---|---|
| typical | 3/8 | 1.3-2.8 h (1.7) | 0-15 | 0-3.5 h |
| showers | 1/8 | 1.3-2.2 h (1.6) | 0-8 | 0-3.5 h |
| nocommit | 2/8 | 1.2-2.8 h (2.1) | 0-13 | 0-3.5 h |
| starve240 | 7/8 | 4.0-9.8 h (7.3) | 0-21 | 0.8-9.5 h |

The last round was 0.8-1.3 h mean life with 0-2 bites. Some critters now
live past the day they arrived, and patches turn to wheat that farms
hold the fill with. Critters spend about 45% of their time building
(mostly stairs).

### Follow-up: sand bricks crumble

Asked for: sand bricks crumble back into sand over 20 minutes, while dirt
bricks last until they're drained.

- A step pressed from sand gets a `SAND_BRICK` bit. 20 minutes of sim
  time after it's laid, it turns back into a grain of sand. Brick from
  dirt (and from debris or stone) has no bit, and lasts until the drain
  crumbles it like other built stone.
- The aging sweep only ran while there was damp earth or a body
  somewhere. `set_cell()` now keeps a running count of sand bricks so it
  runs for them too; the harness recount checks that count.
- A fix along the way: lightning (and painting) could make iron whose
  random color had the `SEEDED` bit, so it would never rust. Iron from
  them now keeps that bit clear, and stone laid in a home never gets
  `SAND_BRICK`.
- t18: a sand brick and a dirt brick both stand at 19 minutes. At 21
  minutes the sand bricks are 10 grains of sand and the dirt bricks
  still stand. A critter's 35-step stairs are all sand again 21 minutes
  after it reaches its patch.

The matrix is about the same as before (8 seeds x 3 days): typical 3/8
runs alive at the end, mean life 1.6 h; showers 0/8, 1.7 h; nocommit
2/8, 2.3 h; starve240 8/8, 7.8 h.

### Follow-up: homes on the patch

Asked for: build homes on the patch, small.

**What a home is now:** a well 3 cells wide (walls included) standing on
the patch's iron, which is its floor. That's 6 cells to build: two walls
two high, and a 2-cell roof with the mouth between. It goes toward one
end of the patch, leaving the middle for the field.
- No hut. A critter standing on its patch already stays through the
  drain, so a hut added nothing.
- The old site search is gone (level ground, food, iron and edge
  bonuses, wide wells). The hut code is still there, because the older
  tests build huts by hand; it could be deleted.
- A critter with a patch joins a home on it or plans one there. A
  critter without one joins the nearest home. Homes nobody lives in are
  forgotten at the next drain.
- `mark_kept()` keeps built stone joined to iron through the drain, so
  the walls last and well water on the iron floor is kept.

**Fixes the test turned up:**
- A carried seed has its own slot. Building stairs needs the critter's
  hands, and it had been dropping the seed, or laying it as a brick.
- Stairs work for planting too: a critter can climb to a floating ledge
  to plant its first seed, not just to return to a patch.
- With a patch, junk cleared off the home is flung over the end of the
  ledge, instead of carried to a dump spot past the edge, which walked
  critters off their ledge.
- With a patch, critters only pour on their own patch. They had walked
  off to water wheat that grew below, from dirt they'd thrown off.
- They don't mine stone within 2 cells of seeded iron (the ledge holding
  up their patch).
- "Go back to the patch" comes before using what they carry. Holding
  stone, a critter used to skip it and stroll on the ground forever.
- The stair scan treats the critter's own body as open, and it never
  lays a step in a column it stands in. It had laid one over its own
  head.
- Stair progress means getting nearer the target or laying a step, with
  about a minute's patience. Shuffling sideways used to count.
- A critter standing on the next cell to build steps aside first.

t19: a newcomer lands on the ground, climbs stairs to a floating ledge,
plants its seed and plans a well there. In 90 minutes it builds 6 of the
9 plan cells (the floor counts), then keeps falling into its own
half-built well on the way to stone on the far side. **It still fails.**
The well, its water and the critter all come through a drain.

**Realistic matrix (8 seeds x 3 days):** about the same as before.

| Scenario | Runs alive at end | Mean life (avg) | Fill held over 3 days | Home cells laid per run |
|---|---|---|---|---|
| typical | 3/8 | 1.4-3.1 h (2.0) | 0-7.3 h | 0-7 |
| nocommit | 2/8 | 1.4-1.8 h (1.6) | 0-3.4 h | 0-7 |
| starve240 | 8/8 | 4.0-9.3 h (6.6) | 3.7-14.5 h | 4-15 |

No well held water in any run, and nothing was guttered. Traced over 3
realistic days, critters with a home spend:
- 26-46% of their time getting back to their patch (they fall off while
  fetching things)
- 35-46% of the time with no stone, debris or bare dirt in sight: the
  ledge around the patch is off-limits and the rest is buried
- under 1% building

**Next step (your call):** let home walls be built from the sand
critters already clear off their patch, pressed into lasting brick.
Material would then always be at hand, and fewer trips would take them
off their ledge.

### Follow-up: hopping

Asked for: critters jump up to 3 pixels.

- A critter that can't walk or step up hops instead: straight up 2-3
  cells through open air, then across onto something solid.
- Stair steps now rise 3 per column, a hop apart, so stairs take about a
  third of the bricks. Going up stairs, a critter tries the highest hop
  first; otherwise it walked off a step into the space under the next.
- t17: from the ground to a patch 31 rows up in 3 minutes with 11 steps,
  for both sizes (was 12-13 minutes and 35 steps).

**In realistic days (3 seeds, traced):** critters with a patch are on it
36-43% of the time (was 19-37%) and below it 45-51% (was 52-70%). They
start back for it about 1,000-2,200 times a day (was 2,300-2,700). They
lay more stair steps in total (about 1,200 a day, was 200-360), because
more of their climbs succeed and each crumbles after 20 minutes.

**Matrix (8 seeds x 3 days):** survival is about the same, within the
noise.

| Scenario | Runs alive at end | Mean life (avg) | Fill held over 3 days |
|---|---|---|---|
| typical | 4/8 | 0.9-2.3 h (1.6) | 0.4-5.1 h |
| nocommit | 2/8 | 1.1-2.5 h (1.7) | 0.4-4.9 h |
| showers | 3/8 | 1.2-1.7 h (1.4) | 0.8-3.7 h |
| starve240 | 7/8 | 3.4-7.8 h (5.3) | 2.7-15.6 h |

Still no well has held water, and home cells laid stay at 0-17 a run.
t19 still stops at 7 of 9 cells.

### Follow-up: pressing sand into dirt

Asked for: critters slowly turn sand into dirt and build with it, as an
idle activity: 70% pressing, 30% strolling.

- With nothing else to do on its patch, a critter presses a grain of sand
  in reach into dirt (`IDLE_PRESS` 0.7), one every 30 seconds of sim time
  (`PRESS_SECS`). Otherwise it strolls as before. It skips sand in a
  home's plan.
- Bare dirt counts as building material as good as fallen debris. Dirt
  right next to a home is fine to use now; dirt in a plan cell, under
  moss or on iron isn't.
- **Critters no longer mine the stone of ledges at all.** In t19 the
  critter kept fetching stone from the ledge beside its well, holed the
  ledge, fell through and never got back. Building material is now
  fallen debris and dirt; large critters still mine iron. (This removed
  the "near a patch" mining rule, which isn't needed any more.)
- **t19 passes now.** The newcomer plants, plans a well on its patch, and
  finishes it in 25 minutes, deepening it to 5 rows. In that test world
  it built with dirt from the ground nearby, not pressed dirt. After a
  drain the well (15 cells), its 5 cells of water and the critter are
  all still there.

**All step tests pass.**

**Matrix (8 seeds x 3 days):** pressing happens (4-238 grains a run,
89-585 with starve240), but homes still barely get built, and survival
is about the same.

| Scenario | Runs alive at end | Mean life (avg) | Home cells laid | Grains pressed |
|---|---|---|---|---|
| typical | 2/8 | 1.1-2.0 h (1.6) | 0-6 | 4-194 |
| nocommit | 1/8 | 1.2-3.3 h (2.2) | 1-6 | 12-238 |
| showers | 2/8 | 1.5-3.1 h (1.9) | 0-6 | 6-136 |
| starve240 | 8/8 | 3.2-10.8 h (6.0) | 5-24 | 89-585 |

No well held water by any evening. One starve240 run guttered 11 drops.

### Follow-up: why pressed dirt doesn't become walls; more critters

**Trace (4 realistic days):**
- Each critter presses 180-380 grains a day, but about 90% of the dirt
  around a patch is under sand, so it isn't usable (brick material has
  to be open to the sky). Material is in sight only 16-27% of the time.
- Critters start 107-288 build trips a day, and about 1 in 10 lays a
  cell. In the failed ones the home is under 50-60 rows of the fill.
  Its plan cells are still empty pockets in the pile, so they look
  buildable, while the critter rides the surface far above. Or the
  material is 50-90 rows below, or the critter has fallen 115 rows below
  its home.
- Time with a patch home: 32-45% getting back to the patch, 7-13%
  clearing it, 8-17% pressing, 1-2% building.

**Changes:**
- A pressed grain goes straight into the critter's hands as a brick if
  its home still needs one, instead of being left to be buried.
- A critter only starts building when it's within 8 rows of its home's
  floor (`HOME_LEVEL`). Otherwise the plan is buried, or out of reach.

So building only happens in the few minutes after a drain, before the
pile covers the patch again. The walls on iron are kept across drains,
so a home grows a few cells per cycle at best.

**More critters (typical day, 8 seeds x 3 days):** `BATCH` is how many
arrive together each day (2 now); `KEEP` replaces critters so that many
are always alive.

| Setup | Arrivals | Runs alive at end | Mean life | Bites per arrival | Fill held | Home cells laid | Homes finished |
|---|---|---|---|---|---|---|---|
| 2 a day (now) | 6 | 2/8 | 1.7 h | 0.8 | 2.0 h | 3.5 | 0 |
| 3 a day | 9 | 2/8 | 1.9 h | 1.1 | 2.4 h | 4.2 | 0 |
| 2 kept alive | 29 | 8/8 | 1.8 h | 1.2 | 10.4 h | 10.4 | 0 |
| 3 kept alive | 38 | 8/8 | 2.0 h | 1.8 | 14.8 h | 14.2 | 1 run |

More critters hold the fill much longer (more farms and tossers), and
each eats a little more, but each still lives about 2 hours. Every
critter plants its own patch and plans its own home there, so their
building is split over separate homes. Letting newcomers join an
existing patch and home, instead of planting their own, is probably the
next lever.

All step tests pass.

### Follow-up: colonies and a sand-mover

Asked for: new critters spawn near the others and stay nearby, and the
second critter specializes in moving sand.

- A new critter drops in within 4 columns of an existing critter's
  patch, if there is one, and adopts that patch and its home.
- The colony's second critter is a sand-mover (`ROLE_SAND`). Besides
  eating and getting back to the patch, it only clears sand off the
  patch and out of the home's plan. It doesn't plant (its seed is
  dropped), farm, build or press. A third critter does everything, like
  the first.
- Two critters that arrive together don't form a colony: the second
  lands before the first has planted a patch to join. Later arrivals
  (and replacements in the `KEEP` runs) do join.

**Matrix (typical day, 8 seeds x 3 days):**

| Setup | Arrivals | Runs alive at end | Mean life | Bites per arrival | Fill held | Sand flung per run |
|---|---|---|---|---|---|---|
| 2 a day | 6 | 2/8 | 1.7 h | 0.7 | 1.6 h | 4,800 |
| 3 a day | 9 | 1/8 | 1.7 h | 0.8 | 2.0 h | 5,400 |
| 2 kept alive | 31 | 8/8 | 1.7 h | 1.1 | 9.8 h | 19,100 |
| 3 kept alive | 40 | 8/8 | 1.9 h | 1.9 | 12.6 h | 29,900 |

That's about the same as without colonies (previous section). Mean life
stays at about 1.7-1.9 hours.

**Why** (traced, 2 or 3 kept alive, 4 seeds):
- Colonies form: a critter shares its patch 53-82% of the time.
- Sand-movers are on their patch 32-55% of the time. They spend 31-48%
  getting back to it and 5-24% flinging sand off it.
- **Food is the cap.** A patch has 1.5-5.8 cells of moss or wheat around
  it, and only 0.4-1.4 of them uncovered at a time. A critter needs about
  3 bites per 40-minute stomach, and the whole colony gets 6-20 bites in
  an 8-hour day. Nearly every death is starvation (37 of 41).

Sharing a patch splits the same small harvest; specializing frees no
food. More food per patch would be the lever: a bigger patch per seed,
newcomers planting their seed next to the colony's patch to widen it
(right now the sand-mover's seed is dropped), or more wheat per pour.

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
