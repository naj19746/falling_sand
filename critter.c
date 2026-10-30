#include "sim_internal.h"

#include <stdlib.h>

/* Critters: small creatures that live in the sandbox. Each has a body in
 * the grid made of CRITTER cells, so sand piles on it and water flows
 * around it, and a slot in s->crit[] with what it's doing. They decide
 * what to do about once a second by looking around them, then walk
 * greedily toward it: no pathfinding. All their timings count sim steps,
 * so nothing happens to them while the desktop is covered. */

/* Timings in seconds of sim time, turned into steps by secs(). */
#define MOVE_SECS   0.5f    /* about 2 cells a second */
#define FALL_STEPS  3       /* falling, at sand's pace */
#define THINK_SECS  1.0f

/* Idle wandering: how far, and how long to stand between strolls. */
#define WANDER      8
#define IDLE_SECS   6.0f

/* Lightning: critters this close to a strike, or touching the bolt, are
 * supercharged for CHARGE_MINUTES and do everything CHARGE_SPEEDUP times
 * as fast. */
#define CHARGE_RADIUS  20
#define CHARGE_MINUTES 10
#define CHARGE_SPEEDUP 3

/* Moves without getting closer before a critter gives up on its goal,
 * and how long it then leaves that spot alone. */
#define STUCK_MOVES 6
#define AVOID_SECS  30.0f
#define FAILS_BEFORE_DETOUR 3
#define DETOUR      10

/* How far around itself a critter looks for things. */
#define SENSE       40

/* Food: hungry below EAT_BELOW, eats until above FULL_ABOVE. A bite takes
 * EAT_SECS; moss is MOSS_MEAL of a full stomach. A small critter burns
 * SMALL_BURN as fast as a large one. */
#define EAT_BELOW   0.5f
#define FULL_ABOVE  0.9f
#define EAT_SECS    2.0f
#define MOSS_MEAL   0.25f
#define SMALL_BURN  0.6f

/* Dying takes DIE_MOVES moves; bodies rot into dirt after BODY_ROT_MINUTES
 * of wall-clock time (sim.c sweeps them). */
#define DIE_MOVES   3

/* Burial: buried deeper than BURY_DEPTH loose cells in every column (or
 * with sand on its head and no way to dig up) for BURY_SECS in all, it
 * suffocates. Digging up through sand moves it one row each DIG_SECS. */
#define BURY_DEPTH  10
#define BURY_SECS   60.0f
#define DIG_SECS    0.5f
#define COVERED     2

/* Farming: a pour of water adds wheat_per_pour stock levels to the field
 * within FIELD_SPOT cells of where it lands, turning moss there into
 * wheat first. A bite of wheat is one level, WHEAT_BITE of a meal.
 * Critters keep FIELD_RESERVE_HOURS of what they eat stocked. */
#define FIELD_SPOT  3
#define WHEAT_BITE  (1.0f / 3)
#define FIELD_RESERVE_HOURS 2.0f
#define POUR_SECS   1.0f

/* Homes: other homes stay HOME_RADIUS clear of one, and junk cleared off
 * one is dumped past it. A well starts WELL_ROWS deep and is deepened up
 * to WELL_MAX_ROWS once the rest is built. It's as wide as the level
 * ground allows, from WELL_MIN_W to WELL_MAX_W with its walls: its roof
 * and the hut's catch rain for it, so wider is wetter. */
#define HOME_RADIUS   12
#define WELL_ROWS     2
#define WELL_MAX_ROWS 5
#define WELL_MIN_W    7
#define WELL_MAX_W    21
#define HUT_W         5
#define HUT_GAP       3
#define HOME_W(ww)    (HUT_W + HUT_GAP + (ww))

/* Building: mining a cell of stone or iron, and laying one. */
#define MINE_STONE_SECS 3.0f
#define MINE_IRON_SECS  4.5f
#define PLACE_SECS      1.0f

/* Tossing: while a farm holds the fill, critters within TOSS_RANGE of a
 * side edge throw sand off the screen, TOSS_SCOOP cells at a time for a
 * large critter (one for a small), a throw every TOSS_SECS. Tossing can
 * offset at most TOSS_SHARE of what the emitters pour, so the screen
 * always drains in the end. Homes this close to an edge are preferred. */
#define TOSS_RANGE  30
#define TOSS_SCOOP  3
#define TOSS_SECS   0.75f
#define TOSS_SHARE  0.75f
#define EDGE_BONUS  20

/* Farms are counted about once a second. */
#define FARM_COUNT_SECS 1.0f

/* Catching rain: drops sink into sand within a moment of landing, so a
 * critter that wants water sprints, SPRINT times its walking pace, to
 * where a drop will come down within CATCH_RANGE columns, if it can get
 * there before the drop does, and grabs any drop that comes within
 * reach. Drops fall a cell a step, straight down. */
#define CATCH_RANGE 24
#define SPRINT      2

/* Ledges: a critter with no home clears the sand off the nearest stone
 * ledge, a scoop at a time as when tossing, flinging it past the nearer
 * end of the ledge. Bare stone doesn't swallow rain the way sand does.
 * It only counts sand piled at most LEDGE_PILE deep on the stone. */
#define LEDGE_PILE  12

/* Planting: a critter with no earth within SENSE plants a seed it
 * carries, turning the sand within PLANT_RADIUS of its feet to dirt, at
 * most once every PLANT_SECS. Dirt isn't food: it still takes water to
 * grow moss on it. */
#define PLANT_RADIUS 5
#define PLANT_SECS   (20 * 60.0f)

/* Looking for iron anywhere on screen costs a full scan, so a critter
 * that found none waits this long before looking again. */
#define IRON_LOOK_SECS 10.0f

enum { G_NONE, G_WANDER, G_EAT, G_DIE, G_FARM, G_STOCK, G_BUILD, G_CLEAR,
       G_DUMP, G_SWAP, G_DETOUR, G_SHELTER, G_LEAVE, G_TOSS, G_CATCH, G_LEDGE };

/* What a home's plan wants in a cell: nothing in particular, built stone
 * or iron, open (empty), or well water (empty or water). */
enum { P_NONE, P_WALL, P_OPEN, P_WATER };

static int secs(const Sim *s, float sec)
{
    int n = (int)(sec * s->steps_per_sec + 0.5f);
    return n < 1 ? 1 : n;
}

static int charged(const Sim *s, const Critter *c)
{
    return s->frame < c->charged_until;
}

/* A timing for critter c: shorter while it's charged. Every critter
 * timing but falling goes through here. */
static int csecs(const Sim *s, const Critter *c, float sec)
{
    int n = secs(s, sec);
    if (charged(s, c))
        n = (n + CHARGE_SPEEDUP - 1) / CHARGE_SPEEDUP;
    return n;
}

static inline uint16_t at(const Sim *s, int x, int y)
{
    return s->cells[y * s->w + x];
}

/* (x, y) is one of slot i's body cells. */
static int own(const Sim *s, int i, int x, int y)
{
    uint16_t c = at(s, x, y);
    return CELL_TYPE(c) == CRITTER && CRITTER_SLOT(c) == i;
}

/* Critter i's body would fit with its bottom-left at (x, y): every cell is
 * empty, water (which it pushes aside) or its own. */
static int fits(const Sim *s, int i, int x, int y)
{
    const Critter *c = &s->crit[i];
    if (x < 0 || x + c->w > s->w || y >= s->h || y - c->h + 1 < 0)
        return 0;
    for (int yy = y - c->h + 1; yy <= y; yy++)
        for (int xx = x; xx < x + c->w; xx++) {
            int t = CELL_TYPE(at(s, xx, yy));
            if (t != EMPTY && t != WATER && !own(s, i, xx, yy))
                return 0;
        }
    return 1;
}

/* Something under its feet holds it up: the floor, or any cell under its
 * bottom row that isn't empty or water. */
/* Walls, rocks, banks of earth held up by moss, and other critters,
 * which it climbs over rather than waiting nose to nose. */
static int climbable(int t)
{
    return t == STONE || t == IRON || t == GLASS || t == CRITTER || EARTH(t);
}

static int on_roof(const Sim *s, int x, int y);

static int supported(const Sim *s, const Critter *c)
{
    if (c->y + 1 >= s->h)
        return 1;
    /* Climbing, it clings to the wall it faces. */
    int wx = c->face > 0 ? c->x + c->w : c->x - 1;
    if (c->climbing && wx >= 0 && wx < s->w)
        for (int y = c->y - c->h + 1; y <= c->y; y++)
            if (climbable(CELL_TYPE(at(s, wx, y))))
                return 1;
    for (int xx = c->x; xx < c->x + c->w; xx++) {
        int t = CELL_TYPE(at(s, xx, c->y + 1));
        if ((t != EMPTY && t != WATER) || on_roof(s, xx, c->y + 1))
            return 1;
    }
    return 0;
}

/* Which part the body cell at column dx, row dy (0 = top) is. */
static int part_at(const Critter *c, int dx, int dy)
{
    if (dy == 0) {
        if (c->w == 1 || c->face == 0)     /* no eye showing, or closed */
            return PART_HEAD;
        return (dx == 1) == (c->face > 0) ? PART_EYE : PART_HEAD;
    }
    if (dy < c->h - 1 || c->h < 3)
        return c->spark ? PART_SPARK : PART_BODY;
    return (dx + c->step) & 1 ? PART_LEG_ALT : PART_LEG;
}

/* Write the body cells that aren't already right, so a critter standing
 * still doesn't wake its chunk. */
static void draw(Sim *s, int i)
{
    Critter *c = &s->crit[i];
    for (int dy = 0; dy < c->h; dy++)
        for (int dx = 0; dx < c->w; dx++) {
            int x = c->x + dx, y = c->y - c->h + 1 + dy;
            uint16_t v = CRITTER_CELL(i, part_at(c, dx, dy));
            if (at(s, x, y) != v)
                set_cell(s, x, y, v);
        }
}

/* Move critter i's body to (nx, ny), which must fit. Water in the way is
 * moved into the cells the body leaves, so the water total holds. */
static void relocate(Sim *s, int i, int nx, int ny)
{
    Critter *c = &s->crit[i];
    uint16_t water[6];
    int nw = 0;
    for (int yy = ny - c->h + 1; yy <= ny; yy++)
        for (int xx = nx; xx < nx + c->w; xx++)
            if (CELL_TYPE(at(s, xx, yy)) == WATER)
                water[nw++] = at(s, xx, yy);
    for (int yy = c->y - c->h + 1; yy <= c->y; yy++)
        for (int xx = c->x; xx < c->x + c->w; xx++)
            set_cell(s, xx, yy, CELL(EMPTY, 0));
    int ox = c->x, oy = c->y;
    c->x = nx;
    c->y = ny;
    draw(s, i);
    for (int yy = oy - c->h + 1; yy <= oy && nw > 0; yy++)
        for (int xx = ox; xx < ox + c->w && nw > 0; xx++)
            if (CELL_TYPE(at(s, xx, yy)) == EMPTY)
                set_cell(s, xx, yy, water[--nw] & ~(SEEPING << 8));
}

/* Loose stuff a critter digs through. Not water (it just wades), and
 * not moss or wheat, which it leaves for food. */
static int diggable(int t)
{
    return FALLS(t) && !MOSSY(t);
}

/* Put cell v down near critter c, on the side away from d: the first
 * open cell going up the column behind it, or on its head. Returns 0 if
 * there's nowhere to put it. */
