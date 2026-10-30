#include "sim.h"

#include <stdlib.h>
#include <string.h>

#include "sim_internal.h"

/* Drain timing, in simulation steps (~60 per second). */
#define DRAIN_OPEN_STEPS    1200    /* floor gap grows to full width */
#define DRAIN_CRUMBLE_AFTER 600     /* ledges start crumbling */
#define DRAIN_MAX_STEPS     4800    /* give up and clear whatever is left */

/* The most of the screen water may cover before rain stops adding more. */
#define WATER_CAP    0.10f

/* Wet dirt dries after WET_SECS; moss dries back to wet dirt MOSS_SECS
 * after the last rain landed near it. Each row is checked for that once
 * every SWEEP_ROWS steps (about a second). Rain landings are remembered
 * per RAIN_TILE x RAIN_TILE block of cells. */
#define WET_SECS    (10 * 60)
#define MOSS_SECS   (30 * 60)

/* Wheat nobody has poured on loses a level this often, in sim time. */
#define WHEAT_ROT_SECS (3 * 3600)

/* A dead critter's body rots into dirt after this long. */
#define BODY_ROT_SECS (30 * 60)

/* A drop soaking into dirt wets the dirt within WET_SPOT cells of where it
 * landed (and keeps wet dirt there wet); a drop landing on wet dirt grows
 * moss on the wet dirt within MOSS_SPOT. */
#define WET_SPOT    1
#define MOSS_SPOT   2
#define SWEEP_ROWS  60
#define RAIN_SHIFT  2
#define RAIN_TILE   (1 << RAIN_SHIFT)

/* Water resting on sand or moss swaps down through it with this chance
 * per step, out of 1024: about 4 cells a second through sand, 8 through
 * moss at 60 steps/s. */
#define SEEP_SAND   64
#define SEEP_MOSS   128

/* Water on the bottom row drains out of the screen with this chance per
 * step, out of 1024: a floor-level pool 100 cells wide loses about 12
 * cells a second. */
#define FLOOR_DRAIN 2

/* Lightning: random spots tried when looking for something to strike, and
 * the shortest segment a bolt's path is split down to. */
#define STRIKE_TRIES 64
#define BOLT_MIN_SEG 2

/* While a tree holds off the drain, emitters pour at 1/HOLD_POUR_DIV of
 * their usual rate, and every OVERFILL_STEPS the screen is checked against
 * drain_overfill. */
#define HOLD_POUR_DIV 10
#define OVERFILL_STEPS 600

/* Iron overhangs can leave the pile blocking the top row before the screen
 * counts as full. If the emitters are pouring but the grains grow by less
 * than STALL_GAIN of the screen over STALL_STEPS, the fill is as full as it
 * will get: drain. Judged by growth rather than by blocked grains, since
 * rain keeps opening the odd cell along the top. */
#define STALL_STEPS 3600
#define STALL_GAIN  0.0025f

/* Chance per fill, in percent, that about half the iron rusts back to
 * stone, and that all of it does. */
#define RUST_HALF 10
#define RUST_ALL  5

/* Trees: a withering tree browns (or, rescued, greens) one leaf every
 * LEAF_SECS; once all are brown it falls after DEAD_SECS plus up to
 * DEAD_JITTER more without water, taking about FALL_SECS to come down. */
#define LEAF_SECS   10
#define DEAD_SECS   180
#define DEAD_JITTER 120
#define FALL_SECS   300

static int grain_color(Sim *s)
{
    return ((int)s->hue + (int)(rnd(s) % 9) - 4) & 0xFF;
}

static void wake_all(Sim *s)
{
    memset(s->awake, WAKE_STEPS, (size_t)s->cw * s->ch);
    memset(s->changed, 1, (size_t)s->cw * s->ch);
}

static inline int drain_immune(const Sim *s, int x, int y)
{
    return s->keep ? s->keep[y * s->w + x] : 0;
}

/* Mark iron and every earth or tree cell connected to it. Runs once per drain. */
static void mark_kept(Sim *s)
{
    int n = 0, w = s->w;
    memset(s->keep, 0, (size_t)w * s->h);
    for (int i = 0; i < w * s->h; i++)
        if (CELL_TYPE(s->cells[i]) == IRON) {
            s->keep[i] = 1;
            s->stack[n++] = i;
        }
    while (n > 0) {
        int i = s->stack[--n];
        int x = i % w, y = i / w;
        int nb[4] = { x > 0 ? i - 1 : -1, x < w - 1 ? i + 1 : -1,
                      y > 0 ? i - w : -1, y < s->h - 1 ? i + w : -1 };
        for (int k = 0; k < 4; k++) {
            int j = nb[k];
            if (j < 0 || s->keep[j] || !HELD_BY_IRON(CELL_TYPE(s->cells[j])))
                continue;
            s->keep[j] = 1;
            s->stack[n++] = j;
        }
    }
}

static void rain_landed(Sim *s, int x, int y)
{
    s->rain_at[(y >> RAIN_SHIFT) * s->rw + (x >> RAIN_SHIFT)] = s->clock;
}

/* Most recent rain landing within moss_rain_radius of (x, y), judged by
 * each tile's center. */
static uint32_t rain_near(Sim *s, int x, int y)
{
    int r = s->moss_rain_radius;
    int reach = r + RAIN_TILE / 2;
    int tx0 = (x - r) >> RAIN_SHIFT, tx1 = (x + r) >> RAIN_SHIFT;
    int ty0 = (y - r) >> RAIN_SHIFT, ty1 = (y + r) >> RAIN_SHIFT;
    if (tx0 < 0) tx0 = 0;
    if (ty0 < 0) ty0 = 0;
    if (tx1 >= s->rw) tx1 = s->rw - 1;
    if (ty1 >= s->rh) ty1 = s->rh - 1;

    uint32_t latest = 0;
    for (int ty = ty0; ty <= ty1; ty++)
        for (int tx = tx0; tx <= tx1; tx++) {
            int dx = tx * RAIN_TILE + RAIN_TILE / 2 - x;
            int dy = ty * RAIN_TILE + RAIN_TILE / 2 - y;
            uint32_t t = s->rain_at[ty * s->rw + tx];
            if (dx * dx + dy * dy <= reach * reach && t > latest)
                latest = t;
        }
    return latest;
}

/* Visit every SWEEP_ROWS-th row, a different set each step, drying out
 * wet dirt and moss whose time is up and rotting bodies into dirt. Works
 * on sleeping chunks too. */
static void age_damp(Sim *s)
{
    uint32_t now = s->frame / (uint32_t)s->steps_per_sec;
    for (int y = s->frame % SWEEP_ROWS; y < s->h; y += SWEEP_ROWS) {
        uint16_t *row = s->cells + y * s->w;
        uint32_t *since = s->since + y * s->w;
        for (int x = 0; x < s->w; x++) {
            int t = CELL_TYPE(row[x]);
            if (t == WET_DIRT && s->clock - since[x] >= WET_SECS) {
                set_cell(s, x, y, CELL(DIRT, CELL_COLOR(row[x])));
            } else if (t == MOSS && s->clock - since[x] >= MOSS_SECS) {
                uint32_t rained = rain_near(s, x, y);
                if (s->clock - rained < MOSS_SECS)
                    since[x] = rained;      /* check again 30 min after it */
                else
                    set_cell(s, x, y, CELL(WET_DIRT, CELL_COLOR(row[x])));
            } else if (t == WHEAT && now - since[x] >= WHEAT_ROT_SECS) {
                /* Unwatered wheat fades a level at a time, back to moss.
                 * Its time is in sim seconds: it doesn't rot overnight. */
                int k = WHEAT_STOCK(row[x]);
                uint32_t next = since[x] + WHEAT_ROT_SECS;
                if (k > 0)
                    set_cell(s, x, y, CELL(WHEAT, CELL_COLOR(row[x]) - 1));
                else
                    set_cell(s, x, y, CELL(MOSS, CELL_COLOR(row[x]) & ~WHEAT_MAX));
                since[x] = k > 0 ? next : s->clock;
                s->rotted += k > 0;
            } else if (t == BODY && s->clock - since[x] >= BODY_ROT_SECS) {
                set_cell(s, x, y, CELL(DIRT, rnd(s) & 0xFF));
            }
        }
    }
}

