#ifndef SIM_INTERNAL_H
#define SIM_INTERNAL_H

/* Helpers shared by sim.c and critter.c. Not part of the public API. */

#include "sim.h"

#include <string.h>

/* A chunk keeps being scanned this many steps after its last change. */
#define WAKE_STEPS 2

/* Moss clings: it only falls when everything in the HOLD_GAP-deep,
 * 3-wide block under it is open. Its roots hold dirt up to ROOT_DEPTH
 * cells below it the same way. */
#define HOLD_GAP   3
#define ROOT_DEPTH 3

/* Water: how far it looks sideways for somewhere lower to flow to. */
#define WATER_SPREAD 4

static inline uint32_t rnd(Sim *s)
{
    /* xorshift32 */
    uint32_t x = s->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return s->rng = x;
}

static inline float rndf(Sim *s)
{
    return (rnd(s) & 0xFFFFFF) / (float)0x1000000;
}

/* Something changed at (x, y): keep its chunk awake and redraw it. A cell
 * near a chunk edge also wakes the neighbor across that edge, since grains
 * there may now be free to move. The band covers water's sideways reach,
 * which is longer than a grain's two-cell slip. Held moss looks HOLD_GAP
 * rows down and roots reach ROOT_DEPTH rows down, so near the top or bottom
 * the chunk above or below is woken too. */
static inline void mark(Sim *s, int x, int y)
{
    int cx = x >> CHUNK_SHIFT, cy = y >> CHUNK_SHIFT;
    int lx = x & (CHUNK - 1), ly = y & (CHUNK - 1);
    int c = cy * s->cw + cx;
    s->changed[c] = 1;
    /* Most cells are well inside their chunk: no neighbors to wake. */
    if (lx >= WATER_SPREAD && lx < CHUNK - WATER_SPREAD &&
        ly >= HOLD_GAP && ly < CHUNK - ROOT_DEPTH) {
        s->awake[c] = WAKE_STEPS;
        return;
    }
    int x0 = cx - (lx < WATER_SPREAD), x1 = cx + (lx >= CHUNK - WATER_SPREAD);
    int y0 = cy - (ly < HOLD_GAP),     y1 = cy + (ly >= CHUNK - ROOT_DEPTH);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= s->cw) x1 = s->cw - 1;
    if (y1 >= s->ch) y1 = s->ch - 1;

    for (int j = y0; j <= y1; j++)
        for (int i = x0; i <= x1; i++)
            s->awake[j * s->cw + i] = WAKE_STEPS;
}

/* Every write that isn't a plain move goes through here, so the grain and
 * solid totals stay right without recounting the grid. */
static inline void set_cell(Sim *s, int x, int y, uint16_t v)
{
    int idx = y * s->w + x;
    uint16_t *cell = &s->cells[idx];
    int old = CELL_TYPE(*cell), new = CELL_TYPE(v);
    s->grains += FALLS(new) - FALLS(old);
    s->solids += SOLID(new) - SOLID(old);
    s->glass  += (new == GLASS) - (old == GLASS);
    s->iron   += (new == IRON) - (old == IRON);
    s->water  += (new == WATER) - (old == WATER);
    s->damp   += DAMP(new) - DAMP(old);
    if (DAMP(new))
        s->since[idx] = s->clock;
    *cell = v;
    /* The kept mask follows the type: a kept cell that turns into
     * something else iron holds (wet dirt drying, moss sprouting a trunk)
     * stays kept, and the kept totals move with it so the end-of-drain
     * check stays right. New iron is always kept. */
    if (s->keep && s->keep[idx]) {
        if (new == IRON || HELD_BY_IRON(new)) {
            s->kept_grains += FALLS(new) - FALLS(old);
            s->kept_solids += SOLID(new) - SOLID(old);
        } else {
            s->keep[idx] = 0;
            s->kept_grains -= FALLS(old);
            s->kept_solids -= SOLID(old);
        }
    } else if (s->keep && new == IRON) {
        s->keep[idx] = 1;
    }
    mark(s, x, y);
}

/* Cells of falling stuff or water stacked on (x, y), up to the first gap
 * or static cell (which carries its own weight). */
static inline int load_on(Sim *s, int x, int y)
{
    int n = 0;
    for (int yy = y - 1; yy >= 0; yy--) {
        int t = CELL_TYPE(s->cells[yy * s->w + x]);
        if (!LOOSE(t))
            break;
        n++;
    }
    return n;
}

/* Move a cell from (x, y) to (nx, y + 1), swapping with what was there
 * (empty, or water that a sinking grain displaces upward). Its drying
 * time goes with it. */
static inline void move(Sim *s, uint16_t *row, uint16_t *next, int x, int nx, int y)
{
    uint16_t c = row[x];
    row[x] = next[nx];
    next[nx] = c;
    /* Only wet dirt and moss read their time, and what they swap with
     * (empty, water, or sand under seeping water) never does. */
    if (DAMP(CELL_TYPE(c)))
        s->since[(y + 1) * s->w + nx] = s->since[y * s->w + x];
    mark(s, x, y);
    mark(s, nx, y + 1);
}

/* Sand and debris sink through water. */
static inline int sand_can_enter(uint16_t c)
{
    int t = CELL_TYPE(c);
    return t == EMPTY || (t == WATER && !(CELL_COLOR(c) & SEEPING));
}

#endif