static int drop_behind(Sim *s, Critter *c, int d, uint16_t v)
{
    int bx = d > 0 ? c->x - 1 : c->x + c->w;
    if (bx >= 0 && bx < s->w)
        for (int y = c->y; y >= c->y - c->h && y >= 0; y--)
            if (CELL_TYPE(at(s, bx, y)) == EMPTY) {
                set_cell(s, bx, y, v);
                return 1;
            }
    int hy = c->y - c->h;
    for (int x = c->x; x < c->x + c->w && hy >= 0; x++)
        if (CELL_TYPE(at(s, x, hy)) == EMPTY) {
            set_cell(s, x, hy, v);
            return 1;
        }
    return 0;
}

/* Move one cell sideways (d = -1 or 1): straight across, or up a
 * one-cell step. Blocked by loose stuff, it digs out the top cell in the
 * way and throws it over its shoulder instead. Returns 1 if it moved or
 * dug. */
static int built(uint16_t v)
{
    int t = CELL_TYPE(v);
    return (t == STONE || t == IRON) && (CELL_COLOR(v) & BUILT);
}

static int step_side(Sim *s, int i, int d)
{
    Critter *c = &s->crit[i];
    c->face = (int8_t)d;
    c->climbing = 0;
    if (fits(s, i, c->x + d, c->y)) {
        c->step++;
        relocate(s, i, c->x + d, c->y);
        return 1;
    }
    if (fits(s, i, c->x, c->y - 1) && fits(s, i, c->x + d, c->y - 1)) {
        c->step++;
        relocate(s, i, c->x + d, c->y - 1);
        return 1;
    }
    /* The column it's stepping into, from a step up down to its feet. */
    int nx = d > 0 ? c->x + c->w : c->x - 1;
    /* Walls and rocks it can clamber up, a cell at a time. */
    if (nx >= 0 && nx < s->w && fits(s, i, c->x, c->y - 1))
        for (int y = c->y - c->h + 1; y <= c->y; y++)
            if (climbable(CELL_TYPE(at(s, nx, y)))) {
                c->step++;
                c->climbing = 1;
                relocate(s, i, c->x, c->y - 1);
                return 1;
            }
    if (nx >= 0 && nx < s->w)
        for (int y = c->y - c->h; y <= c->y; y++) {
            if (y < 0)
                continue;
            uint16_t v = at(s, nx, y);
            int t = CELL_TYPE(v);
            if (t == EMPTY || t == WATER)
                continue;
            /* It tunnels under a field rather than through it. */
            if (MOSSY(t))
                continue;
            if (!diggable(t))
                break;
            set_cell(s, nx, y, CELL(EMPTY, 0));
            if (!drop_behind(s, c, d, v)) {
                set_cell(s, nx, y, v);
                break;
            }
            c->state = CR_DIG;
            draw(s, i);
            return 1;
        }
    draw(s, i);
    return 0;
}

/* Loose cells (not water) piled on critter c's head: the least over any
 * of its columns, and whether there's any at all. */
static int buried_under(const Sim *s, const Critter *c, int *any)
{
    int least = 1 << 30;
    *any = 0;
    for (int x = c->x; x < c->x + c->w; x++) {
        int n = 0;
        for (int y = c->y - c->h; y >= 0 && FALLS(CELL_TYPE(at(s, x, y))); y--)
            n++;
        if (n)
            *any = 1;
        if (n < least)
            least = n;
    }
    return least;
}

/* Wriggle up one row through the loose cells on its head: they end up
 * under its feet. Returns 0 if something solid is in the way. */
static int dig_up(Sim *s, int i)
{
    Critter *c = &s->crit[i];
    int hy = c->y - c->h;
    uint16_t v[2];
    if (hy < 0)
        return 0;
    for (int k = 0; k < c->w; k++) {
        v[k] = at(s, c->x + k, hy);
        int t = CELL_TYPE(v[k]);
        if (t != EMPTY && t != WATER && !FALLS(t))
            return 0;
    }
    for (int k = 0; k < c->w; k++)
        set_cell(s, c->x + k, hy, CELL(EMPTY, 0));
    int oy = c->y;
    relocate(s, i, c->x, c->y - 1);
    for (int k = 0; k < c->w; k++)
        if (CELL_TYPE(at(s, c->x + k, oy)) == EMPTY)
            set_cell(s, c->x + k, oy, v[k]);
    c->state = CR_DIG;
    return 1;
}

/* Put what it carries down at (x, y). */
static void put_down(Sim *s, Critter *c, int x, int y)
{
    set_cell(s, x, y, c->carry);
    c->carry = 0;
}

/* Something under (x, y) that isn't open, or the floor. */
static int solid_below(const Sim *s, int x, int y)
{
    if (y + 1 >= s->h)
        return 1;
    int t = CELL_TYPE(at(s, x, y + 1));
    return t != EMPTY && t != WATER && t != CRITTER;
}

/* Drop what it's carrying on the nearest open cell around it, beside its
 * feet if it can. Stone it was going to build with goes down as loose
 * debris, so it doesn't hang in the air. It's lost only if it's boxed
 * in. */
static void drop_carry(Sim *s, Critter *c)
{
    if (!c->carry)
        return;
    if (CELL_TYPE(c->carry) == STONE)
        c->carry = CELL(DEBRIS, CELL_COLOR(c->carry) & ~BUILT);
    for (int k = 0; k < 2; k++) {
        int x = k == 0 ? c->x - 1 : c->x + c->w;
        if (x >= 0 && x < s->w && CELL_TYPE(at(s, x, c->y)) == EMPTY) {
            put_down(s, c, x, c->y);
            return;
        }
    }
    /* Iron doesn't fall: it goes only where something holds it up. */
    int needs_support = !FALLS(CELL_TYPE(c->carry)) && CELL_TYPE(c->carry) != WATER;
    for (int r = 0; r <= 2; r++)
        for (int y = c->y + r; y >= c->y - c->h - r; y--)
            for (int x = c->x - 1 - r; x <= c->x + c->w + r; x++)
                if (x >= 0 && x < s->w && y >= 0 && y < s->h &&
                    CELL_TYPE(at(s, x, y)) == EMPTY &&
                    (!needs_support || solid_below(s, x, y))) {
                    put_down(s, c, x, y);
                    return;
                }
    c->carry = 0;
}

/* Cells critter c can touch without moving: its own body and the ring
 * of cells around it. */
static int in_reach(const Critter *c, int x, int y)
{
    return x >= c->x - 1 && x <= c->x + c->w && y >= c->y - c->h && y <= c->y + 1;
}

/* Steps from critter c's reach to (x, y), roughly. */
static int dist(const Critter *c, int x, int y)
{
    int dx = x < c->x - 1 ? c->x - 1 - x : x > c->x + c->w ? x - c->x - c->w : 0;
    int dy = y < c->y - c->h ? c->y - c->h - y : y > c->y + 1 ? y - c->y - 1 : 0;
    return dx + dy;
}

static void set_goal(Sim *s, Critter *c, int goal, int tx, int ty)
{
    (void)s;
    c->goal = goal;
    c->tx = tx;
    c->ty = ty;
    c->stuck = 0;
    c->best = goal == G_DUMP ? 0 : dist(c, tx, ty);
    c->work_until = 0;
    c->state = CR_WALK;
}

/* Done with the goal (or giving up on it): think again next move. */
static void end_goal(Sim *s, Critter *c)
{
    c->goal = G_NONE;
    c->state = CR_IDLE;
    c->work_until = 0;
    c->next_think = s->frame;
}

/* Give up on the goal's spot for a while. Walking straight at things
 * can leave it stuck behind a wall with everything it wants on the other
 * side: after a few failures in a row it heads off the other way for a
 * while, to come at them from somewhere else. */
static void fail_goal(Sim *s, Critter *c)
{
    int away = c->tx < c->x ? 1 : -1;
    c->avoid_x = c->tx;
    c->avoid_y = c->ty;
    c->avoid_until = s->frame + csecs(s, c, AVOID_SECS);
    end_goal(s, c);
    if (++c->fails >= FAILS_BEFORE_DETOUR) {
        c->fails = 0;
        int tx = c->x + away * (DETOUR + (int)(rnd(s) % DETOUR));
        c->goal = G_DETOUR;
        c->tx = tx < 0 ? 0 : tx > s->w - c->w ? s->w - c->w : tx;
        c->ty = c->y;
        c->stuck = 0;
        c->state = CR_WALK;
    }
}

/* One move toward the goal cell. Returns 1 once it's in reach, 0 while
 * still on the way; gives up on the goal after STUCK_MOVES moves that
 * don't get it any closer. */
static int approach(Sim *s, int i)
{
    Critter *c = &s->crit[i];
    if (in_reach(c, c->tx, c->ty))
        return 1;
    int d = c->tx < c->x ? -1 : c->tx >= c->x + c->w ? 1 : 0;
    /* Right above or below it but out of reach: step aside and look
     * again, so it can drop into a hollow. */
    if (d == 0)
        d = c->face;
    step_side(s, i, d);
    int now = dist(c, c->tx, c->ty);
    if (now < c->best) {
        c->best = now;
        c->stuck = 0;
    } else if (++c->stuck >= STUCK_MOVES) {
        fail_goal(s, c);
    }
    return 0;
}

static int avoided(const Sim *s, const Critter *c, int x, int y)
{
    return s->frame < c->avoid_until && abs(x - c->avoid_x) <= 1 && abs(y - c->avoid_y) <= 1;
}

/* Open to a critter: something it could stand in or next to. */
static int exposed(const Sim *s, int i, int x, int y)
{
    static const int nb[4][2] = {{0, -1}, {-1, 0}, {1, 0}, {0, 1}};
    for (int k = 0; k < 4; k++) {
        int xx = x + nb[k][0], yy = y + nb[k][1];
        if (xx < 0 || xx >= s->w || yy < 0 || yy >= s->h)
            continue;
        int t = CELL_TYPE(at(s, xx, yy));
        if (t == EMPTY || t == WATER || own(s, i, xx, yy))
            return 1;
    }
    return 0;
}

/* Ranks a cell for a search: lower is better, -1 to skip it. */
typedef int (*Rank)(const Sim *s, int x, int y);

/* The best-ranked exposed cell within SENSE, the nearest of equals.
 * Returns 0 if none. */
static int find_best(Sim *s, int i, Rank rank, int *fx, int *fy)
{
    Critter *c = &s->crit[i];
    int best = 1 << 30;
    int x0 = c->x - SENSE, x1 = c->x + SENSE, y0 = c->y - SENSE, y1 = c->y + SENSE;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= s->w) x1 = s->w - 1;
    if (y1 >= s->h) y1 = s->h - 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            int r = rank(s, x, y);
            if (r < 0)
                continue;
            int score = r * 1024 + dist(c, x, y);
            if (score >= best || avoided(s, c, x, y) || !exposed(s, i, x, y))
                continue;
            best = score;
            *fx = x;
            *fy = y;
        }
    return best < (1 << 30);
}

/* Open above: grazed or watered from on top, not through a wall. */
static int open_above(const Sim *s, int x, int y)
{
    int t = y > 0 ? CELL_TYPE(at(s, x, y - 1)) : EMPTY;
    return t == EMPTY || t == WATER || t == CRITTER;
}