/* Glass at (x, y) gave way: it shatters, and the crack runs sideways
 * through neighboring glass carrying more than half the weight it holds. */
static void shatter(Sim *s, int x, int y)
{
    set_cell(s, x, y, CELL(SHARD, CELL_COLOR(s->cells[y * s->w + x])));
    int half = s->glass_strength / 2;
    for (int d = -1; d <= 1; d += 2)
        for (int xx = x + d; xx >= 0 && xx < s->w; xx += d) {
            uint16_t c = s->cells[y * s->w + xx];
            if (CELL_TYPE(c) != GLASS || load_on(s, xx, y) <= half)
                break;
            set_cell(s, xx, y, CELL(SHARD, CELL_COLOR(c)));
        }
}

/* Like age_damp: every SWEEP_ROWS-th row each step, so each glass cell is
 * weighed about once a second, sleeping chunks included. Sand landing
 * high on a pile doesn't wake the chunk the glass is in. */
static void load_glass(Sim *s)
{
    for (int y = s->frame % SWEEP_ROWS; y < s->h; y += SWEEP_ROWS) {
        uint16_t *row = s->cells + y * s->w;
        for (int x = 0; x < s->w; x++)
            if (CELL_TYPE(row[x]) == GLASS && load_on(s, x, y) > s->glass_strength)
                shatter(s, x, y);
    }
}

static void place_ledges(Sim *s)
{
    int count = 3 + rnd(s) % 3;
    for (int i = 0; i < count; i++) {
        int len = s->w / 10 + rnd(s) % (s->w / 6);
        int x0  = rnd(s) % (s->w - len);
        int y0  = s->h / 4 + rnd(s) % (s->h / 2);
        float slope = (rndf(s) - 0.5f) * 0.4f;
        for (int dx = 0; dx < len; dx++) {
            int y = y0 + (int)(dx * slope);
            for (int t = 0; t < 2; t++) {
                int yy = y + t;
                if (yy >= 0 && yy < s->h) {
                    int idx = yy * s->w + (x0 + dx);
                    if (CELL_TYPE(s->cells[idx]) == EMPTY && !(s->keep && s->keep[idx]))
                        set_cell(s, x0 + dx, yy, CELL(STONE, rnd(s) & 0x7F));
                }
            }
        }
    }
}

int sim_init(Sim *s, int w, int h, uint32_t seed, int n_emitters)
{
    memset(s, 0, sizeof *s);
    s->w = w;
    s->h = h;
    s->cw = (w + CHUNK - 1) / CHUNK;
    s->ch = (h + CHUNK - 1) / CHUNK;
    s->rng = seed ? seed : 0x9E3779B9u;
    s->n_emitters = n_emitters < 1 ? 1
                  : n_emitters > MAX_EMITTERS ? MAX_EMITTERS : n_emitters;
    s->fill_reset = 0.6f;
    s->seed_radius = 15;
    s->slip = 307;          /* 0.3 */
    s->moss_rain_radius = 25;
    s->lightning_reach = 60;
    s->strike_radius = 7;
    s->glass_strength = 35;
    s->tree_minutes = 30;
    s->tree_thirst_minutes = 22;
    s->drain_overfill = 0.8f;
    s->steps_per_sec = 60;
    s->critters_on = 1;
    s->critter_max = 24;
    s->starve_minutes = 40;
    s->wheat_per_pour = 3;
    s->rw = (w + RAIN_TILE - 1) / RAIN_TILE;
    s->rh = (h + RAIN_TILE - 1) / RAIN_TILE;
    s->cells   = calloc((size_t)w * h, sizeof *s->cells);
    s->awake   = calloc((size_t)s->cw * s->ch, 1);
    s->changed = calloc((size_t)s->cw * s->ch, 1);
    s->moved   = calloc((size_t)w * h, 1);
    s->since   = calloc((size_t)w * h, sizeof *s->since);
    s->rain_at = calloc((size_t)s->rw * s->rh, sizeof *s->rain_at);
    s->keep    = calloc((size_t)w * h, 1);
    s->stack   = malloc((size_t)w * h * sizeof *s->stack);
    if (!s->cells || !s->awake || !s->changed || !s->moved || !s->since || !s->rain_at || !s->keep || !s->stack)
        return -1;
    s->bl.x = w * 0.5f;
    s->bl.y = h - 1;
    sim_reset(s);
    return 0;
}

void sim_free(Sim *s)
{
    free(s->cells);
    free(s->awake);
    free(s->changed);
    free(s->moved);
    free(s->since);
    free(s->rain_at);
    free(s->keep);
    free(s->stack);
    s->moved = NULL;
    s->since = s->rain_at = NULL;
    s->cells = NULL;
    s->awake = s->changed = NULL;
}

void sim_reset(Sim *s)
{
    memset(s->cells, 0, (size_t)s->w * s->h * sizeof *s->cells);
    if (s->keep) memset(s->keep, 0, (size_t)s->w * s->h);
    s->grains = s->solids = s->water = s->damp = s->glass = s->iron = 0;
    s->wheat = s->bodies = 0;
    s->kept_grains = s->kept_solids = 0;
    memset(s->tree, 0, sizeof s->tree);
    s->trees = 0;
    reset_critters(s);
    wake_all(s);
    for (int i = 0; i < s->n_emitters; i++) {
        s->em[i].burst_left = 0;
        /* Stagger the first bursts so emitters don't start in unison. */
        s->em[i].idle_left = i * 60 + rnd(s) % 60;
    }
    s->phase = PHASE_FILL;
    s->drain_steps = 0;
    s->stall_steps = s->stall_tried = s->stall_grains = s->stall_poured = s->toss_window = 0;
    s->held = 0;
    place_ledges(s);
}

/* Iron rusts back to stone now and then, so it can't build up until the
 * sand has nowhere to go: at the start of a fill, sometimes about half of
 * it goes, and rarely all of it. Each connected piece rusts whole or not
 * at all, so what stays is intact. Uses keep as the visited mask; the
 * caller clears it afterwards. */
static void rust_iron(Sim *s)
{
    uint32_t roll = rnd(s) % 100;
    if (roll >= RUST_HALF + RUST_ALL)
        return;
    int all = roll < RUST_ALL, w = s->w;
    memset(s->keep, 0, (size_t)w * s->h);
    for (int i = 0; i < w * s->h; i++) {
        if (CELL_TYPE(s->cells[i]) != IRON || s->keep[i])
            continue;
        /* Breadth-first, so when it's done stack[0..tail) is the piece. */
        int head = 0, tail = 0;
        s->keep[i] = 1;
        s->stack[tail++] = i;
        while (head < tail) {
            int j = s->stack[head++];
            int x = j % w, y = j / w;
            int nb[4] = { x > 0 ? j - 1 : -1, x < w - 1 ? j + 1 : -1,
                          y > 0 ? j - w : -1, y < s->h - 1 ? j + w : -1 };
            for (int k = 0; k < 4; k++) {
                int n = nb[k];
                if (n < 0 || s->keep[n] || CELL_TYPE(s->cells[n]) != IRON)
                    continue;
                s->keep[n] = 1;
                s->stack[tail++] = n;
            }
        }
        if (!all && (rnd(s) & 1))
            continue;
        for (int k = 0; k < tail; k++) {
            int j = s->stack[k];
            set_cell(s, j % w, j / w, CELL(STONE, CELL_COLOR(s->cells[j])));
        }
    }
}

/* End of a drain: clear everything but iron and what it holds up. */
static void clear_unkept(Sim *s)
{
    for (int i = 0; i < s->w * s->h; i++) {
        if (s->keep && s->keep[i])
            continue;
        int t = CELL_TYPE(s->cells[i]);
        if (t == WATER && well_holds(s, i % s->w, i / s->w))
            continue;
        if (t != EMPTY && t != CRITTER)
            set_cell(s, i % s->w, i / s->w, CELL(EMPTY, 0));
    }
    if (s->critters)
        critters_refill(s);
    for (int i = 0; i < s->n_emitters; i++) {
        s->em[i].burst_left = 0;
        s->em[i].idle_left = i * 60 + (int)(rnd(s) % 60);
    }
    s->phase = PHASE_FILL;
    s->drain_steps = 0;
    rust_iron(s);
    /* Kept dirt can fall during the fill, and move() doesn't carry the
     * mask, so drop it now; the next drain marks the kept cells again. */
    memset(s->keep, 0, (size_t)s->w * s->h);
    s->kept_grains = 0;
    s->kept_solids = 0;
    s->stall_steps = s->stall_tried = s->stall_poured = s->toss_window = 0;
    s->stall_grains = s->grains;
    s->held = 0;
    wake_all(s);
    place_ledges(s);
}

