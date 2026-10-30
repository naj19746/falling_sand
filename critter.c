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
#define DIG_SECS    1.0f

/* Farming: a pour of water adds wheat_per_pour stock levels to the field
 * within FIELD_SPOT cells of where it lands, turning moss there into
 * wheat first. A bite of wheat is one level, WHEAT_BITE of a meal.
 * Critters keep FIELD_RESERVE_HOURS of what they eat stocked. */
#define FIELD_SPOT  3
#define WHEAT_BITE  (1.0f / 3)
#define FIELD_RESERVE_HOURS 2.0f
#define POUR_SECS   1.0f

enum { G_NONE, G_WANDER, G_EAT, G_DIE, G_FARM };

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
static int supported(const Sim *s, const Critter *c)
{
    if (c->y + 1 >= s->h)
        return 1;
    for (int xx = c->x; xx < c->x + c->w; xx++) {
        int t = CELL_TYPE(at(s, xx, c->y + 1));
        if (t != EMPTY && t != WATER)
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
static int step_side(Sim *s, int i, int d)
{
    Critter *c = &s->crit[i];
    c->face = (int8_t)d;
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
    if (nx >= 0 && nx < s->w)
        for (int y = c->y - c->h; y <= c->y; y++) {
            if (y < 0)
                continue;
            uint16_t v = at(s, nx, y);
            int t = CELL_TYPE(v);
            if (t == EMPTY || t == WATER)
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

/* Drop what it's carrying on the nearest open cell around it (dying, or
 * leaving). It's lost only if it's boxed in. */
static void drop_carry(Sim *s, Critter *c)
{
    if (!c->carry)
        return;
    for (int r = 0; r <= 2; r++)
        for (int y = c->y - c->h - r; y <= c->y + r; y++)
            for (int x = c->x - 1 - r; x <= c->x + c->w + r; x++)
                if (x >= 0 && x < s->w && y >= 0 && y < s->h &&
                    CELL_TYPE(at(s, x, y)) == EMPTY) {
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
    c->best = dist(c, tx, ty);
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

/* Give up on the goal's spot for a while. */
static void fail_goal(Sim *s, Critter *c)
{
    c->avoid_x = c->tx;
    c->avoid_y = c->ty;
    c->avoid_until = s->frame + csecs(s, c, AVOID_SECS);
    end_goal(s, c);
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

/* Food: the fullest wheat first, which keeps a field even, then moss. */
static int rank_food(const Sim *s, int x, int y)
{
    uint16_t v = at(s, x, y);
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
 * take in. */
static int rank_pour(const Sim *s, int x, int y)
{
    uint16_t v = at(s, x, y);
    if (CELL_TYPE(v) == WHEAT) {
        int k = WHEAT_STOCK(v);
        return k >= WHEAT_MAX ? -1 : k < WHEAT_MAX / 2 ? k : 2 * WHEAT_MAX + k;
    }
    if (CELL_TYPE(v) == MOSS)
        return wheat_near(s, x, y, FIELD_SPOT) ? WHEAT_MAX : 2 * WHEAT_MAX;
    return -1;
}

/* Water to carry: open water. */
static int rank_water(const Sim *s, int x, int y)
{
    return CELL_TYPE(at(s, x, y)) == WATER ? 0 : -1;
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
    int moss = 0, room = 0;
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

/* Stock levels in the field around critter c, and how many it wants
 * there: what it eats in FIELD_RESERVE_HOURS. */
static int field_short(const Sim *s, const Critter *c)
{
    int have = 0;
    int x0 = c->x - SENSE, x1 = c->x + SENSE, y0 = c->y - SENSE, y1 = c->y + SENSE;
    for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < s->h; y++)
        for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < s->w; x++)
            if (CELL_TYPE(at(s, x, y)) == WHEAT)
                have += WHEAT_STOCK(at(s, x, y));
    float per_hour = 60.0f / s->starve_minutes * (c->kind == CR_SMALL ? SMALL_BURN : 1.0f);
    float want = per_hour * FIELD_RESERVE_HOURS / WHEAT_BITE;
    return have < want;
}

/* Farming: fetch a water cell, carry it to the field, pour it. */
static void farm(Sim *s, int i)
{
    Critter *c = &s->crit[i];
    int fx, fy;
    if (!c->carry) {
        if (CELL_TYPE(at(s, c->tx, c->ty)) != WATER) {
            end_goal(s, c);
            return;
        }
        c->state = CR_FETCH;
        c->carry = CELL(WATER, CELL_COLOR(at(s, c->tx, c->ty)) & 0x3F);
        set_cell(s, c->tx, c->ty, CELL(EMPTY, 0));
        if (find_best(s, i, rank_pour, &fx, &fy)) {
            set_goal(s, c, G_FARM, fx, fy);
            c->state = CR_CARRY;
        } else {
            drop_carry(s, c);
            end_goal(s, c);
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
        c->ax = c->tx;
        end_goal(s, c);
    } else if (find_best(s, i, rank_pour, &fx, &fy)) {
        set_goal(s, c, G_FARM, fx, fy);     /* somewhere else, then */
        c->state = CR_CARRY;
    } else {
        drop_carry(s, c);
        end_goal(s, c);
    }
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
        c->food += WHEAT_BITE;
        c->ax = c->tx;
    } else {
        set_cell(s, c->tx, c->ty, CELL(WET_DIRT, CELL_COLOR(food)));
        c->food += MOSS_MEAL;
        c->ax = c->tx;
    }
    if (c->food > 1.0f)
        c->food = 1.0f;
    end_goal(s, c);
}

/* Choose what to do next. A goal in progress is only interrupted for a
 * stroll. */
static void think(Sim *s, int i)
{
    Critter *c = &s->crit[i];
    int fx, fy;
    c->next_think = s->frame + csecs(s, c, THINK_SECS);
    if (c->goal != G_NONE && c->goal != G_WANDER)
        return;

    int hungry = c->food < EAT_BELOW || (c->eating && c->food < FULL_ABOVE);
    c->eating = (uint8_t)hungry;
    if (hungry && find_best(s, i, rank_food, &fx, &fy)) {
        set_goal(s, c, G_EAT, fx, fy);
        return;
    }

    /* Still holding water from before: pour it, or put it down. */
    int px, py;
    if (c->carry && CELL_TYPE(c->carry) == WATER) {
        if (find_best(s, i, rank_pour, &px, &py)) {
            set_goal(s, c, G_FARM, px, py);
            c->state = CR_CARRY;
            return;
        }
        drop_carry(s, c);
    }

    /* Keep the field stocked, if there's water to fetch and somewhere
     * to pour it. */
    if (field_short(s, c) && find_best(s, i, rank_pour, &px, &py) &&
        find_best(s, i, rank_water, &fx, &fy)) {
        set_goal(s, c, G_FARM, fx, fy);
        c->state = CR_FETCH;
        return;
    }

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

    /* Sand on its head: dig up through it before anything else. Buried
     * deep, or stuck under it, it's running out of air. */
    int any, load = buried_under(s, c, &any);
    if (any) {
        int dug = s->frame >= c->dig_at && dig_up(s, i);
        if (dug)
            c->dig_at = s->frame + csecs(s, c, DIG_SECS);
        if (load > BURY_DEPTH || (!dug && s->frame >= c->dig_at))
            c->buried_steps += (uint32_t)csecs(s, c, MOVE_SECS);
        if (c->buried_steps >= (uint32_t)secs(s, BURY_SECS)) {
            start_dying(s, c);
            return;
        }
        c->next_move = s->frame + csecs(s, c, MOVE_SECS);
        return;
    }
    c->buried_steps = 0;

    if (s->frame >= c->next_think)
        think(s, i);
    c->next_move = s->frame + csecs(s, c, MOVE_SECS);

    switch (c->goal) {
    case G_WANDER:
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
        if (c->state == CR_FREE)
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

void update_critters(Sim *s)
{
    if (!s->critters_on)
        return;
    for (int i = 0; i < MAX_CRITTERS; i++)
        if (s->crit[i].state != CR_FREE)
            critter_tick(s, i);
}

void reset_critters(Sim *s)
{
    memset(s->crit, 0, sizeof s->crit);
    s->critters = 0;
}

/* Something worth landing near in column x: moss or wheat anywhere down
 * it. */
static int good_column(const Sim *s, int x)
{
    for (int y = 0; y < s->h; y++)
        if (MOSSY(CELL_TYPE(at(s, x, y))))
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