/* Food: the fullest wheat first, which keeps a field even, then moss. */
static int rank_food(const Sim *s, int x, int y)
{
    uint16_t v = at(s, x, y);
    if (!open_above(s, x, y))
        return -1;
    if (CELL_TYPE(v) == WHEAT && WHEAT_STOCK(v) > 0)
        return WHEAT_MAX - WHEAT_STOCK(v);
    return CELL_TYPE(v) == MOSS ? WHEAT_MAX + 1 : -1;
}

/* Wheat cells within r of (x, y). */
static int wheat_near(const Sim *s, int x, int y, int r)
{
    int n = 0;
    for (int yy = y - r; yy <= y + r; yy++)
        for (int xx = x - r; xx <= x + r; xx++)
            if (xx >= 0 && xx < s->w && yy >= 0 && yy < s->h &&
                CELL_TYPE(at(s, xx, yy)) == WHEAT)
                n++;
    return n;
}

/* Where to pour: the emptiest wheat, then moss beside the field to make
 * it bigger. Fuller wheat is only worth it once there's no moss to
 * take in. With no moss at all, water on dirt starts some, the way rain
 * does: wet dirt first, since it turns straight to moss. */
static int rank_pour(const Sim *s, int x, int y)
{
    uint16_t v = at(s, x, y);
    if (!open_above(s, x, y))
        return -1;
    if (CELL_TYPE(v) == WHEAT) {
        int k = WHEAT_STOCK(v);
        return k >= WHEAT_MAX ? -1 : k < WHEAT_MAX / 2 ? k : 2 * WHEAT_MAX + k;
    }
    if (CELL_TYPE(v) == MOSS)
        return wheat_near(s, x, y, FIELD_SPOT) ? WHEAT_MAX : 2 * WHEAT_MAX;
    if (CELL_TYPE(v) == WET_DIRT)
        return 3 * WHEAT_MAX + 1;
    if (CELL_TYPE(v) == DIRT)
        return 3 * WHEAT_MAX + 2;
    return -1;
}

/* Any earth: moss and wheat to eat or farm, dirt to grow moss on. */
static int rank_earth(const Sim *s, int x, int y)
{
    return EARTH(CELL_TYPE(at(s, x, y))) && open_above(s, x, y) ? 0 : -1;
}

/* Turn the sand around critter c's feet into dirt. Returns 0 if there
 * was no sand there to plant in. */
static int plant(Sim *s, Critter *c)
{
    int n = 0, cx = c->x + c->w / 2, cy = c->y + 1, r = PLANT_RADIUS;
    for (int y = cy - r; y <= cy + r; y++)
        for (int x = cx - r; x <= cx + r; x++) {
            int dx = x - cx, dy = y - cy;
            if (x < 0 || x >= s->w || y < 0 || y >= s->h || dx * dx + dy * dy > r * r ||
                CELL_TYPE(at(s, x, y)) != SAND)
                continue;
            set_cell(s, x, y, CELL(DIRT, rnd(s) & 0xFF));
            n++;
        }
    s->planted += n > 0;
    return n > 0;
}

/* Water to carry: open water that isn't already in a well. */
static int rank_water(const Sim *s, int x, int y)
{
    return CELL_TYPE(at(s, x, y)) == WATER && !in_well(s, x, y) ? 0 : -1;
}

/* ---- Homes ---- */

/* HUT_GAP columns apart: a one-column gap would trap a large critter
 * under the edge of the hut's roof. */
static int hut_x(const Home *h)
{
    return h->dir > 0 ? h->x : h->x + h->ww + HUT_GAP;
}

static int well_x(const Home *h)
{
    return h->dir > 0 ? h->x + HUT_W + HUT_GAP : h->x;
}

static int home_w(const Home *h)
{
    return HOME_W(h->ww);
}

static int well_top(const Home *h)
{
    return h->y - h->rows - 1;
}

/* The plan: a well (7 wide at the least) with a one-cell mouth in the
 * middle of its roof, and a 5x5 hut beside it.
 *
 *     hut      well
 *    #####    ###.###
 *             #     #
 *                   #
 *             #######
 *    #####
 *
 * The hut is a roof over a floor, open at both ends, three cells high
 * inside so a large critter fits. With a wall and a door, critters
 * walking straight at what they want would keep getting stuck inside or
 * behind it. */
static int plan_at(const Home *h, int x, int y)
{
    int f = h->y, wx = well_x(h), top = well_top(h);
    if (x >= wx && x < wx + h->ww && y >= top && y <= f) {
        int cx = x - wx;
        if (y == f)
            return P_WALL;
        if (y == top)
            return cx == h->ww / 2 ? P_OPEN : P_WALL;
        return cx == 0 || cx == h->ww - 1 ? P_WALL : P_WATER;
    }
    int hx = hut_x(h);
    if (x >= hx && x < hx + HUT_W && y >= f - 4 && y <= f)
        return y == f || y == f - 4 ? P_WALL : P_OPEN;
    return P_NONE;
}

static void well_rect(Home *h)
{
    h->wx0 = well_x(h) + 1;
    h->wx1 = well_x(h) + h->ww - 2;
    h->wy0 = well_top(h) + 1;
    h->wy1 = h->y - 1;
}

static void mouth(const Home *h, int *x, int *y)
{
    *x = well_x(h) + h->ww / 2;
    *y = well_top(h);
}

/* (x, y) is in the roof row of a well, mouth and gaps included, which
 * critters walk across as if it were finished: they'd fall in through
 * the mouth, or through a gap while it's being built, and be shut in.
 * The row under it counts too: it's the old roof while a well is being
 * deepened. */
static int on_roof(const Sim *s, int x, int y)
{
    for (int k = 0; k < MAX_HOMES; k++) {
        const Home *h = &s->home[k];
        if (h->used && (y == well_top(h) || y == well_top(h) + 1) &&
            x >= well_x(h) && x < well_x(h) + h->ww)
            return 1;
    }
    return 0;
}

/* Rain landing on a built roof cell of a hut or a well runs down a
 * gutter into the lowest open cell of the well, if it has room; it gets
 * there at once. */
int gutter(Sim *s, int x, int y)
{
    if (y + 1 >= s->h || !built(at(s, x, y + 1)))
        return 0;
    for (int k = 0; k < MAX_HOMES; k++) {
        const Home *h = &s->home[k];
        if (!h->used)
            continue;
        int hx = hut_x(h), wx = well_x(h);
        if (!(y + 1 == well_top(h) && x >= wx && x < wx + h->ww) &&
            !(y + 1 == h->y - 4 && x >= hx && x < hx + HUT_W))
            continue;
        for (int wy = h->wy1; wy >= h->wy0; wy--)
            for (int wxx = h->wx0; wxx <= h->wx1; wxx++)
                if (CELL_TYPE(at(s, wxx, wy)) == EMPTY) {
                    uint16_t drop = at(s, x, y);
                    set_cell(s, x, y, CELL(EMPTY, 0));
                    set_cell(s, wxx, wy, CELL(WATER, CELL_COLOR(drop) & 0x3F));
                    s->guttered++;
                    return 1;
                }
        return 0;
    }
    return 0;
}

/* Every plan cell of the well (or the hut) is built. */
static int part_done(const Sim *s, const Home *h, int well)
{
    int x0 = well ? well_x(h) : hut_x(h), x1 = x0 + (well ? h->ww : HUT_W);
    int y0 = well ? well_top(h) : h->y - 4;
    for (int y = y0; y <= h->y; y++)
        for (int x = x0; x < x1; x++)
            if (plan_at(h, x, y) == P_WALL && !built(at(s, x, y)))
                return 0;
    return 1;
}

static int well_water(const Sim *s, const Home *h, int *room)
{
    int n = 0, cells = 0;
    for (int y = h->wy0; y <= h->wy1; y++)
        for (int x = h->wx0; x <= h->wx1; x++) {
            n += CELL_TYPE(at(s, x, y)) == WATER;
            cells++;
        }
    *room = cells - n;
    return n;
}

/* Close to any home's footprint, where the ground holds it up: not to be
 * mined. */
static int near_home(const Sim *s, int x, int y)
{
    for (int k = 0; k < MAX_HOMES; k++) {
        const Home *h = &s->home[k];
        if (h->used && x >= h->x - 1 && x <= h->x + home_w(h) &&
            y >= well_top(h) - 1 && y <= h->y + 2)
            return 1;
    }
    return 0;
}

/* Top of the ground in column x: the first cell that isn't empty or
 * water, or the floor. */
static int ground_y(const Sim *s, int x)
{
    for (int y = 0; y < s->h; y++) {
        int t = CELL_TYPE(at(s, x, y));
        if (t != EMPTY && t != WATER && t != CRITTER)
            return y;
    }
    return s->h;
}

/* Something solid enough to build against. */
static int solid_at(const Sim *s, int x, int y)
{
    if (x < 0 || x >= s->w || y >= s->h)
        return y >= s->h;
    if (y < 0)
        return 0;
    int t = CELL_TYPE(at(s, x, y));
    return t != EMPTY && t != WATER && t != CRITTER;
}

/* Any critter's body overlaps the box x0..x1, y0..y1. */
static int critter_in(const Sim *s, int x0, int y0, int x1, int y1)
{
    for (int i = 0; i < MAX_CRITTERS; i++) {
        const Critter *c = &s->crit[i];
        if (c->state != CR_FREE && c->x <= x1 && c->x + c->w - 1 >= x0 &&
            c->y >= y0 && c->y - c->h + 1 <= y1)
            return 1;
    }
    return 0;
}

/* The lowest plan cell still to build that's open and has something
 * solid beside or under it. The hut's roof has nothing under it but the
 * floor, so it goes up once the floor is down. The well's roof waits
 * while anyone is down in it, or they'd be shut in. */
static int next_build(const Sim *s, const Home *h, int *bx, int *by)
{
    int top = well_top(h) < h->y - 4 ? well_top(h) : h->y - 4;
    int roof_ok = !critter_in(s, h->wx0, h->wy0, h->wx1, h->wy1);
    int hx = hut_x(h), floor_done = 1;
    for (int x = hx; x < hx + HUT_W; x++)
        floor_done &= built(at(s, x, h->y));
    for (int y = h->y; y >= top; y--)
        for (int x = h->x; x < h->x + home_w(h); x++) {
            if (plan_at(h, x, y) != P_WALL || CELL_TYPE(at(s, x, y)) != EMPTY)
                continue;
            if (!roof_ok && y == well_top(h) && x >= well_x(h) && x < well_x(h) + h->ww)
                continue;
            if (solid_at(s, x - 1, y) || solid_at(s, x + 1, y) ||
                solid_at(s, x, y + 1) || solid_at(s, x, y - 1) ||
                (floor_done && y == h->y - 4 && x >= hx && x < hx + HUT_W)) {
                *bx = x;
                *by = y;
                return 1;
            }
        }
    return 0;
}

/* Something in a plan cell that has to go: anything loose where the plan
 * wants it open or built (water only where it wants water), or junk on
 * top of the field or in the well. Returns the cell to take; well junk
 * is taken through the mouth. */