void sim_start_drain(Sim *s)
{
    if (s->phase == PHASE_DRAIN)
        return;
    s->phase = PHASE_DRAIN;
    s->drain_steps = 0;
    s->held = 0;
    mark_kept(s);
    if (s->critters)
        critters_drain(s);
    /* Keep trees whose base cell is kept; drop the rest. */
    int nt = 0;
    for (int i = 0; i < MAX_TREES; i++) {
        if (s->tree[i].stage == TREE_FREE)
            continue;
        int idx = s->tree[i].y * s->w + s->tree[i].x;
        if (s->keep[idx]) {
            if (nt != i)
                s->tree[nt] = s->tree[i];
            nt++;
        } else
            memset(&s->tree[i], 0, sizeof s->tree[i]);
    }
    for (int i = nt; i < MAX_TREES; i++)
        memset(&s->tree[i], 0, sizeof s->tree[i]);
    s->trees = nt;
    /* Count kept cells so the end-of-drain check can ignore them. */
    s->kept_grains = 0;
    s->kept_solids = 0;
    for (int i = 0; i < s->w * s->h; i++)
        if (s->keep[i]) {
            int t = CELL_TYPE(s->cells[i]);
            if (FALLS(t)) s->kept_grains++;
            if (SOLID(t)) s->kept_solids++;
        }
}

int sim_awake_chunks(const Sim *s)
{
    int n = 0;
    for (int i = 0; i < s->cw * s->ch; i++)
        n += s->awake[i] > 0;
    return n;
}

static void start_burst(Sim *s, Emitter *e)
{
    e->x           = 2 + rndf(s) * (s->w - 5);
    e->v           = (rndf(s) - 0.5f) * 0.6f;
    e->burst_left  = 20 + rnd(s) % 100;
    e->burst_width = 1 + rnd(s) % 5;
    e->burst_rate  = 1 + rnd(s) % 5;
    e->idle_left   = 10 + rnd(s) % 120;   /* pause after this burst */
}

/* Random walk with a speed cap, bouncing off the side edges. */
static void wander(Sim *s, float *x, float *v, float jitter, float vmax,
                   float speed)
{
    *v += (rndf(s) - 0.5f) * jitter;
    if (*v >  vmax) *v =  vmax;
    if (*v < -vmax) *v = -vmax;
    *x += *v * speed;
    if (*x < 2)          { *x = 2;          *v = -*v; }
    if (*x > s->w - 3)   { *x = s->w - 3;   *v = -*v; }
}

/* Pours 1 in div of the grains it otherwise would. */
/* Adds how many grains it tried to pour (not counting ones skipped by the
 * trickle) to *tried. */
static void emit(Sim *s, Emitter *e, int div, int *tried)
{
    if (e->burst_left <= 0) {
        if (e->idle_left > 0) {
            e->idle_left--;
            return;
        }
        start_burst(s, e);
    }
    e->burst_left--;

    wander(s, &e->x, &e->v, 0.05f, 0.6f, 1.0f);

    int span = 2 * e->burst_width + 1;
    for (int i = 0; i < e->burst_rate; i++) {
        int x = (int)e->x + (int)(rnd(s) % span) - e->burst_width;
        if (div > 1 && rnd(s) % div)
            continue;
        if (x < 0 || x >= s->w)
            continue;
        (*tried)++;
        if (CELL_TYPE(s->cells[x]) == EMPTY) {
            set_cell(s, x, 0, CELL(SAND, grain_color(s)));
            s->stall_poured++;
        }
    }
}

/* Top of whatever is in column x, or the floor. */
static int surface_y(Sim *s, int x)
{
    for (int y = 0; y < s->h; y++)
        if (CELL_TYPE(s->cells[y * s->w + x]) != EMPTY)
            return y;
    return s->h - 1;
}

/* Drift along the pile surface, faster the louder it is. Lightning comes
 * from here, so it keeps moving without audio too. */
static void blaster_move(Sim *s)
{
    Blaster *b = &s->bl;
    wander(s, &b->x, &b->v, 0.08f, 1.0f, 0.3f + b->level * 2.5f);
    int target = surface_y(s, (int)b->x);
    b->y += (target - b->y) * 0.08f;
}

static void blast(Sim *s)
{
    Blaster *b = &s->bl;
    float lvl = b->level;

    /* Louder = bigger. */
    b->radius = lvl * s->h / 6.0f;
    if (b->radius < 1.5f)
        return;

    int r  = (int)b->radius;
    int cx = (int)b->x, cy = (int)b->y;
    /* Probability out of 1024 that a grain in range is hit this step. */
    uint32_t hit = (uint32_t)(lvl * lvl * 1024);

    for (int y = cy - r; y <= cy + r; y++) {
        if (y < 0 || y >= s->h)
            continue;
        for (int x = cx - r; x <= cx + r; x++) {
            if (x < 0 || x >= s->w)
                continue;
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy > r * r)
                continue;
            uint16_t grain = s->cells[y * s->w + x];
            int t = CELL_TYPE(grain);
            if (!LOOSE(t) || EARTH(t) || (rnd(s) & 1023) >= hit)
                continue;

            /* A quarter get flung upward as spray; the rest vanish. */
            set_cell(s, x, y, CELL(EMPTY, 0));
            if ((rnd(s) & 3) == 0) {
                int fy = y - r - (int)(rnd(s) % (2 * r + 1));
                int fx = x + (int)(rnd(s) % (2 * r + 1)) - r;
                if (fy >= 0 && fx >= 0 && fx < s->w &&
                    CELL_TYPE(s->cells[fy * s->w + fx]) == EMPTY)
                    set_cell(s, fx, fy, grain);
            }
        }
    }
}

static void crumble(Sim *s)
{
    /* The chance per step ramps up so ledges go gradually, then quickly.
     * Scans the whole grid, but only for the ~30 s this lasts per cycle. */
    uint32_t chance = 1 + (s->drain_steps - DRAIN_CRUMBLE_AFTER) / 40;
    for (int y = 0; y < s->h; y++) {
        uint16_t *row = s->cells + y * s->w;
        for (int x = 0; x < s->w; x++) {
            int t = CELL_TYPE(row[x]);
            if (!SOLID(t) || (rnd(s) & 4095) >= chance || s->keep[y * s->w + x])
                continue;
            /* Leaves vanish; wood and roots break up into brown dirt. */
            if (t == LEAF)
                set_cell(s, x, y, CELL(EMPTY, 0));
            else if (TREE(t))
                set_cell(s, x, y, CELL(DIRT, CELL_COLOR(row[x])));
            else
                set_cell(s, x, y, CELL(t == GLASS ? SHARD : DEBRIS, CELL_COLOR(row[x]) & ~BUILT));
        }
    }
}

static void rain(Sim *s)
{
    s->water_acc += s->water_rate;
    int cap = (int)(WATER_CAP * s->w * s->h);
    while (s->water_acc >= 1.0f) {
        s->water_acc -= 1.0f;
        int x = rnd(s) % s->w;
        if (s->water < cap && CELL_TYPE(s->cells[x]) == EMPTY)
            set_cell(s, x, 0, CELL(WATER, RAIN_FRESH | (rnd(s) & 0x7F)));
    }
}

/* Spot-check random cells; water open to the air above evaporates. Random
 * sampling costs the same however much water there is, and surface area
 * sets the rate: puddles shrink, deep pools last. */
static void evaporate(Sim *s)
{
    int samples = s->w * s->h / 4096 + 1;
    uint32_t chance = s->raining ? 64 : 512;        /* out of 1024 */
    for (int i = 0; i < samples; i++) {
        int idx = (int)(rnd(s) % (uint32_t)(s->w * s->h));
        if (CELL_TYPE(s->cells[idx]) != WATER)
            continue;
        int x = idx % s->w, y = idx / s->w;
        if (y > 0 && CELL_TYPE(s->cells[idx - s->w]) != EMPTY)
            continue;
        if (in_well(s, x, y))
            continue;
        if ((rnd(s) & 1023) < chance)
            set_cell(s, x, y, CELL(EMPTY, 0));
    }
}

