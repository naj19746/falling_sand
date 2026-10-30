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

/* Moves without getting closer before a critter gives up on its goal. */
#define STUCK_MOVES 6

enum { G_NONE, G_WANDER };

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
        if (c->w == 1)
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

/* One step toward tx: straight across, or up a one-cell step. Returns 1
 * if it moved. Blocked, it turns around. */
static int step_toward(Sim *s, int i)
{
    Critter *c = &s->crit[i];
    int d = c->tx < c->x ? -1 : c->tx > c->x ? 1 : 0;
    if (d == 0)
        return 0;
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
    c->face = (int8_t)-d;
    draw(s, i);
    return 0;
}

/* Choose what to do next. */
static void think(Sim *s, int i)
{
    Critter *c = &s->crit[i];
    c->next_think = s->frame + csecs(s, c, THINK_SECS);
    if (c->goal != G_NONE)
        return;
    if (s->frame < c->work_until)
        return;
    c->goal = G_WANDER;
    c->tx = c->x + (int)(rnd(s) % (2 * WANDER + 1)) - WANDER;
    if (c->tx < 0) c->tx = 0;
    if (c->tx > s->w - c->w) c->tx = s->w - c->w;
    c->ty = c->y;
    c->stuck = 0;
    c->state = CR_WALK;
}

static void critter_tick(Sim *s, int i)
{
    Critter *c = &s->crit[i];

    if (s->frame < c->next_move)
        return;
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
    if (c->state == CR_FALLING)
        c->state = CR_IDLE;

    if (s->frame >= c->next_think)
        think(s, i);

    switch (c->goal) {
    case G_WANDER: {
        int before = abs(c->tx - c->x);
        if (before == 0 || !step_toward(s, i)) {
            if (before == 0 || ++c->stuck >= STUCK_MOVES) {
                c->goal = G_NONE;
                c->state = CR_IDLE;
                c->work_until = s->frame + csecs(s, c, IDLE_SECS * (0.5f + rndf(s)));
            }
        }
        c->next_move = s->frame + csecs(s, c, MOVE_SECS);
        break;
    }
    default:
        c->next_move = s->frame + csecs(s, c, MOVE_SECS);
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