static int next_clear(const Sim *s, const Home *h, int *jx, int *jy)
{
    int top = well_top(h) < h->y - 4 ? well_top(h) : h->y - 4;
    for (int y = top; y <= h->y; y++)
        for (int x = h->x; x < h->x + home_w(h); x++) {
            int p = plan_at(h, x, y), t = CELL_TYPE(at(s, x, y));
            /* A wall where it's now open is taken down and used again.
             * The old roof of a deepened well waits for the new one, then
             * comes out through the mouth, so nobody falls in. */
            if (p == P_OPEN && built(at(s, x, y))) {
                *jx = x;
                *jy = y;
                return 1;
            }
            if (p == P_WATER && built(at(s, x, y))) {
                if (!part_done(s, h, 1))
                    continue;
                mouth(h, jx, jy);
                return 1;
            }
            if (p == P_NONE || t == EMPTY || !(FALLS(t) || t == WATER))
                continue;
            if (p == P_WATER && t == WATER)
                continue;
            if (p == P_WALL && t == WATER)
                continue;           /* it'll flow off */
            if (p == P_WATER)
                mouth(h, jx, jy);
            else {
                *jx = x;
                *jy = y;
            }
            return 1;
        }
    return 0;
}

/* Loose earth banked against the outside of a well wall that isn't
 * finished: nobody can get at the wall to build it, so the bank is dug
 * away from the top. */
static int wall_bank(const Sim *s, const Home *h, int *jx, int *jy)
{
    for (int k = 0; k < 2; k++) {
        int wx = k ? well_x(h) + h->ww - 1 : well_x(h), out = k ? wx + 1 : wx - 1;
        int open = 0;
        for (int y = h->wy0; y <= h->wy1; y++)
            open |= !built(at(s, wx, y));
        if (!open || out < 0 || out >= s->w)
            continue;
        for (int y = well_top(h) - 3 < 0 ? 0 : well_top(h) - 3; y < h->y; y++)
            if (diggable(CELL_TYPE(at(s, out, y)))) {
                *jx = out;
                *jy = y;
                return 1;
            }
    }
    return 0;
}

/* Loose cells piled on the hut's or the well's roof. */
static int roof_junk(const Sim *s, const Home *h, int *jx, int *jy)
{
    for (int k = 0; k < 2; k++) {
        int x0 = k ? well_x(h) : hut_x(h), x1 = x0 + (k ? h->ww : HUT_W);
        int y = (k ? well_top(h) : h->y - 4) - 1;
        if (y < 0)
            continue;
        for (int x = x0; x < x1; x++)
            if (diggable(CELL_TYPE(at(s, x, y)))) {
                *jx = x;
                *jy = y;
                return 1;
            }
    }
    return 0;
}

/* Stuff to clear off the field near a home: loose cells lying on wheat
 * within HOME_RADIUS of it. Junk is dumped further out than that, so it
 * doesn't get cleared again. */
static int field_junk(const Sim *s, const Home *h, int *jx, int *jy)
{
    int x0 = h->x - HOME_RADIUS, x1 = h->x + home_w(h) + HOME_RADIUS;
    for (int y = h->y - HOME_RADIUS; y <= h->y + HOME_RADIUS; y++)
        for (int x = x0; x < x1; x++) {
            if (x < 0 || x >= s->w || y < 0 || y + 1 >= s->h)
                continue;
            if (diggable(CELL_TYPE(at(s, x, y))) && CELL_TYPE(at(s, x, y + 1)) == WHEAT) {
                *jx = x;
                *jy = y;
                return 1;
            }
        }
    return 0;
}

/* Building material: iron first (large critters only), then stone, then
 * fallen debris. Never anything already built, or the ground under a
 * home. */
static int rank_stone(const Sim *s, int x, int y)
{
    uint16_t v = at(s, x, y);
    int t = CELL_TYPE(v);
    if ((t == STONE && !(CELL_COLOR(v) & BUILT)) || t == DEBRIS)
        return near_home(s, x, y) ? -1 : 1;
    return -1;
}

/* Nothing in sight: the best-ranked cell open above anywhere on screen,
 * nearest of equals. A full scan, so it waits IRON_LOOK_SECS after
 * finding nothing. */
static int find_far(Sim *s, int i, Rank rank, int *fx, int *fy)
{
    Critter *c = &s->crit[i];
    if (s->frame < c->far_next)
        return 0;
    int best = 1 << 30;
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) {
            int r = rank(s, x, y);
            if (r < 0)
                continue;
            int score = r * 1024 + dist(c, x, y);
            if (score >= best || avoided(s, c, x, y) || !open_above(s, x, y))
                continue;
            best = score;
            *fx = x;
            *fy = y;
        }
    if (best < (1 << 30))
        return 1;
    c->far_next = s->frame + csecs(s, c, IRON_LOOK_SECS);
    return 0;
}

/* Unbuilt iron anywhere on the screen, the nearest first: iron is rare,
 * and worth a long trip. Large critters only. */
static int find_iron(Sim *s, int i, int *fx, int *fy)
{
    Critter *c = &s->crit[i];
    if (c->kind != CR_LARGE || s->iron == 0 || s->frame < c->iron_next)
        return 0;
    int best = 1 << 30;
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) {
            uint16_t v = at(s, x, y);
            if (CELL_TYPE(v) != IRON || (CELL_COLOR(v) & BUILT))
                continue;
            int d = dist(c, x, y);
            if (d >= best || near_home(s, x, y) || avoided(s, c, x, y) || !open_above(s, x, y))
                continue;
            best = d;
            *fx = x;
            *fy = y;
        }
    if (best < (1 << 30))
        return 1;
    c->iron_next = s->frame + csecs(s, c, IRON_LOOK_SECS);
    return 0;
}

/* Open air beside (x, y), outside any well: somewhere a critter could
 * stand to work on it. */
static int open_beside(const Sim *s, int x, int y)
{
    static const int nb[4][2] = {{0, -1}, {-1, 0}, {1, 0}, {0, 1}};
    for (int k = 0; k < 4; k++) {
        int xx = x + nb[k][0], yy = y + nb[k][1];
        if (xx < 0 || xx >= s->w || yy < 0 || yy >= s->h || in_well(s, xx, yy))
            continue;
        int t = CELL_TYPE(at(s, xx, yy));
        if (t == EMPTY || t == CRITTER)
            return 1;
    }
    return 0;
}

/* The well's own floor, under the water: only reached through the mouth. */
static int well_floor(const Home *h, int x, int y)
{
    return y == h->y && x >= h->wx0 && x <= h->wx1;
}

/* The lowest built stone in the plan that critter c can get at, floors
 * first: what iron replaces (rusted iron included). The well's floor is
 * swapped through the mouth, so for that it gives the mouth. */
static int stone_to_swap(const Sim *s, const Critter *c, const Home *h, int *bx, int *by)
{
    int top = well_top(h) < h->y - 4 ? well_top(h) : h->y - 4;
    for (int y = h->y; y >= top; y--)
        for (int x = h->x; x < h->x + home_w(h); x++) {
            if (plan_at(h, x, y) != P_WALL || CELL_TYPE(at(s, x, y)) != STONE ||
                !built(at(s, x, y)))
                continue;
            if (well_floor(h, x, y)) {
                mouth(h, bx, by);
                if (!avoided(s, c, *bx, *by))
                    return 1;
            } else if (open_beside(s, x, y) && !avoided(s, c, x, y)) {
                *bx = x;
                *by = y;
                return 1;
            }
        }
    return 0;
}

/* Critters living at home k. */
static int residents(const Sim *s, int k, float *eat_per_hour)
{
    int n = 0;
    *eat_per_hour = 0;
    for (int i = 0; i < MAX_CRITTERS; i++) {
        const Critter *c = &s->crit[i];
        if (c->state == CR_FREE || c->home != k)
            continue;
        n++;
        *eat_per_hour += 60.0f / s->starve_minutes * (c->kind == CR_SMALL ? SMALL_BURN : 1.0f);
    }
    return n;
}

/* A spot for a new home with a well ww wide near critter c: within
 * SENSE, the ground level to within a cell over all its columns, room
 * above it for the tallest well, food within SENSE, and clear of other
 * homes. Nearer is better. */
static int find_site_w(Sim *s, int i, int ww, Home *out)
{
    Critter *c = &s->crit[i];
    int best = 1 << 30, hw = HOME_W(ww);
    for (int x0 = c->x - SENSE; x0 <= c->x + SENSE; x0++) {
        if (x0 < 1 || x0 + hw >= s->w - 1)
            continue;
        int lo = s->h, hi = -1, ok = 1;
        for (int x = x0; x < x0 + hw; x++) {
            int g = ground_y(s, x);
            if (g < lo) lo = g;
            if (g > hi) hi = g;
        }
        if (hi - lo > 2)
            continue;
        int f = (lo + hi) / 2;
        if (lo + 1 == hi)
            f = hi;
        f -= 1;             /* floors sit on the ground */
        if (f - WELL_MAX_ROWS - 2 < 0 || f >= s->h)
            continue;
        for (int k = 0; k < MAX_HOMES && ok; k++) {
            const Home *h = &s->home[k];
            if (h->used && x0 < h->x + home_w(h) + HOME_RADIUS &&
                h->x < x0 + hw + HOME_RADIUS)
                ok = abs(h->y - f) > 2 * HOME_RADIUS;
        }
        /* Nothing static but stone and iron in the way. */
        for (int y = f - 5; y <= f && ok; y++)
            for (int x = x0; x < x0 + hw && ok; x++) {
                int t = CELL_TYPE(at(s, x, y));
                ok = t == EMPTY || LOOSE(t) || t == STONE || t == IRON || t == CRITTER;
            }
        if (!ok)
            continue;
        int d = abs(x0 + hw / 2 - c->x) + abs(f - c->y);
        if (x0 < TOSS_RANGE || x0 + hw > s->w - TOSS_RANGE)
            d -= EDGE_BONUS;
        if (d >= best)
            continue;
        /* Food near the middle of it. */
        int food = 0;
        for (int y = f - SENSE / 2; y <= f + SENSE / 2 && !food; y++)
            for (int x = x0 - SENSE / 2; x <= x0 + hw + SENSE / 2 && !food; x++)
                food = x >= 0 && x < s->w && y >= 0 && y < s->h &&
                       MOSSY(CELL_TYPE(at(s, x, y)));
        if (!food)
            continue;
        best = d;
        memset(out, 0, sizeof *out);
        out->used = 1;
        out->x = x0;
        out->y = f;
        out->rows = WELL_ROWS;
        out->ww = ww;
        /* The well goes on the side nearer the food, the hut away. */
        out->dir = c->ax >= x0 + hw / 2 ? 1 : -1;
        well_rect(out);
    }
    return best < (1 << 30);
}

/* The widest well that has a site. */
static int find_site(Sim *s, int i, Home *out)
{
    for (int ww = WELL_MAX_W; ww >= WELL_MIN_W; ww -= 2)
        if (find_site_w(s, i, ww, out))
            return 1;
    return 0;
}

/* Join the nearest home within SENSE, or plan a new one. */
static int settle(Sim *s, int i)
{
    Critter *c = &s->crit[i];
    int best = -1, bd = SENSE + 1;
    for (int k = 0; k < MAX_HOMES; k++) {
        const Home *h = &s->home[k];
        if (!h->used)
            continue;
        int d = abs(h->x + home_w(h) / 2 - c->x) + abs(h->y - c->y);
        if (d < bd) {
            bd = d;
            best = k;
        }
    }
    if (best < 0) {
        Home site;
        for (int k = 0; k < MAX_HOMES && best < 0; k++)
            if (!s->home[k].used && find_site(s, i, &site)) {
                s->home[k] = site;
                best = k;
            }
    }
    if (best < 0)
        return 0;
    c->home = best;
    c->ax = hut_x(&s->home[best]) + 2;
    return 1;
}