/* Water on the floor slowly drains away; what's above falls into the gap. */
static void drain_floor(Sim *s)
{
    int y = s->h - 1;
    uint16_t *floor = s->cells + y * s->w;
    for (int x = 0; x < s->w; x++)
        if (CELL_TYPE(floor[x]) == WATER && (rnd(s) & 1023) < FLOOR_DRAIN)
            set_cell(s, x, y, CELL(EMPTY, 0));
}

/* Half the width of the gap in the floor, centered, widening until the
 * whole floor is open. */
static int floor_gap(const Sim *s)
{
    int half = s->w / 20 + (int)((long)s->drain_steps * s->w / (2 * DRAIN_OPEN_STEPS));
    return half > s->w / 2 + 1 ? s->w / 2 + 1 : half;
}

static void drain(Sim *s)
{
    s->drain_steps++;

    int half = floor_gap(s);
    int y = s->h - 1;
    uint16_t *floor = s->cells + y * s->w;
    for (int x = s->w / 2 - half; x < s->w / 2 + half; x++)
        if (x >= 0 && x < s->w && LOOSE(CELL_TYPE(floor[x])) && !drain_immune(s, x, y))
            set_cell(s, x, y, CELL(EMPTY, 0));

    if (s->drain_steps > DRAIN_CRUMBLE_AFTER)
        crumble(s);

    int done = s->drain_steps > DRAIN_OPEN_STEPS &&
               s->grains == s->kept_grains && s->solids == s->kept_solids;
    if (done || s->drain_steps > DRAIN_MAX_STEPS)
        clear_unkept(s);
}

/* Through iron and what it holds, per step out of 1024. Ramps up over
 * the drain so the rest of the pile sifts out as the floor opens. */
static inline uint32_t phase_chance(const Sim *s)
{
    int c = 8 + (int)s->drain_steps / 20;
    if (c > 512) c = 512;
    return (uint32_t)c;
}

/* Keep a chunk scanned without redrawing it. */
static inline void stay_awake(Sim *s, int x, int y)
{
    s->awake[(y >> CHUNK_SHIFT) * s->cw + (x >> CHUNK_SHIFT)] = WAKE_STEPS;
}

/* A drop soaked into the dirt at (cx, cy): wet the dirt around it, or if it
 * was already wet there, grow moss on the wet dirt around it. */
static void soak(Sim *s, int cx, int cy, int dry)
{
    int r = dry ? WET_SPOT : MOSS_SPOT;
    for (int y = cy - r; y <= cy + r; y++) {
        if (y < 0 || y >= s->h)
            continue;
        for (int x = cx - r; x <= cx + r; x++) {
            int dx = x - cx, dy = y - cy;
            if (x < 0 || x >= s->w || dx * dx + dy * dy > r * r)
                continue;
            int idx = y * s->w + x;
            uint16_t c = s->cells[idx];
            if (dry && CELL_TYPE(c) == DIRT)
                set_cell(s, x, y, CELL(WET_DIRT, CELL_COLOR(c)));
            else if (dry && CELL_TYPE(c) == WET_DIRT)
                s->since[idx] = s->clock;       /* stays wet longer */
            else if (!dry && CELL_TYPE(c) == WET_DIRT)
                set_cell(s, x, y, CELL(MOSS, CELL_COLOR(c)));
        }
    }
}

/* The tree that (x, y) belongs to has had water reach it. */
static void water_tree_at(Sim *s, int x, int y)
{
    for (int i = 0; i < MAX_TREES; i++) {
        Tree *t = &s->tree[i];
        if (t->stage == TREE_FREE || t->stage == TREE_FALLING)
            continue;
        for (int k = 0; k < t->n; k++)
            if (t->cx[k] == x && t->cy[k] == y) {
                t->watered_at = s->clock;
                return;
            }
    }
}

/* A drop on moss, leaves or wood at (x, y) soaks straight through, without
 * shifting anything, to whatever is under it: dirt takes it up, a root
 * drinks it, an open space gets it as a drip, and anything else just
 * absorbs it. */
static void through_moss(Sim *s, int x, int y)
{
    uint16_t drop = s->cells[y * s->w + x];
    set_cell(s, x, y, CELL(EMPTY, 0));
    int yy = y + 1;
    for (; yy < s->h; yy++) {
        int t = CELL_TYPE(s->cells[yy * s->w + x]);
        if (!MOSSY(t) && t != LEAF && t != WOOD)
            break;
    }
    if (yy >= s->h)
        return;
    int t = CELL_TYPE(s->cells[yy * s->w + x]);
    if (t == DIRT || t == WET_DIRT)
        soak(s, x, yy, t == DIRT);
    else if (t == ROOT)
        water_tree_at(s, x, yy);
    else if (t == EMPTY)
        set_cell(s, x, yy, CELL(WATER, CELL_COLOR(drop) & ~(SEEPING | RAIN_FRESH)));
}

/* A raindrop at (x, y) with something under it (or the floor) has landed;
 * from then on it's just water. */
static inline void land(Sim *s, uint16_t *cell, int x, int y)
{
    if (CELL_COLOR(*cell) & RAIN_FRESH) {
        *cell &= ~(RAIN_FRESH << 8);
        rain_landed(s, x, y);
    }
}

/* During the drain, a grain or drop stuck on iron or what it holds hops
 * down its column past the kept run to the first open cell, or out through
 * the floor gap if the run reaches the floor. It's a swap, so the totals
 * don't move. Until it gets through it keeps its chunk awake: a sleeping
 * chunk would never roll again, and the drain would wait on it. */
static void phase_through(Sim *s, uint16_t *row, int x, int y)
{
    int w = s->w;
    if (s->phase != PHASE_DRAIN || y + 1 >= s->h || !s->keep[(y + 1) * w + x])
        return;
    if (CELL_TYPE(row[x]) == WATER && well_holds(s, x, y))
        return;                             /* it stays in the well */
    stay_awake(s, x, y);
    if ((rnd(s) & 1023) >= phase_chance(s))
        return;
    int ny = y + 2;
    while (ny < s->h && s->keep[ny * w + x])
        ny++;
    if (ny >= s->h) {
        int half = floor_gap(s);
        if (x >= w / 2 - half && x < w / 2 + half)
            set_cell(s, x, y, CELL(EMPTY, 0));
        return;
    }
    int j = ny * w + x;
    if (!sand_can_enter(s->cells[j]))
        return;
    uint16_t c = row[x];
    row[x] = s->cells[j];
    s->cells[j] = c;
    if (AGES(CELL_TYPE(c)))
        s->since[j] = s->since[y * w + x];
    if (CELL_TYPE(c) == WATER) {
        s->moved[j] = (uint8_t)s->frame;
        if (ny == s->h - 1)
            land(s, &s->cells[j], x, ny);
    }
    mark(s, x, y);
    mark(s, x, ny);
}

