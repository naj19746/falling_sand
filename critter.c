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

enum { G_NONE, G_WANDER, G_EAT, G_DIE };

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

/* Move one cell sideways (d = -1 or 1): straight across, or up a
 * one-cell step. Returns 1 if it moved. */
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
    draw(s, i);
    return 0;
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

/* Nearest exposed cell of the given type within SENSE. Returns 0 if none. */
static int find_near(Sim *s, int i, int type, int *fx, int *fy)
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
            if (CELL_TYPE(at(s, x, y)) != type)
                continue;
            int d = dist(c, x, y);
            if (d >= best || avoided(s, c, x, y) || !exposed(s, i, x, y))
                continue;
            best = d;
            *fx = x;
            *fy = y;
        }
    return best < (1 << 30);
}

static int find_food(Sim *s, int i, int *fx, int *fy)
{
    return find_near(s, i, MOSS, fx, fy);
}

/* At the food: take a bite every EAT_SECS until full. */
static void eat(Sim *s, int i)
{
    Critter *c = &s->crit[i];
    int t = CELL_TYPE(at(s, c->tx, c->ty));
    if (t != MOSS) {
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
    uint16_t v = at(s, c->tx, c->ty);
    set_cell(s, c->tx, c->ty, CELL(WET_DIRT, CELL_COLOR(v)));
    c->food += MOSS_MEAL;
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
    if (hungry && find_food(s, i, &fx, &fy)) {
        set_goal(s, c, G_EAT, fx, fy);
        return;
    }

    if (c->goal != G_NONE || s->frame < c->work_until)
        return;
    int tx = c->x + (int)(rnd(s) % (2 * WANDER + 1)) - WANDER;
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
static void lay_body(Sim *s, int i)
{
    Critter *c = &s->crit[i];
    int n = c->w + 1, y = c->y;
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
    if (c->state == CR_FALLING)
        c->state = CR_IDLE;

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