/* Where to put junk: past HOME_RADIUS from home, on the near side, or
 * the far side if the near side is off the screen. */
static int dump_x(const Sim *s, const Critter *c)
{
    if (c->home < 0)
        return c->x + (c->x < s->w / 2 ? 6 : -6);
    const Home *h = &s->home[c->home];
    int left = h->x - HOME_RADIUS - 4, right = h->x + home_w(h) + HOME_RADIUS + 4;
    int x = c->x < h->x + home_w(h) / 2 ? left : right;
    if (x < 1)
        x = right;
    if (x > s->w - 2)
        x = left;
    return x < 1 ? 1 : x > s->w - 2 ? s->w - 2 : x;
}

/* The top water cell anywhere in the well, or (junk) the top thing that
 * isn't water: a bucket on a rope. */
static int draw_from_well(Sim *s, Critter *c, const Home *h, int junk)
{
    for (int y = h->wy0; y <= h->wy1; y++)
        for (int x = h->wx0; x <= h->wx1; x++) {
            int t = CELL_TYPE(at(s, x, y));
            if (t == EMPTY || t == CRITTER || (t == WATER) == junk)
                continue;
            c->carry = t == WATER ? CELL(WATER, CELL_COLOR(at(s, x, y)) & 0x3F)
                                  : at(s, x, y) & ~(BUILT << 8);
            set_cell(s, x, y, CELL(EMPTY, 0));
            return 1;
        }
    return 0;
}

/* Sim time in seconds, which wheat counts its rot by. */
static uint32_t sim_secs(const Sim *s)
{
    return s->frame / (uint32_t)s->steps_per_sec;
}

/* Write a wheat cell with stock k. A fresh pour restarts its rot clock;
 * otherwise it keeps the time it had (set_cell would set it to now). */
static void set_wheat(Sim *s, int x, int y, int k, int poured)
{
    int idx = y * s->w + x;
    uint32_t t = poured || CELL_TYPE(s->cells[idx]) != WHEAT ? sim_secs(s) : s->since[idx];
    set_cell(s, x, y, CELL(WHEAT, (CELL_COLOR(s->cells[idx]) & ~WHEAT_MAX) | k));
    s->since[idx] = t;
}

/* Pour a water cell at (x, y): moss within FIELD_SPOT becomes wheat, then
 * wheat_per_pour levels go one at a time to the lowest wheat there.
 * Returns 0, changing nothing, if there's no moss or room in reach. */
static int pour(Sim *s, int x, int y)
{
    int moss = 0, room = 0, t = CELL_TYPE(at(s, x, y));
    if (t == DIRT || t == WET_DIRT) {
        soak(s, x, y, t == DIRT);
        s->soaks++;
        return 1;
    }
    for (int yy = y - FIELD_SPOT; yy <= y + FIELD_SPOT; yy++)
        for (int xx = x - FIELD_SPOT; xx <= x + FIELD_SPOT; xx++) {
            if (xx < 0 || xx >= s->w || yy < 0 || yy >= s->h)
                continue;
            uint16_t v = at(s, xx, yy);
            moss += CELL_TYPE(v) == MOSS;
            room += CELL_TYPE(v) == WHEAT && WHEAT_STOCK(v) < WHEAT_MAX;
        }
    if (!moss && !room)
        return 0;
    for (int yy = y - FIELD_SPOT; yy <= y + FIELD_SPOT; yy++)
        for (int xx = x - FIELD_SPOT; xx <= x + FIELD_SPOT; xx++)
            if (xx >= 0 && xx < s->w && yy >= 0 && yy < s->h &&
                CELL_TYPE(at(s, xx, yy)) == MOSS) {
                set_wheat(s, xx, yy, 0, 1);
                s->pour_converted++;
            }
    for (int n = 0; n < s->wheat_per_pour; n++) {
        int bx = -1, by = -1, bk = WHEAT_MAX;
        for (int yy = y - FIELD_SPOT; yy <= y + FIELD_SPOT; yy++)
            for (int xx = x - FIELD_SPOT; xx <= x + FIELD_SPOT; xx++) {
                if (xx < 0 || xx >= s->w || yy < 0 || yy >= s->h)
                    continue;
                uint16_t v = at(s, xx, yy);
                if (CELL_TYPE(v) == WHEAT && WHEAT_STOCK(v) < bk) {
                    bk = WHEAT_STOCK(v);
                    bx = xx;
                    by = yy;
                }
            }
        if (bx < 0)
            break;          /* full: the rest of the water is wasted */
        set_wheat(s, bx, by, bk + 1, 1);
        s->pour_levels++;
    }
    s->pours++;
    return 1;
}

/* The field around critter c's home (or around c) is short of what its
 * critters eat in FIELD_RESERVE_HOURS. */
static int field_short(const Sim *s, const Critter *c)
{
    int cx = c->x, cy = c->y, have = 0;
    float per_hour = 60.0f / s->starve_minutes * (c->kind == CR_SMALL ? SMALL_BURN : 1.0f);
    if (c->home >= 0) {
        const Home *h = &s->home[c->home];
        cx = h->x + home_w(h) / 2;
        cy = h->y;
        residents(s, c->home, &per_hour);
    }
    for (int y = cy - SENSE < 0 ? 0 : cy - SENSE; y <= cy + SENSE && y < s->h; y++)
        for (int x = cx - SENSE < 0 ? 0 : cx - SENSE; x <= cx + SENSE && x < s->w; x++)
            if (CELL_TYPE(at(s, x, y)) == WHEAT)
                have += WHEAT_STOCK(at(s, x, y));
    return have < per_hour * FIELD_RESERVE_HOURS / WHEAT_BITE;
}

/* The goal cell is c's own well mouth. */
static int at_mouth(const Sim *s, const Critter *c)
{
    int mx, my;
    if (c->home < 0)
        return 0;
    mouth(&s->home[c->home], &mx, &my);
    return c->tx == mx && c->ty == my;
}

/* Pick up the water at the goal: a puddle cell, or out of the well
 * through its mouth. */
static int fetch_water(Sim *s, Critter *c)
{
    if (at_mouth(s, c))
        return draw_from_well(s, c, &s->home[c->home], 0);
    uint16_t v = at(s, c->tx, c->ty);
    if (CELL_TYPE(v) != WATER)
        return 0;
    c->carry = CELL(WATER, CELL_COLOR(v) & 0x3F);
    set_cell(s, c->tx, c->ty, CELL(EMPTY, 0));
    return 1;
}

/* Farming: fetch a water cell, carry it to the field, pour it. */
static void farm(Sim *s, int i)
{
    Critter *c = &s->crit[i];
    int fx, fy;
    if (!c->carry) {
        if (!fetch_water(s, c)) {
            end_goal(s, c);
            return;
        }
        if (find_best(s, i, rank_pour, &fx, &fy)) {
            set_goal(s, c, G_FARM, fx, fy);
            c->state = CR_CARRY;
        } else {
            end_goal(s, c);         /* think() finds a use for it */
        }
        return;
    }
    if (!c->work_until) {
        c->work_until = s->frame + csecs(s, c, POUR_SECS);
        return;
    }
    if (s->frame < c->work_until)
        return;
    if (pour(s, c->tx, c->ty)) {
        c->carry = 0;
        c->fails = 0;
        c->ax = c->home >= 0 ? c->ax : c->tx;
    }
    end_goal(s, c);
}

/* Stockpiling: carry puddle water to the well and pour it in the mouth. */
static void stock(Sim *s, int i)
{
    Critter *c = &s->crit[i];
    if (!c->carry) {
        if (!fetch_water(s, c)) {
            end_goal(s, c);
            return;
        }
        int mx, my;
        mouth(&s->home[c->home], &mx, &my);
        set_goal(s, c, G_STOCK, mx, my);
        c->state = CR_CARRY;
        return;
    }
    if (CELL_TYPE(at(s, c->tx, c->ty)) == EMPTY) {
        put_down(s, c, c->tx, c->ty);
        c->fails = 0;
        s->stocked++;
    }
    end_goal(s, c);
}

/* Building: mine material, carry it home, lay it in the plan. */
static void build(Sim *s, int i)
{
    Critter *c = &s->crit[i];
    uint16_t v = at(s, c->tx, c->ty);
    int t = CELL_TYPE(v);
    c->state = CR_MINE;
    if (!c->carry) {
        int ok = (t == STONE || t == IRON) ? !(CELL_COLOR(v) & BUILT) : t == DEBRIS;
        if (!ok) {
            end_goal(s, c);
            return;
        }
        if (!c->work_until) {
            float sec = t == IRON ? MINE_IRON_SECS : t == STONE ? MINE_STONE_SECS : PLACE_SECS;
            c->work_until = s->frame + csecs(s, c, sec);
            return;
        }
        if (s->frame < c->work_until)
            return;
        /* Fallen debris is laid as stone. */
        c->carry = CELL(t == IRON ? IRON : STONE, CELL_COLOR(v) & ~BUILT);
        set_cell(s, c->tx, c->ty, CELL(EMPTY, 0));
        end_goal(s, c);             /* think() picks where it goes */
        return;
    }
    c->state = CR_BUILD;
    Home *h = c->home >= 0 ? &s->home[c->home] : NULL;
    if (t != EMPTY || !h || plan_at(h, c->tx, c->ty) != P_WALL ||
        (c->ty == well_top(h) && critter_in(s, h->wx0, h->wy0, h->wx1, h->wy1))) {
        end_goal(s, c);
        return;
    }
    if (!c->work_until) {
        c->work_until = s->frame + csecs(s, c, PLACE_SECS);
        return;
    }
    if (s->frame < c->work_until)
        return;
    set_cell(s, c->tx, c->ty, c->carry | (BUILT << 8));
    c->carry = 0;
    c->fails = 0;
    s->laid++;
    end_goal(s, c);
}

/* Sand to throw: loose sand, open above. */
static int rank_sand(const Sim *s, int x, int y)
{
    int t = CELL_TYPE(at(s, x, y));
    return (t == SAND || t == DEBRIS || t == SHARD) && open_above(s, x, y) ? 0 : -1;
}

/* The natural stone under the loose pile at (x, y), within LEDGE_PILE:
 * the y of its top, or -1. */
static int ledge_under(const Sim *s, int x, int y)
{
    for (int k = 0; k <= LEDGE_PILE && y + k < s->h; k++) {
        uint16_t v = at(s, x, y + k);
        if (CELL_TYPE(v) == STONE && !built(v))
            return y + k;
        if (!FALLS(CELL_TYPE(v)) || MOSSY(CELL_TYPE(v)) || CELL_TYPE(v) == DIRT)
            return -1;
    }
    return -1;
}

static int rank_ledge(const Sim *s, int x, int y)
{
    return rank_sand(s, x, y) == 0 && ledge_under(s, x, y) >= 0 ? 0 : -1;
}

/* Follow the ledge whose top is at (x, y) one way (d) to its end; gives
 * the first column past it and the top row at the end. */