static void update_water(Sim *s, uint16_t *row, uint16_t *next, int x, int y)
{
    int w = s->w;
    uint8_t stamp = (uint8_t)s->frame;
    /* Already slid this step. The stamp is per position and wraps every 256
     * steps, so a cell that just arrived here can match by accident; keep
     * the chunk awake so that costs one step instead of freezing it. */
    if (s->moved[y * w + x] == stamp) {
        mark(s, x, y);
        return;
    }

    int below = CELL_TYPE(next[x]);
    if (below != EMPTY)
        land(s, &row[x], x, y);

    /* A tree lets drops through its canopy and trunk; its roots drink. */
    if (below == LEAF || below == WOOD) {
        through_moss(s, x, y);
        return;
    }
    if (below == ROOT) {
        water_tree_at(s, x, y + 1);
        set_cell(s, x, y, CELL(EMPTY, 0));
        return;
    }

    /* Dirt below soaks the drop up: dry dirt gets wet, wet dirt grows moss. */
    if (below == DIRT || below == WET_DIRT) {
        soak(s, x, y + 1, below == DIRT);
        set_cell(s, x, y, CELL(EMPTY, 0));
        return;
    }

    /* Seep slowly down through sand and moss. Water on top keeps its chunk
     * awake until it has soaked in; water that fails the roll this step
     * still spreads as usual if it's on the surface. */
    int seeping = CELL_COLOR(row[x]) & SEEPING;
    if (below == SAND || MOSSY(below)) {
        uint32_t chance = below == SAND ? SEEP_SAND : SEEP_MOSS;
        if ((rnd(s) & 1023) < chance) {
            if (MOSSY(below)) {
                through_moss(s, x, y);
            } else if (y + 1 == s->h - 1) {     /* into the ground */
                set_cell(s, x, y, CELL(EMPTY, 0));
            } else {
                row[x] |= SEEPING << 8;
                move(s, row, next, x, x, y);
            }
            return;
        }
        mark(s, x, y);
        if (seeping)
            return;
    } else if (seeping && below == WATER && (CELL_COLOR(next[x]) & SEEPING)) {
        mark(s, x, y);                      /* queued behind another drop */
        return;
    } else if (seeping) {
        /* Out the bottom of the pile into open air or a pool, it's plain
         * water again; stopped by stone or the floor, it soaks away. */
        if (below != EMPTY && below != WATER) {
            set_cell(s, x, y, CELL(EMPTY, 0));
            return;
        }
        row[x] &= ~(SEEPING << 8);
    }

    /* The bottom row is never scanned, so a drop reaching it lands now. */
    if (below == EMPTY) {
        move(s, row, next, x, x, y);
        if (y + 1 == s->h - 1)
            land(s, &next[x], x, y + 1);
        return;
    }
    int d = (rnd(s) & 1) ? 1 : -1;
    for (int k = 0; k < 2; k++, d = -d) {
        int nx = x + d;
        if (nx >= 0 && nx < w && CELL_TYPE(next[nx]) == EMPTY) {
            move(s, row, next, x, nx, y);
            if (y + 1 == s->h - 1)
                land(s, &next[nx], nx, y + 1);
            return;
        }
    }

    /* Sideways, toward the nearest spot within reach that has room below.
     * With nowhere lower to go, only water pressing down from above makes
     * it spread; otherwise a lone drop on a flat floor would wander forever
     * and keep its chunk awake. */
    int target = -1;
    for (int k = 0; k < 2 && target < 0; k++, d = -d) {
        for (int i = 1; i <= WATER_SPREAD; i++) {
            int nx = x + i * d;
            if (nx < 0 || nx >= w || CELL_TYPE(row[nx]) != EMPTY)
                break;
            if (CELL_TYPE(next[nx]) == EMPTY) {
                target = nx;
                break;
            }
        }
    }
    if (target < 0 && y > 0 && CELL_TYPE(row[x - w]) == WATER) {
        for (int k = 0; k < 2 && target < 0; k++, d = -d)
            if (x + d >= 0 && x + d < w && CELL_TYPE(row[x + d]) == EMPTY)
                target = x + d;
    }
    if (target < 0) {
        phase_through(s, row, x, y);
        return;
    }

    row[target] = row[x];
    row[x] = CELL(EMPTY, 0);
    s->moved[y * w + target] = stamp;
    mark(s, x, y);
    mark(s, target, y);
}

/* Down, else diagonally down. Returns the new x, or -1 if stuck. */
static int fall(Sim *s, uint16_t *row, uint16_t *next, int x, int y)
{
    if (sand_can_enter(next[x])) {
        move(s, row, next, x, x, y);
        return x;
    }
    int d = (rnd(s) & 1) ? 1 : -1;
    for (int k = 0; k < 2; k++, d = -d) {
        int nx = x + d;
        if (nx >= 0 && nx < s->w && sand_can_enter(next[nx])) {
            move(s, row, next, x, nx, y);
            return nx;
        }
    }
    /* Slippery: sometimes slide two over and one down, past a clear cell,
     * so piles settle flatter than 45 degrees. */
    if ((rnd(s) & 1023) < s->slip) {
        for (int k = 0; k < 2; k++, d = -d) {
            int nx = x + 2 * d;
            if (nx >= 0 && nx < s->w && CELL_TYPE(row[x + d]) == EMPTY &&
                sand_can_enter(next[nx])) {
                move(s, row, next, x, nx, y);
                return nx;
            }
        }
    }
    return -1;
}

/* Turn the sand in a circle around (cx, cy) into dirt. */
static void sprout(Sim *s, int cx, int cy)
{
    int r = s->seed_radius;
    for (int y = cy - r; y <= cy + r; y++) {
        if (y < 0 || y >= s->h)
            continue;
        for (int x = cx - r; x <= cx + r; x++) {
            int dx = x - cx, dy = y - cy;
            if (x < 0 || x >= s->w || dx * dx + dy * dy > r * r)
                continue;
            if (CELL_TYPE(s->cells[y * s->w + x]) == SAND)
                set_cell(s, x, y, CELL(DIRT, rnd(s) & 0xFF));
        }
    }
}

static void update_seed(Sim *s, uint16_t *row, uint16_t *next, int x, int y)
{
    if (CELL_TYPE(next[x]) == SAND) {
        sprout(s, x, y + 1);
        set_cell(s, x, y, CELL(DIRT, rnd(s) & 0xFF));
        return;
    }
    int nx = fall(s, row, next, x, y);
    if (nx < 0)                             /* landed on something else */
        set_cell(s, x, y, CELL(DIRT, rnd(s) & 0xFF));
    else if (y + 1 == s->h - 1)             /* the bottom row isn't scanned */
        set_cell(s, nx, y + 1, CELL(DIRT, rnd(s) & 0xFF));
}

/* Dirt or wet dirt with moss at most ROOT_DEPTH cells above it, through
 * unbroken dirt, is held by the moss's roots; dirt next to a tree root is
 * held by the tree. */
static int rooted(Sim *s, int x, int y)
{
    const uint16_t *c = s->cells + y * s->w + x;
    if ((x > 0 && CELL_TYPE(c[-1]) == ROOT) ||
        (x < s->w - 1 && CELL_TYPE(c[1]) == ROOT) ||
        (y > 0 && CELL_TYPE(c[-s->w]) == ROOT) ||
        (y < s->h - 1 && CELL_TYPE(c[s->w]) == ROOT))
        return 1;
    for (int i = 1; i <= ROOT_DEPTH && y - i >= 0; i++) {
        int t = CELL_TYPE(s->cells[(y - i) * s->w + x]);
        if (MOSSY(t))
            return 1;
        if (t != DIRT && t != WET_DIRT)
            return 0;
    }
    return 0;
}

/* Everything in the block under (x, y) is open (empty, or water a grain
 * could sink through). Past the edges and the floor counts as open, so held
 * cells still drain out of the bottom. */
static int open_below(Sim *s, int x, int y)
{
    for (int yy = y + 1; yy <= y + HOLD_GAP && yy < s->h; yy++)
        for (int xx = x - 1; xx <= x + 1; xx++)
            if (xx >= 0 && xx < s->w && !sand_can_enter(s->cells[yy * s->w + xx]))
                return 0;
    return 1;
}