static int ledge_end(const Sim *s, int x, int y, int d, int *ey)
{
    for (;;) {
        int nx = x + d, ny = -1;
        if (nx < 0 || nx >= s->w)
            break;
        for (int yy = y - 1; yy <= y + 1 && ny < 0; yy++)
            if (yy > 0 && yy < s->h && CELL_TYPE(at(s, nx, yy)) == STONE &&
                !built(at(s, nx, yy)) && CELL_TYPE(at(s, nx, yy - 1)) != STONE)
                ny = yy;
        if (ny < 0)
            break;
        x = nx;
        y = ny;
    }
    *ey = y;
    return x + d;
}

/* Scoop the sand at the goal off the ledge and fling it past the nearer
 * end, where it falls. */
static void clear_ledge(Sim *s, int i)
{
    Critter *c = &s->crit[i];
    c->state = CR_DIG;
    if (!c->work_until) {
        c->work_until = s->frame + csecs(s, c, TOSS_SECS);
        return;
    }
    if (s->frame < c->work_until)
        return;
    int top = ledge_under(s, c->tx, c->ty), ly, ry;
    if (top < 0) {
        end_goal(s, c);
        return;
    }
    int lx = ledge_end(s, c->tx, top, -1, &ly), rx = ledge_end(s, c->tx, top, 1, &ry);
    /* The nearer end, unless that's off the screen. */
    int left = rx > s->w - 2 || (lx >= 1 && c->tx - lx <= rx - c->tx);
    int fx = left ? lx - 1 : rx + 1, fy = (left ? ly : ry) - 2;
    int want = c->kind == CR_LARGE ? TOSS_SCOOP : 1, n = 0;
    for (int dy = 0; dy <= 1 && n < want; dy++)
        for (int dx = -1; dx <= 1 && n < want; dx++) {
            int x = c->tx + dx, y = c->ty + dy;
            if (x < 0 || x >= s->w || y >= s->h || rank_ledge(s, x, y) < 0)
                continue;
            /* Where it lands: the first open cell up from past the end. */
            int ty = fy;
            while (ty > 0 && (fx < 0 || fx >= s->w || CELL_TYPE(at(s, fx, ty)) != EMPTY))
                ty--;
            if (ty <= 0 || fx < 0 || fx >= s->w)
                break;
            uint16_t v = at(s, x, y);
            set_cell(s, x, y, CELL(EMPTY, 0));
            set_cell(s, fx, ty, v);
            s->flung++;
            n++;
        }
    if (n)
        c->fails = 0;
    end_goal(s, c);
}

/* Tossing can still take away some of what the emitters poured this
 * stall window. */
static int toss_room(const Sim *s)
{
    return s->toss_window < TOSS_SHARE * s->stall_poured;
}

/* Scoop up the sand at the goal and next to it, and throw it off the
 * nearer side of the screen. */
static void toss(Sim *s, int i)
{
    Critter *c = &s->crit[i];
    c->state = CR_DIG;
    if (!c->work_until) {
        c->work_until = s->frame + csecs(s, c, TOSS_SECS);
        return;
    }
    if (s->frame < c->work_until)
        return;
    int want = c->kind == CR_LARGE ? TOSS_SCOOP : 1, n = 0;
    uint16_t last = 0;
    for (int dy = 0; dy <= 1 && n < want; dy++)
        for (int dx = -1; dx <= 1 && n < want; dx++) {
            int x = c->tx + dx, y = c->ty + dy;
            if (x < 0 || x >= s->w || y >= s->h || !toss_room(s))
                continue;
            int t = CELL_TYPE(at(s, x, y));
            if (t != SAND && t != DEBRIS && t != SHARD)
                continue;
            last = at(s, x, y);
            set_cell(s, x, y, CELL(EMPTY, 0));
            s->tossed++;
            s->toss_window++;
            n++;
        }
    if (n) {
        Toss *t = &s->toss[i];
        t->frame = s->frame;
        t->x = c->x + c->w / 2;
        t->y = c->y - c->h;
        t->dir = c->x < s->w / 2 ? -1 : 1;
        t->cell = last;
        c->fails = 0;
    }
    end_goal(s, c);
}

/* Homes with wheat near them, which hold the fill like a living tree. */
static void count_farms(Sim *s)
{
    int n = 0;
    for (int k = 0; k < MAX_HOMES; k++) {
        const Home *h = &s->home[k];
        float eat;
        if (!h->used || !residents(s, k, &eat))
            continue;
        int found = 0;
        for (int y = h->y - SENSE; y <= h->y + SENSE && !found; y++)
            for (int x = h->x - SENSE; x < h->x + home_w(h) + SENSE && !found; x++)
                found = x >= 0 && x < s->w && y >= 0 && y < s->h &&
                        CELL_TYPE(at(s, x, y)) == WHEAT;
        n += found;
    }
    s->farms = n;
}

/* Swapping: take out a built stone cell and lay the iron it carries in
 * its place. The stone goes down beside it as debris. */
static void swap_in(Sim *s, int i)
{
    Critter *c = &s->crit[i];
    int sx = c->tx, sy = c->ty;
    if (at_mouth(s, c)) {
        /* Down the well on a rope: the first stone in its floor. */
        const Home *h = &s->home[c->home];
        sx = -1;
        for (int x = h->wx0; x <= h->wx1 && sx < 0; x++)
            if (CELL_TYPE(at(s, x, h->y)) == STONE && built(at(s, x, h->y)))
                sx = x;
        sy = h->y;
        if (sx < 0) {
            end_goal(s, c);
            return;
        }
    }
    uint16_t v = at(s, sx, sy);
    if (CELL_TYPE(v) != STONE || !built(v) || CELL_TYPE(c->carry) != IRON) {
        end_goal(s, c);
        return;
    }
    c->state = CR_BUILD;
    if (!c->work_until) {
        c->work_until = s->frame + csecs(s, c, MINE_STONE_SECS);
        return;
    }
    if (s->frame < c->work_until)
        return;
    set_cell(s, sx, sy, c->carry | (BUILT << 8));
    c->carry = CELL(STONE, CELL_COLOR(v) & ~BUILT);
    drop_carry(s, c);
    s->laid++;
    end_goal(s, c);
}

/* Clearing: pick up the junk, then go and dump it. */
static void clear(Sim *s, int i)
{
    Critter *c = &s->crit[i];
    if (at_mouth(s, c)) {
        if (!draw_from_well(s, c, &s->home[c->home], 1)) {
            end_goal(s, c);
            return;
        }
    } else if (built(at(s, c->tx, c->ty))) {
        uint16_t v = at(s, c->tx, c->ty);
        c->state = CR_MINE;
        if (!c->work_until) {
            float sec = CELL_TYPE(v) == IRON ? MINE_IRON_SECS : MINE_STONE_SECS;
            c->work_until = s->frame + csecs(s, c, sec);
            return;
        }
        if (s->frame < c->work_until)
            return;
        c->carry = v & ~(BUILT << 8);
        set_cell(s, c->tx, c->ty, CELL(EMPTY, 0));
    } else {
        uint16_t v = at(s, c->tx, c->ty);
        if (!FALLS(CELL_TYPE(v)) && CELL_TYPE(v) != WATER) {
            end_goal(s, c);
            return;
        }
        c->carry = CELL_TYPE(v) == WATER ? CELL(WATER, CELL_COLOR(v) & 0x3F) : v;
        set_cell(s, c->tx, c->ty, CELL(EMPTY, 0));
    }
    end_goal(s, c);                 /* think() takes it away */
}

/* At the food: take a bite every EAT_SECS until full. */
static void eat(Sim *s, int i)
{
    Critter *c = &s->crit[i];
    uint16_t food = at(s, c->tx, c->ty);
    int t = CELL_TYPE(food);
    if (t != MOSS && !(t == WHEAT && WHEAT_STOCK(food) > 0)) {
        end_goal(s, c);
        return;
    }
    c->state = CR_EAT;
    if (!c->work_until) {
        c->work_until = s->frame + csecs(s, c, EAT_SECS);
        return;
    }
    if (s->frame < c->work_until)
        return;
    if (t == WHEAT) {
        /* Eaten bare, it goes back to moss. */
        int k = WHEAT_STOCK(food) - 1;
        if (k > 0)
            set_wheat(s, c->tx, c->ty, k, 0);
        else
            set_cell(s, c->tx, c->ty, CELL(MOSS, CELL_COLOR(food) & ~WHEAT_MAX));
        s->bites++;
        c->fails = 0;
        c->food += WHEAT_BITE;
        c->ax = c->tx;
    } else {
        set_cell(s, c->tx, c->ty, CELL(WET_DIRT, CELL_COLOR(food)));
        c->fails = 0;
        c->food += MOSS_MEAL;
        c->ax = c->tx;
    }
    if (c->food > 1.0f)
        c->food = 1.0f;
    end_goal(s, c);
}

static int falling_drop(const Sim *s, const Critter *c, int *dx);

/* Something to do with what it's holding: water goes on the field or in
 * the well, stone and iron into the plan, and anything else is dumped
 * away from home. Returns 0 if it put it down instead. */
static int use_carry(Sim *s, int i)
{
    Critter *c = &s->crit[i];
    Home *h = c->home >= 0 ? &s->home[c->home] : NULL;
    int x, y, room;
    int t = CELL_TYPE(c->carry);
    if (t == WATER) {
        if (find_best(s, i, rank_pour, &x, &y) && (field_short(s, c) || !h)) {
            set_goal(s, c, G_FARM, x, y);
            c->state = CR_CARRY;
            return 1;
        }
        if (h && part_done(s, h, 1) && (well_water(s, h, &room), room > 0) &&
            (mouth(h, &x, &y), !avoided(s, c, x, y))) {
            set_goal(s, c, G_STOCK, x, y);
            c->state = CR_CARRY;
            return 1;
        }
        if (find_best(s, i, rank_pour, &x, &y)) {
            set_goal(s, c, G_FARM, x, y);
            c->state = CR_CARRY;
            return 1;
        }
        /* Water and nowhere to put it: plant a seed to pour it on. */
        if (s->frame >= c->plant_at && plant(s, c)) {
            c->plant_at = s->frame + csecs(s, c, PLANT_SECS);
            set_goal(s, c, G_FARM, c->x, c->y + 1);
            c->state = CR_CARRY;
            return 1;
        }
    } else if (t == STONE || t == IRON) {
        if (h && next_build(s, h, &x, &y) && !avoided(s, c, x, y)) {
            set_goal(s, c, G_BUILD, x, y);
            c->state = CR_CARRY;
            return 1;
        }
        if (t == IRON && h && stone_to_swap(s, c, h, &x, &y)) {
            set_goal(s, c, G_SWAP, x, y);
            c->state = CR_CARRY;
            return 1;
        }
        /* Nowhere to lay it just now (a critter standing there, say):
         * hang on to it and try again in a while. Iron is worth keeping
         * until there's stone to swap it for. */
        if (h && (t == IRON || !(part_done(s, h, 1) && part_done(s, h, 0))))
            return 0;
    } else {
        set_goal(s, c, G_DUMP, dump_x(s, c), c->y);
        c->state = CR_CARRY;
        return 1;
    }
    drop_carry(s, c);
    return 0;
}

/* Choose what to do next, in order: eat, clear junk off home, farm,
 * stockpile water, build, idle. A goal in progress is only interrupted
 * for a stroll. */
static void think(Sim *s, int i)
{
    Critter *c = &s->crit[i];
    int fx, fy, room;
    c->next_think = s->frame + csecs(s, c, THINK_SECS);

    /* Down in a well with walls up to its head (it fell in while it was
     * being built): before anything else, climb out over the nearer
     * wall, holding on to what it carries. */
    for (int k = 0; k < MAX_HOMES && c->goal != G_WANDER; k++) {
        const Home *w = &s->home[k];
        if (w->used && c->x <= w->wx1 && c->x + c->w - 1 >= w->wx0 &&
            c->y >= w->wy0 && c->y <= w->wy1 &&
            (built(at(s, w->wx0 - 1, c->y - c->h + 1)) ||
             built(at(s, w->wx1 + 1, c->y - c->h + 1)))) {
            int out = c->x - w->wx0 < w->wx1 - c->x ? w->wx0 - 3 : w->wx1 + 3;
            set_goal(s, c, G_WANDER, out, c->y);
            return;
        }
    }
    if (c->goal != G_NONE && c->goal != G_WANDER)
        return;

    /* Eating comes first, even with its hands full. */
    int hungry = c->food < EAT_BELOW || (c->eating && c->food < FULL_ABOVE);
    c->eating = (uint8_t)hungry;
    if (hungry && (find_best(s, i, rank_food, &fx, &fy) || find_far(s, i, rank_food, &fx, &fy))) {
        set_goal(s, c, G_EAT, fx, fy);
        return;
    }
    if (c->carry && use_carry(s, i))
        return;
    if (c->carry)
        goto stroll;                /* hands full: nothing else for now */

    if (c->home < 0 || !s->home[c->home].used)
        c->home = -1;
    if (c->home < 0)
        settle(s, i);
    Home *h = c->home >= 0 ? &s->home[c->home] : NULL;

    if (h && (next_clear(s, h, &fx, &fy) || roof_junk(s, h, &fx, &fy) ||
              wall_bank(s, h, &fx, &fy) || field_junk(s, h, &fx, &fy)) &&
        !avoided(s, c, fx, fy)) {
        set_goal(s, c, G_CLEAR, fx, fy);
        return;
    }

    /* Nothing growing and nothing to grow it on: plant a seed. */
    if (s->frame >= c->plant_at && !find_best(s, i, rank_earth, &fx, &fy) && plant(s, c)) {
        c->plant_at = s->frame + csecs(s, c, PLANT_SECS);
        return;
    }

    /* Keep the field stocked: puddles first, then the well. */
    int px, py, puddle = find_best(s, i, rank_water, &fx, &fy);
    int well_done = h && part_done(s, h, 1);
    c->thirsty = (uint8_t)((field_short(s, c) && find_best(s, i, rank_pour, &px, &py)) ||
                           (well_done && (well_water(s, h, &room), room > 0)));
    if (field_short(s, c) && find_best(s, i, rank_pour, &px, &py)) {
        if (!puddle && well_done && well_water(s, h, &room) > 0) {
            mouth(h, &fx, &fy);
            puddle = 1;
        }
        if (puddle) {
            set_goal(s, c, G_FARM, fx, fy);
            c->state = CR_FETCH;
            return;
        }
    }

    /* Any open water around goes in the well while there's room. */
    if (puddle && well_done && (well_water(s, h, &room), room > 0)) {
        set_goal(s, c, G_STOCK, fx, fy);
        c->state = CR_FETCH;
        return;
    }

    /* Build: fetch material for the next plan cell, iron if it's a large
     * critter and there's any to be had, or stone. With everything built,
     * large critters go on fetching iron to swap for the stone in it,
     * which also mends rust. */
    int need = h && next_build(s, h, &px, &py);
    int swap = h && !need && stone_to_swap(s, c, h, &px, &py);
    if ((need || swap) && find_iron(s, i, &fx, &fy)) {
        set_goal(s, c, G_BUILD, fx, fy);
        return;
    }
    /* A wide well can use up the stone in sight: then it goes looking. */
    if (need && (find_best(s, i, rank_stone, &fx, &fy) || find_far(s, i, rank_stone, &fx, &fy))) {
        set_goal(s, c, G_BUILD, fx, fy);
        return;
    }

    /* The fill is being held: near an edge, throw sand off the screen
     * while that still slows the fill. */
    int edge = c->x < TOSS_RANGE ? -1 : c->x + c->w > s->w - TOSS_RANGE ? 1 : 0;
    if (s->held && edge && toss_room(s) && find_best(s, i, rank_sand, &fx, &fy)) {
        set_goal(s, c, G_TOSS, fx, fy);
        return;
    }

    /* No home: clear the sand off the nearest ledge. */
    if (!h && find_best(s, i, rank_ledge, &fx, &fy)) {
        set_goal(s, c, G_LEDGE, fx, fy);
        return;
    }

    /* Rain falling nearby: run under it. */
    int dx;
    if (c->thirsty && !c->carry && falling_drop(s, c, &dx)) {
        set_goal(s, c, G_CATCH, dx, c->y);
        return;
    }

    /* Nothing else to do and all built: dig the well a row deeper. */
    if (h && well_done && part_done(s, h, 0) && h->rows < WELL_MAX_ROWS &&
        !next_build(s, h, &px, &py)) {
        h->rows++;
        well_rect(h);
    }

stroll:
    if (c->goal != G_NONE || s->frame < c->work_until)
        return;
    /* Stroll about near where it lives, so it doesn't drift away from
     * its food. */
    int tx = c->ax + (int)(rnd(s) % (2 * WANDER + 1)) - WANDER;
    if (tx < 0) tx = 0;
    if (tx > s->w - c->w) tx = s->w - c->w;
    set_goal(s, c, G_WANDER, tx, c->y);
}

static void free_slot(Sim *s, int i)
{
    memset(&s->crit[i], 0, sizeof s->crit[i]);
    s->critters--;
}

/* Lay the body down as a line of BODY cells, one wider than the critter,
 * along its bottom row where that fits, else in its own column. */
static void start_dying(Sim *s, Critter *c);

static void lay_body(Sim *s, int i)
{
    Critter *c = &s->crit[i];
    int n = c->w + 1, y = c->y;
    drop_carry(s, c);
    for (int yy = c->y - c->h + 1; yy <= c->y; yy++)
        for (int xx = c->x; xx < c->x + c->w; xx++)
            set_cell(s, xx, yy, CELL(EMPTY, 0));
    int x0 = -1;
    for (int k = 0; k < 2 && x0 < 0; k++) {
        int x = k == 0 ? c->x : c->x - 1;
        int ok = x >= 0 && x + n <= s->w;
        for (int xx = x; ok && xx < x + n; xx++)
            ok = CELL_TYPE(at(s, xx, y)) == EMPTY;
        if (ok)
            x0 = x;
    }
    for (int k = 0; k < n; k++) {
        int x = x0 >= 0 ? x0 + k : c->x + k % c->w;
        int yy = x0 >= 0 ? y : y - k / c->w;
        set_cell(s, x, yy, CELL(BODY, rnd(s) & 0x7F));
    }
}

/* Crumpling over: the eye closes and the body goes grey, then it lies
 * down. */
static void die_step(Sim *s, int i)
{
    Critter *c = &s->crit[i];
    c->state = CR_DYING;
    c->spark = 0;
    if (++c->stuck < DIE_MOVES) {
        c->face = 0;
        c->step = 0;
        draw(s, i);
        return;
    }
    lay_body(s, i);
    free_slot(s, i);
}

static void start_dying(Sim *s, Critter *c)
{
    c->goal = G_DIE;
    c->stuck = 0;
    c->next_move = s->frame;
}

static void leave(Sim *s, int i);

/* Grab a drop of water in reach, falling or just landed, outside any
 * well. Checked every step: a falling drop is only in reach for a
 * moment. */
static void catch_drop(Sim *s, int i)
{
    Critter *c = &s->crit[i];
    for (int y = c->y - c->h; y <= c->y + 1; y++)
        for (int x = c->x - 1; x <= c->x + c->w; x++) {
            if (x < 0 || x >= s->w || y < 0 || y >= s->h)
                continue;
            uint16_t v = at(s, x, y);
            if (CELL_TYPE(v) != WATER || in_well(s, x, y))
                continue;
            c->carry = CELL(WATER, CELL_COLOR(v) & 0x3F);
            set_cell(s, x, y, CELL(EMPTY, 0));
            s->caught++;
            if (c->goal == G_CATCH || c->goal == G_WANDER)
                end_goal(s, c);
            return;
        }
}

/* A raindrop falling within CATCH_RANGE columns, above it, that it can
 * sprint under before the drop gets down to its head. Gives the column
 * to stand in to catch the nearest one. */
static int falling_drop(const Sim *s, const Critter *c, int *dx)
{
    int best = CATCH_RANGE + 1, move = csecs(s, c, MOVE_SECS / SPRINT);
    if (move < 1)
        move = 1;
    for (int x = c->x - CATCH_RANGE; x < c->x + c->w + CATCH_RANGE; x++) {
        if (x < 0 || x >= s->w)
            continue;
        for (int y = 0; y < c->y - c->h; y++) {
            uint16_t v = at(s, x, y);
            if (CELL_TYPE(v) == WATER && (CELL_COLOR(v) & RAIN_FRESH)) {
                /* It's in reach one column out from its body. */
                int to = x < c->x - 1 ? x + 1 : x > c->x + c->w ? x - c->w : c->x;
                int d = abs(to - c->x);
                if (d < best && d * move <= c->y - c->h - y) {
                    best = d;
                    *dx = to;
                }
                break;
            }
            if (CELL_TYPE(v) != EMPTY)
                break;
        }
    }
    return best <= CATCH_RANGE;
}