/* Four cells at once, to skip runs of empty space quickly. */
static inline uint64_t load4(const uint16_t *p)
{
    uint64_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

static inline void update_cell(Sim *s, uint16_t *row, uint16_t *next, int x, int y)
{
    int t = CELL_TYPE(row[x]);
    switch (t) {
    case EMPTY: case STONE: case IRON: case GLASS: case WOOD: case LEAF: case ROOT:
    case CRITTER:
        break;
    case WATER:
        update_water(s, row, next, x, y);
        break;
    case SEED:
        update_seed(s, row, next, x, y);
        break;
    case DIRT: case WET_DIRT: case MOSS: case WHEAT:
        /* During the drain, kept earth stays put. Other earth stops
         * clinging once the floor is open, or straight away when it rests
         * on a kept cell: kept roots, moss and iron never crumble, so what
         * they hold up would wait there forever. */
        if (s->phase == PHASE_DRAIN) {
            if (drain_immune(s, x, y))
                break;
            if (s->drain_steps > DRAIN_OPEN_STEPS || s->keep[(y + 1) * s->w + x]) {
                if (fall(s, row, next, x, y) < 0)
                    phase_through(s, row, x, y);
                break;
            }
        }
        if (MOSSY(t)) {
            if (open_below(s, x, y))        /* held: straight down or not at all */
                move(s, row, next, x, x, y);
            break;
        }
        if (!rooted(s, x, y)) {
            if (fall(s, row, next, x, y) < 0)
                phase_through(s, row, x, y);
            break;
        }
        if (open_below(s, x, y))            /* held like moss */
            move(s, row, next, x, x, y);
        break;
    default:
        if (FALLS(t) && fall(s, row, next, x, y) < 0)
            phase_through(s, row, x, y);
        break;
    }
}

static void update_span(Sim *s, int y, int x0, int x1, int dir)
{
    int w = s->w;
    uint16_t *row  = s->cells + y * w;
    uint16_t *next = row + w;

    /* Empty cells do nothing, so skipping them doesn't change the order
     * anything else is updated in. Each group is read just before it's
     * visited, after any change the cells before it made. */
    if (dir > 0) {
        for (int x = x0; x < x1; ) {
            if (x + 4 <= x1 && load4(row + x) == 0) {
                x += 4;
                continue;
            }
            update_cell(s, row, next, x, y);
            x++;
        }
    } else {
        for (int x = x1 - 1; x >= x0; ) {
            if (x - 3 >= x0 && load4(row + x - 3) == 0) {
                x -= 4;
                continue;
            }
            update_cell(s, row, next, x, y);
            x--;
        }
    }
}

/* ---- Trees ---- */

/* base seconds, give or take 20%, so trees planted together don't grow in
 * lockstep. At least a second. */
static uint32_t jitter(Sim *s, float base)
{
    float v = base * (0.8f + 0.4f * rndf(s));
    return v < 1.0f ? 1 : (uint32_t)v;
}

/* Write a tree cell and remember it, if the tree has room in its list. */
static int tree_add(Sim *s, Tree *t, int x, int y, int type)
{
    if (t->n >= TREE_CELLS)
        return -1;
    set_cell(s, x, y, CELL(type, rnd(s) & 0x7F));
    t->cx[t->n] = (int16_t)x;
    t->cy[t->n] = (int16_t)y;
    t->n++;
    return 0;
}

/* Struck moss at (x, y) becomes the foot of a trunk. Returns -1 if every
 * slot is taken. */
static int plant_tree(Sim *s, int x, int y)
{
    Tree *t = NULL;
    for (int i = 0; i < MAX_TREES && !t; i++)
        if (s->tree[i].stage == TREE_FREE)
            t = &s->tree[i];
    if (!t)
        return -1;
    memset(t, 0, sizeof *t);
    t->stage = t->prev_stage = TREE_GROWING;
    t->x = x;
    t->y = t->top = y;
    t->height = 12 + rnd(s) % 13;
    t->canopy = 4 + rnd(s) % 3;
    t->watered_at = t->checked_at = s->clock;
    float per = s->tree_minutes * 60.0f;
    t->grow_at = s->clock + jitter(s, per / (t->height + t->canopy));
    /* Root tips start beside and under the foot of the trunk. */
    for (int r = 0; r < TREE_ROOTS; r++) {
        t->rx[r] = (int16_t)(x + (r == 0 ? 0 : r == 1 ? -1 : 1));
        t->ry[r] = (int16_t)y;
        t->rdir[r] = (int8_t)(r == 1 ? -1 : r == 2 ? 1 : (rnd(s) & 1) ? 1 : -1);
        t->rleft[r] = t->rlen[r] = (int16_t)(12 + rnd(s) % 9);
        t->root_at[r] = s->clock + jitter(s, per / t->rlen[r]);
    }
    tree_add(s, t, x, y, WOOD);
    s->trees++;
    return 0;
}

/* One step of trunk, then one ring of leaves at a time. */
static void grow_up(Sim *s, Tree *t)
{
    if (t->trunk < t->height) {
        int y = t->top - 1;
        int c = y >= 0 ? CELL_TYPE(s->cells[y * s->w + t->x]) : STONE;
        /* Grains and water in the way are cleared; anything solid stops
         * the trunk, and the canopy starts there. */
        if (y < 0 || !(c == EMPTY || LOOSE(c)) || tree_add(s, t, t->x, y, WOOD) != 0) {
            t->height = t->trunk;
            return;
        }
        t->top = y;
        t->trunk++;
        return;
    }
    if (t->ring >= t->canopy)
        return;
    int k = ++t->ring;
    int lo = (k - 1) * (k - 1) + (k - 1), hi = k * k + k;      /* k +- 1/2 */
    for (int dy = -k; dy <= 1; dy++)
        for (int dx = -k; dx <= k; dx++) {
            int d2 = dx * dx + dy * dy, x = t->x + dx, y = t->top + dy;
            if (d2 <= lo || d2 > hi || x < 0 || x >= s->w || y < 0 || y >= s->h)
                continue;
            if (CELL_TYPE(s->cells[y * s->w + x]) == EMPTY && rnd(s) % 10 < 7)
                tree_add(s, t, x, y, LEAF);
        }
    /* A branch or two, 2 cells long, diagonally under the canopy. */
    if (k == 1)
        for (int d = -1; d <= 1; d += 2) {
            if (d > 0 && rnd(s) & 1)
                continue;
            for (int i = 1; i <= 2; i++) {
                int x = t->x + d * i, y = t->top + 1 + i;
                if (x < 0 || x >= s->w || y >= t->y ||
                    CELL_TYPE(s->cells[y * s->w + x]) != EMPTY)
                    break;
                tree_add(s, t, x, y, WOOD);
            }
        }
}

/* Soft ground a root can grow into (or along its own length). */
static int root_soil(int c)
{
    return c == DIRT || c == WET_DIRT || c == SAND || MOSSY(c) || c == SHARD || c == ROOT;
}

/* Root r creeps one cell, sideways its own way about twice as often as
 * down, through soft ground. Blocked sideways, it tries down instead. */
static void grow_root(Sim *s, Tree *t, int r)
{
    int x = t->rx[r], y = t->ry[r];
    int wide = rnd(s) % 3 != 0;
    t->rleft[r]--;
    for (int tries = 0; tries < 2; tries++, wide = !wide) {
        int nx = wide ? x + t->rdir[r] : x, ny = wide ? y : y + 1;
        if (nx < 0 || nx >= s->w || ny >= s->h)
            continue;
        int c = CELL_TYPE(s->cells[ny * s->w + nx]);
        if (!root_soil(c))
            continue;               /* stone, glass, open space, water */
        if (c != ROOT && tree_add(s, t, nx, ny, ROOT) != 0)
            break;
        t->rx[r] = (int16_t)nx;
        t->ry[r] = (int16_t)ny;
        return;
    }
    t->rleft[r] = 0;
}

/* Rain landed anywhere over the tree, from the top of its canopy down to
 * its deepest root, or water is touching one of its roots. */
static void check_water(Sim *s, Tree *t)
{
    int x0 = t->x, x1 = t->x, y0 = t->y, y1 = t->y;
    for (int k = 0; k < t->n; k++) {
        int x = t->cx[k], y = t->cy[k];
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
        if (CELL_TYPE(s->cells[y * s->w + x]) != ROOT)
            continue;
        const uint16_t *c = s->cells + y * s->w + x;
        if ((x > 0 && CELL_TYPE(c[-1]) == WATER) ||
            (x < s->w - 1 && CELL_TYPE(c[1]) == WATER) ||
            (y > 0 && CELL_TYPE(c[-s->w]) == WATER) ||
            (y < s->h - 1 && CELL_TYPE(c[s->w]) == WATER))
            t->watered_at = s->clock;
    }
    if (y0 > 0)
        y0--;                       /* a drop lands just above the canopy */
    for (int ty = y0 >> RAIN_SHIFT; ty <= y1 >> RAIN_SHIFT; ty++)
        for (int tx = x0 >> RAIN_SHIFT; tx <= x1 >> RAIN_SHIFT; tx++) {
            uint32_t at = s->rain_at[ty * s->rw + tx];
            if (at > t->watered_at && at <= s->clock)
                t->watered_at = at;
        }
}

/* A random leaf of the tree that is (or isn't) brown, or -1. */
static int pick_leaf(Sim *s, Tree *t, int brown)
{
    int n = 0, pick = -1;
    for (int k = 0; k < t->n; k++) {
        uint16_t c = s->cells[t->cy[k] * s->w + t->cx[k]];
        if (CELL_TYPE(c) == LEAF && !(CELL_COLOR(c) & WITHERED) == !brown &&
            rnd(s) % ++n == 0)
            pick = k;
    }
    return pick;
}

/* The next cell to come down: leaves first, then wood from the top, then
 * roots. Cells that aren't the tree's any more (erased with the mouse) are
 * skipped. Returns -1 when nothing is left. */
static int next_to_fall(Sim *s, Tree *t)
{
    int best = -1, rank = 3;
    for (int k = 0; k < t->n; k++) {
        int c = CELL_TYPE(s->cells[t->cy[k] * s->w + t->cx[k]]);
        int r = c == LEAF ? 0 : c == WOOD ? 1 : c == ROOT ? 2 : 3;
        if (r < rank || (r == rank && r == 1 && t->cy[k] < t->cy[best])) {
            best = k;
            rank = r;
        }
    }
    return rank < 3 ? best : -1;
}

static void tree_tick(Sim *s, Tree *t, int slot)
{
    uint32_t now = s->clock;
    uint32_t thirst = (uint32_t)s->tree_thirst_minutes * 60;
    int dry = now - t->watered_at >= thirst;
    int k;

    if (t->stage != TREE_FALLING && now != t->checked_at &&
        (s->frame & (MAX_TREES - 1)) == (uint32_t)slot) {
        t->checked_at = now;
        check_water(s, t);
        dry = now - t->watered_at >= thirst;
    }

    switch (t->stage) {
    case TREE_GROWING:
    case TREE_GROWN:
        if (dry) {
            t->prev_stage = t->stage;
            t->stage = TREE_WITHERING;
            t->decay_at = now;
            t->dead_at = 0;
            return;
        }
        if (t->stage == TREE_GROWN)
            return;
        float per = s->tree_minutes * 60.0f;
        if ((t->trunk < t->height || t->ring < t->canopy) && now >= t->grow_at) {
            grow_up(s, t);
            t->grow_at += jitter(s, per / (t->height + t->canopy));
        }
        int done = t->trunk >= t->height && t->ring >= t->canopy;
        for (int r = 0; r < TREE_ROOTS; r++) {
            if (t->rleft[r] > 0 && now >= t->root_at[r]) {
                grow_root(s, t, r);
                t->root_at[r] += jitter(s, per / t->rlen[r]);
            }
            done &= t->rleft[r] <= 0;
        }
        if (done)
            t->stage = TREE_GROWN;
        return;

    case TREE_WITHERING:
        if (now < t->decay_at)
            return;
        t->decay_at += LEAF_SECS;
        if (!dry) {
            /* Rescued: green up again, then carry on. */
            k = pick_leaf(s, t, 1);
            if (k >= 0) {
                int idx = t->cy[k] * s->w + t->cx[k];
                set_cell(s, t->cx[k], t->cy[k], s->cells[idx] & ~(WITHERED << 8));
            } else {
                t->stage = t->prev_stage;
                t->dead_at = 0;
                t->grow_at = now;
                for (int r = 0; r < TREE_ROOTS; r++)
                    t->root_at[r] = now;
            }
            return;
        }
        k = pick_leaf(s, t, 0);
        if (k >= 0) {
            int idx = t->cy[k] * s->w + t->cx[k];
            set_cell(s, t->cx[k], t->cy[k], s->cells[idx] | (WITHERED << 8));
        } else if (!t->dead_at) {
            t->dead_at = now + DEAD_SECS + rnd(s) % DEAD_JITTER;
        } else if (now >= t->dead_at) {
            t->stage = TREE_FALLING;
            t->fall_secs = t->n ? FALL_SECS / t->n : 1;
            if (t->fall_secs < 1)
                t->fall_secs = 1;
            t->decay_at = now;
        }
        return;

    case TREE_FALLING:
        if (now < t->decay_at)
            return;
        t->decay_at += t->fall_secs;
        k = next_to_fall(s, t);
        if (k < 0) {
            t->stage = TREE_FREE;
            s->trees--;
            return;
        }
        /* Dead leaves and wood rot into dark soil that moss can take to;
         * roots go back to plain dirt. */
        set_cell(s, t->cx[k], t->cy[k], CELL(DIRT, rnd(s) & 0xFF));
        return;
    }
}

/* Trees run on the wall clock, sleeping chunks or not, so they keep
 * growing and drying while the desktop is covered. Each schedule applies at
 * most one tick per step, so catching up shows rather than jumps. */
static void update_trees(Sim *s)
{
    for (int i = 0; i < MAX_TREES; i++)
        if (s->tree[i].stage != TREE_FREE)
            tree_tick(s, &s->tree[i], i);
}

void sim_step(Sim *s)
{
    s->frame++;
    s->hue += 0.02f;
    if (s->hue >= 256.0f)
        s->hue -= 256.0f;

    if (s->phase == PHASE_FILL) {
        /* A living tree or a farmed field holds off the drain: pour at a
         * trickle instead, until the screen is overfull. */
        int full = s->grains >= (int)(s->fill_reset * s->w * s->h);
        int hold = full && (s->trees > 0 || s->farms > 0);
        s->held = hold;
        int tried = 0;
        for (int i = 0; i < s->n_emitters; i++)
            emit(s, &s->em[i], hold ? HOLD_POUR_DIV : 1, &tried);
        int stalled = 0;
        s->stall_tried += tried;
        if (++s->stall_steps >= STALL_STEPS) {
            /* A held tree's trickle grows slower, so expect less of it. */
            float gain = STALL_GAIN / (hold ? HOLD_POUR_DIV : 1);
            /* Grains critters tossed off the screen count as poured, so
             * their keeping up doesn't look like a stalled fill. */
            stalled = s->stall_tried > 0 &&
                      s->grains + s->toss_window - s->stall_grains < (int)(gain * s->w * s->h);
            s->stall_steps = s->stall_tried = s->stall_poured = s->toss_window = 0;
            s->stall_grains = s->grains;
        }
        if ((full && !hold) || stalled)
            sim_start_drain(s);
        else if (hold && s->frame % OVERFILL_STEPS == 0 &&
                 s->grains + s->water + s->solids + s->iron > s->drain_overfill * s->w * s->h)
            sim_start_drain(s);
    } else {
        drain(s);
    }

    rain(s);
    if (s->water) {
        evaporate(s);
        drain_floor(s);
    }
    if (s->damp || s->bodies)
        age_damp(s);
    if (s->glass)
        load_glass(s);
    if (s->trees)
        update_trees(s);
    if (s->critters)
        update_critters(s);
    else
        s->farms = 0;

    if (s->blaster_on || s->lightning_on)
        blaster_move(s);
    if (s->blaster_on)
        blast(s);

    /* Bottom-up so a grain moves at most once per step (all moves go
     * downward into rows already scanned). Alternate horizontal direction
     * each step so piles don't lean. Sleeping chunks are skipped. */
    int dir = (s->frame & 1) ? 1 : -1;
    for (int y = s->h - 2; y >= 0; y--) {
        const uint8_t *awake = s->awake + (y >> CHUNK_SHIFT) * s->cw;
        for (int k = 0; k < s->cw; k++) {
            int cx = dir > 0 ? k : s->cw - 1 - k;
            if (!awake[cx])
                continue;
            int x0 = cx * CHUNK;
            int x1 = x0 + CHUNK < s->w ? x0 + CHUNK : s->w;
            update_span(s, y, x0, x1, dir);
        }
    }

    for (int i = 0; i < s->cw * s->ch; i++)
        if (s->awake[i])
            s->awake[i]--;
}

void sim_drop_seed(Sim *s)
{
    /* A few tries, in case the pile reaches the top somewhere. */
    for (int i = 0; i < 8; i++) {
        int x = rnd(s) % s->w;
        if (CELL_TYPE(s->cells[x]) == EMPTY) {
            set_cell(s, x, 0, CELL(SEED, 0));
            return;
        }
    }
}

void sim_paint(Sim *s, int cx, int cy, int radius, int type)
{
    for (int y = cy - radius; y <= cy + radius; y++) {
        for (int x = cx - radius; x <= cx + radius; x++) {
            if (x < 0 || x >= s->w || y < 0 || y >= s->h)
                continue;
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy > radius * radius)
                continue;
            uint16_t cell = s->cells[y * s->w + x];
            if (CELL_TYPE(cell) == CRITTER)
                continue;
            if (type == SAND) {
                /* Sprinkle rather than fill solid. */
                if (CELL_TYPE(cell) == EMPTY && (rnd(s) & 3) == 0)
                    set_cell(s, x, y, CELL(SAND, grain_color(s)));
            } else if (CELL_TYPE(cell) != type) {
                set_cell(s, x, y, CELL(type, rnd(s) & 0x7F));
            }
        }
    }
}