static void critter_tick(Sim *s, int i)
{
    Critter *c = &s->crit[i];

    if (c->goal != G_DIE) {
        float burn = 1.0f / ((float)s->starve_minutes * 60.0f * s->steps_per_sec);
        c->food -= c->kind == CR_SMALL ? burn * SMALL_BURN : burn;
        if (c->food <= 0.0f) {
            c->food = 0.0f;
            start_dying(s, c);
        }
    }
    if (c->thirsty && !c->carry && c->goal != G_DIE)
        catch_drop(s, i);
    if (s->frame < c->next_move)
        return;
    if (c->goal == G_DIE) {
        die_step(s, i);
        if (s->crit[i].state != CR_FREE)
            c->next_move = s->frame + secs(s, MOVE_SECS);
        return;
    }
    /* A charged critter's body crackles every move, even standing still;
     * when the charge runs out it goes back to normal. */
    int spark = charged(s, c) ? !c->spark : 0;
    if (spark != c->spark) {
        c->spark = (uint8_t)spark;
        draw(s, i);
    }
    if (!supported(s, c)) {
        c->state = CR_FALLING;
        if (fits(s, i, c->x, c->y + 1))
            relocate(s, i, c->x, c->y + 1);
        c->next_move = s->frame + FALL_STEPS;
        return;
    }
    if (c->state == CR_FALLING && !c->ax)
        c->ax = c->x;               /* first landing */
    if (c->state == CR_FALLING || c->state == CR_DIG)
        c->state = CR_IDLE;

    /* Covered in sand: wriggle up through it. Buried deep, or stuck
     * under it, it's running out of air. A grain or two on its head it
     * shrugs off as it goes about its business. */
    int any, load = buried_under(s, c, &any);
    if (load >= COVERED) {
        int dug = s->frame >= c->dig_at && dig_up(s, i);
        if (dug)
            c->dig_at = s->frame + csecs(s, c, DIG_SECS);
        if (load > BURY_DEPTH || (!dug && s->frame >= c->dig_at))
            c->buried_steps += (uint32_t)csecs(s, c, MOVE_SECS);
        if (c->buried_steps >= (uint32_t)secs(s, BURY_SECS)) {
            start_dying(s, c);
            return;
        }
        if (load > BURY_DEPTH || !dug) {
            c->next_move = s->frame + csecs(s, c, MOVE_SECS);
            return;
        }
    } else {
        c->buried_steps = 0;
    }

    if (s->frame >= c->next_think)
        think(s, i);
    /* Rain coming down: between other things, it looks every move. */
    int dx;
    if (c->thirsty && !c->carry &&
        (c->goal == G_NONE || c->goal == G_WANDER || c->goal == G_CATCH) &&
        falling_drop(s, c, &dx))
        set_goal(s, c, G_CATCH, dx, c->y);
    c->next_move = s->frame + csecs(s, c, c->goal == G_CATCH ? MOVE_SECS / SPRINT : MOVE_SECS);

    switch (c->goal) {
    case G_CATCH:
    case G_WANDER:
    case G_DETOUR:
        if (c->x == c->tx || !step_side(s, i, c->tx < c->x ? -1 : 1)) {
            if (c->x == c->tx || ++c->stuck >= STUCK_MOVES) {
                end_goal(s, c);
                c->work_until = s->frame + csecs(s, c, IDLE_SECS * (0.5f + rndf(s)));
                c->next_think = s->frame + csecs(s, c, THINK_SECS);
            }
        }
        break;
    case G_EAT:
        if (approach(s, i))
            eat(s, i);
        break;
    case G_FARM:
        if (approach(s, i))
            farm(s, i);
        break;
    case G_STOCK:
        if (approach(s, i))
            stock(s, i);
        break;
    case G_BUILD:
        if (approach(s, i))
            build(s, i);
        break;
    case G_CLEAR:
        if (approach(s, i))
            clear(s, i);
        break;
    case G_SWAP:
        if (approach(s, i))
            swap_in(s, i);
        break;
    case G_TOSS:
        if (approach(s, i))
            toss(s, i);
        break;
    case G_LEDGE:
        if (approach(s, i))
            clear_ledge(s, i);
        break;
    case G_SHELTER:
        /* Into the hut, then wait there for the next fill. Given up on,
         * it waits where it is. */
        if (!in_reach(c, c->tx, c->ty) && !approach(s, i) && c->goal != G_SHELTER)
            set_goal(s, c, G_SHELTER, c->x, c->y);
        c->state = CR_IDLE;
        break;
    case G_LEAVE:
        /* Off the side of the screen, digging through whatever's in the
         * way; there it leaves until the next fill. */
        if (c->tx <= 0 ? c->x <= 0 : c->x + c->w >= s->w)
            leave(s, i);
        else
            step_side(s, i, c->tx <= 0 ? -1 : 1);
        break;
    case G_DUMP:
        /* Far enough out, or as far as it can get: put it down. Here
         * best counts the moves that went nowhere. */
        if (abs(c->x - c->tx) > 1 && ++c->stuck < 3 * SENSE) {
            if (!step_side(s, i, c->tx < c->x ? -1 : 1))
                c->best++;
            if (c->best < STUCK_MOVES)
                break;
        }
        drop_carry(s, c);
        end_goal(s, c);
        break;
    }
}

/* The bolt's path passes through or right next to critter c: walk each
 * line between corners a cell at a time. */
static int on_bolt(const Bolt *b, const Critter *c)
{
    int x0 = c->x - 1, x1 = c->x + c->w, y0 = c->y - c->h, y1 = c->y + 1;
    for (int l = 0; l < b->lines; l++)
        for (int k = b->start[l]; k + 1 < b->start[l + 1]; k++) {
            int dx = b->x[k + 1] - b->x[k], dy = b->y[k + 1] - b->y[k];
            int n = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
            for (int j = 0; j <= n; j++) {
                int x = b->x[k] + (n ? dx * j / n : 0);
                int y = b->y[k] + (n ? dy * j / n : 0);
                if (x >= x0 && x <= x1 && y >= y0 && y <= y1)
                    return 1;
            }
        }
    return 0;
}

void charge_critters(Sim *s)
{
    const Bolt *b = &s->bolt;
    uint32_t until = s->frame + (uint32_t)secs(s, CHARGE_MINUTES * 60.0f);
    for (int i = 0; i < MAX_CRITTERS; i++) {
        Critter *c = &s->crit[i];
        if (c->state == CR_FREE || c->state == CR_AWAY)
            continue;
        /* From the middle of its body. */
        int dx = c->x + c->w / 2 - b->tx, dy = c->y - c->h / 2 - b->ty;
        if (dx * dx + dy * dy > CHARGE_RADIUS * CHARGE_RADIUS && !on_bolt(b, c))
            continue;
        /* A second strike makes it last longer, not go faster. */
        if (until > c->charged_until)
            c->charged_until = until;
        if (c->next_move > s->frame + 1)
            c->next_move = s->frame + 1;
    }
}

/* ---- The drain ---- */

/* Every cell of the hut, roof and floor, is iron: it'll come through. */
static int hut_iron(const Sim *s, const Home *h)
{
    int hx = hut_x(h);
    for (int x = hx; x < hx + HUT_W; x++)
        if (CELL_TYPE(at(s, x, h->y)) != IRON || CELL_TYPE(at(s, x, h->y - 4)) != IRON)
            return 0;
    return 1;
}

/* Off the screen: its body leaves the grid, and it keeps its food and
 * whatever it was carrying until it comes back. */
static void leave(Sim *s, int i)
{
    Critter *c = &s->crit[i];
    for (int y = c->y - c->h + 1; y <= c->y; y++)
        for (int x = c->x; x < c->x + c->w; x++)
            set_cell(s, x, y, CELL(EMPTY, 0));
    c->state = CR_AWAY;
    c->goal = G_NONE;
    c->face = c->x < s->w / 2 ? 1 : -1;     /* the way back in */
}

void critters_drain(Sim *s)
{
    for (int i = 0; i < MAX_CRITTERS; i++) {
        Critter *c = &s->crit[i];
        if (c->state == CR_FREE || c->state == CR_AWAY || c->goal == G_DIE)
            continue;
        const Home *h = c->home >= 0 ? &s->home[c->home] : NULL;
        int on_kept = c->y + 1 < s->h && (s->keep[(c->y + 1) * s->w + c->x] ||
                                          s->keep[(c->y + 1) * s->w + c->x + c->w - 1]);
        if (h && hut_iron(s, h)) {
            set_goal(s, c, G_SHELTER, hut_x(h) + 2, h->y - 1);
            c->state = CR_WALK;
        } else if (on_kept) {
            set_goal(s, c, G_SHELTER, c->x, c->y);
        } else {
            set_goal(s, c, G_LEAVE, c->x < s->w / 2 ? 0 : s->w - c->w, c->y);
        }
    }
}

/* Half or more of a home's plan is still standing. */
static int home_stands(const Sim *s, const Home *h)
{
    int have = 0, want = 0;
    int top = well_top(h) < h->y - 4 ? well_top(h) : h->y - 4;
    for (int y = top; y <= h->y; y++)
        for (int x = h->x; x < h->x + home_w(h); x++)
            if (plan_at(h, x, y) == P_WALL) {
                want++;
                have += built(at(s, x, y));
            }
    return 2 * have >= want;
}

void critters_refill(Sim *s)
{
    /* Anyone still walking off is lifted out now. */
    for (int i = 0; i < MAX_CRITTERS; i++) {
        Critter *c = &s->crit[i];
        if (c->state != CR_FREE && c->state != CR_AWAY && c->goal == G_LEAVE)
            leave(s, i);
    }
    for (int k = 0; k < MAX_HOMES; k++) {
        Home *h = &s->home[k];
        if (!h->used || home_stands(s, h))
            continue;
        memset(h, 0, sizeof *h);
        for (int i = 0; i < MAX_CRITTERS; i++)
            if (s->crit[i].home == k)
                s->crit[i].home = -1;
    }
    /* Back in on the side each left by, dropping from the top somewhere
     * in that half of the screen. */
    for (int i = 0; i < MAX_CRITTERS; i++) {
        Critter *c = &s->crit[i];
        if (c->state == CR_AWAY) {
            c->y = c->h - 1;
            c->x = (int)(rnd(s) % (uint32_t)(s->w / 2 - c->w));
            if (c->face < 0)
                c->x = s->w - c->w - c->x;
            for (int k = 0; k < SENSE && !fits(s, i, c->x, c->y); k++)
                c->x += c->face;
            if (!fits(s, i, c->x, c->y))
                continue;       /* no room: it waits for the next one */
            c->state = CR_FALLING;
            c->goal = G_NONE;
            c->ax = 0;
            c->next_move = s->frame;
            draw(s, i);
        } else if (c->state != CR_FREE && c->goal == G_SHELTER) {
            end_goal(s, c);
        }
    }
}

void update_critters(Sim *s)
{
    if (!s->critters_on)
        return;
    if (s->frame % (uint32_t)secs(s, FARM_COUNT_SECS) == 0)
        count_farms(s);
    for (int i = 0; i < MAX_CRITTERS; i++)
        if (s->crit[i].state != CR_FREE && s->crit[i].state != CR_AWAY)
            critter_tick(s, i);
}

void reset_critters(Sim *s)
{
    memset(s->crit, 0, sizeof s->crit);
    memset(s->home, 0, sizeof s->home);
    memset(s->toss, 0, sizeof s->toss);
    s->farms = 0;
    s->critters = 0;
}

/* Something worth landing near in column x: moss, wheat, or the dirt
 * it grows on, anywhere down it. */
static int good_column(const Sim *s, int x)
{
    for (int y = 0; y < s->h; y++)
        if (EARTH(CELL_TYPE(at(s, x, y))))
            return 1;
    return 0;
}

int sim_add_critter(Sim *s, int kind)
{
    int slot = -1, alive = 0;
    for (int i = 0; i < MAX_CRITTERS; i++) {
        if (s->crit[i].state == CR_FREE) {
            if (slot < 0)
                slot = i;
        } else {
            alive++;
        }
    }
    if (slot < 0 || alive >= s->critter_max)
        return -1;
    Critter *c = &s->crit[slot];
    memset(c, 0, sizeof *c);
    c->kind = kind == CR_ANY ? (int)(rnd(s) & 1) : kind;
    c->w = c->kind == CR_LARGE ? 2 : 1;
    c->h = c->kind == CR_LARGE ? 3 : 2;
    c->y = c->h - 1;

    /* A clear spot along the top, near moss or wheat if there is any. */
    int best = -1;
    for (int tries = 0; tries < 64; tries++) {
        int x = (int)(rnd(s) % (uint32_t)(s->w - c->w + 1));
        if (!fits(s, slot, x, c->y))
            continue;
        if (best < 0)
            best = x;
        if (good_column(s, x)) {
            best = x;
            break;
        }
    }
    if (best < 0) {
        memset(c, 0, sizeof *c);
        return -1;
    }
    c->x = best;
    c->state = CR_FALLING;
    c->face = (rnd(s) & 1) ? 1 : -1;
    c->food = 1.0f;
    c->home = -1;
    c->next_think = s->frame + (uint32_t)slot;
    s->critters++;
    /* A fresh slot can't have stray cells of its own in the grid. */
    draw(s, slot);
    return slot;
}