/* Jagged line from (x0, y0) to (x1, y1) by midpoint displacement: each
 * segment's midpoint is pushed sideways by up to a quarter of its length,
 * down to segments min_seg long. Adds every corner after the first. */
static void bolt_split(Sim *s, Bolt *b, int x0, int y0, int x1, int y1, int min_seg)
{
    int dx = x1 - x0, dy = y1 - y0;
    if (dx * dx + dy * dy <= min_seg * min_seg || b->start[b->lines] >= BOLT_MAX - 1) {
        int n = b->start[b->lines];
        if (n < BOLT_MAX) {
            b->x[n] = (int16_t)x1;
            b->y[n] = (int16_t)y1;
            b->start[b->lines]++;
        }
        return;
    }
    /* (-dy, dx) is perpendicular; times up to +-1/4 of the length. */
    float off = (rndf(s) - 0.5f) * 0.5f;
    int mx = (x0 + x1) / 2 + (int)(-dy * off);
    int my = (y0 + y1) / 2 + (int)(dx * off);
    bolt_split(s, b, x0, y0, mx, my, min_seg);
    bolt_split(s, b, mx, my, x1, y1, min_seg);
}

/* Starts a new line at (x0, y0) and draws it to (x1, y1). */
static void bolt_line(Sim *s, Bolt *b, int x0, int y0, int x1, int y1, int min_seg)
{
    int n = b->start[b->lines];
    if (n >= BOLT_MAX - 1 || b->lines >= BOLT_LINES)
        return;
    b->x[n] = (int16_t)x0;
    b->y[n] = (int16_t)y0;
    b->start[b->lines] = n + 1;
    bolt_split(s, b, x0, y0, x1, y1, min_seg);
    b->start[b->lines + 1] = b->start[b->lines];
    b->start[b->lines] = n;
    b->lines++;
}

/* Nothing but material between (x, y) and the floor or a static cell, so
 * it's part of a pile rather than a grain falling through the air. */
static int resting(Sim *s, int x, int y)
{
    for (int yy = y + 1; yy < s->h; yy++) {
        int t = CELL_TYPE(s->cells[yy * s->w + x]);
        if (t == EMPTY)
            return 0;
        if (!LOOSE(t))
            return 1;
    }
    return 1;
}

/* Something to strike within reach of (ox, oy): not empty, not water, and
 * not falling, or a stream of sand would turn to glass hanging in the air. */
static int strike_target(Sim *s, int ox, int oy, int reach, int *tx, int *ty)
{
    for (int i = 0; i < STRIKE_TRIES; i++) {
        int x = ox + (int)(rnd(s) % (2 * reach + 1)) - reach;
        int y = oy + (int)(rnd(s) % (2 * reach + 1)) - reach;
        int dx = x - ox, dy = y - oy;
        if (x < 0 || x >= s->w || y < 0 || y >= s->h || dx * dx + dy * dy > reach * reach)
            continue;
        int t = CELL_TYPE(s->cells[y * s->w + x]);
        if (t != EMPTY && t != WATER && t != CRITTER && resting(s, x, y)) {
            *tx = x;
            *ty = y;
            return 0;
        }
    }
    return -1;
}

int sim_strike(Sim *s)
{
    int ox = (int)s->bl.x, oy = (int)s->bl.y;
    if (ox < 0) ox = 0;
    if (ox >= s->w) ox = s->w - 1;
    if (oy < 0) oy = 0;
    if (oy >= s->h) oy = s->h - 1;
    int tx, ty;
    if (strike_target(s, ox, oy, s->lightning_reach, &tx, &ty) != 0 &&
        strike_target(s, ox, oy, 2 * s->lightning_reach, &tx, &ty) != 0)
        return -1;

    /* Long bolts get longer segments, so the corners fit in BOLT_MAX. */
    Bolt *b = &s->bolt;
    int dx = tx - ox, dy = ty - oy;
    int len = 0;
    while (len * len < dx * dx + dy * dy)
        len++;
    int min_seg = len / 48 > BOLT_MIN_SEG ? len / 48 : BOLT_MIN_SEG;
    b->lines = 0;
    b->start[0] = 0;
    bolt_line(s, b, ox, oy, tx, ty, min_seg);

    /* 0-2 forks off random corners of the main path, a quarter to half its
     * length, heading roughly the same way. */
    int main_n = b->start[1];
    int forks = rnd(s) % 3;
    for (int f = 0; f < forks && main_n > 2; f++) {
        int k = 1 + rnd(s) % (main_n - 2);
        float scale = 0.25f + rndf(s) * 0.25f;
        float turn = (rndf(s) - 0.5f) * 1.6f;     /* about +-45 degrees */
        float fx = dx * scale, fy = dy * scale;
        int ex = b->x[k] + (int)(fx - fy * turn);
        int ey = b->y[k] + (int)(fy + fx * turn);
        bolt_line(s, b, b->x[k], b->y[k], ex, ey, min_seg);
    }

    int r = s->strike_radius;
    b->tx = tx;
    b->ty = ty;
    b->radius = r;
    b->box[0] = tx - r; b->box[1] = ty - r;
    b->box[2] = tx + r; b->box[3] = ty + r;
    for (int i = 0; i < b->start[b->lines]; i++) {
        if (b->x[i] < b->box[0]) b->box[0] = b->x[i];
        if (b->y[i] < b->box[1]) b->box[1] = b->y[i];
        if (b->x[i] > b->box[2]) b->box[2] = b->x[i];
        if (b->y[i] > b->box[3]) b->box[3] = b->y[i];
    }
    b->id++;
    charge_critters(s);

    /* The first struck moss with open air, water or loose grains above it
     * becomes a tree; the rest stays as ground cover. The column above the
     * tree is left unfused so the trunk can push up through it. */
    int bx = -1, by = -1;
    for (int y = ty - r; y <= ty + r && bx < 0; y++) {
        if (y < 1 || y >= s->h)
            continue;
        for (int x = tx - r; x <= tx + r; x++) {
            int ddx = x - tx, ddy = y - ty;
            if (x < 0 || x >= s->w || ddx * ddx + ddy * ddy > r * r ||
                CELL_TYPE(s->cells[y * s->w + x]) != MOSS)
                continue;
            int above = CELL_TYPE(s->cells[(y - 1) * s->w + x]);
            if ((above == EMPTY || (LOOSE(above) && !EARTH(above))) &&
                plant_tree(s, x, y) == 0) {
                bx = x;
                by = y;
                break;
            }
        }
    }

    /* Sand fuses to glass, keeping its color so the banding shows through,
     * unless it's still falling; dirt bakes to stone; stone becomes iron.
     * Iron struck again does nothing. */
    for (int y = ty - r; y <= ty + r; y++) {
        if (y < 0 || y >= s->h)
            continue;
        for (int x = tx - r; x <= tx + r; x++) {
            int ddx = x - tx, ddy = y - ty;
            if (x < 0 || x >= s->w || ddx * ddx + ddy * ddy > r * r)
                continue;
            uint16_t c = s->cells[y * s->w + x];
            int t = CELL_TYPE(c);
            if (x == bx && y < by)
                continue;
            if (t == SAND && resting(s, x, y))
                set_cell(s, x, y, CELL(GLASS, CELL_COLOR(c)));
            else if (t == DIRT || t == WET_DIRT)
                set_cell(s, x, y, CELL(STONE, rnd(s) & 0x7F));
            else if (t == STONE)     /* a stone hut stays the critters' */
                set_cell(s, x, y, CELL(IRON, (rnd(s) & 0x7F) | (CELL_COLOR(c) & BUILT)));
        }
    }
    return 0;
}
