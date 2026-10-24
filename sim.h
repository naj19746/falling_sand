#ifndef SIM_H
#define SIM_H

#include <stdint.h>

/* DEBRIS is crumbled stone: falls like sand, drawn in stone colors.
 * WATER falls, spreads sideways and pools; sand sinks through it.
 * SEED falls like sand; landing on sand turns the sand around it to DIRT,
 * landing on anything else leaves a single DIRT grain. DIRT acts like sand.
 * A drop of water landing on DIRT soaks in and makes it WET_DIRT, which
 * dries after a while; a drop landing on WET_DIRT makes MOSS, which dries
 * back to WET_DIRT once no rain has landed near it for a while.
 * GLASS is sand fused by lightning: rigid like stone until too much weight
 * piles on it, then it breaks into SHARD, which falls like sand.
 * WOOD, LEAF and ROOT make up a tree, grown from moss struck by lightning.
 * Everything from SAND on falls. */
enum { EMPTY, STONE, IRON, WATER, GLASS, WOOD, LEAF, ROOT,
       SAND, DEBRIS, SEED, DIRT, WET_DIRT, MOSS, SHARD };

/* Each cell: low byte = type, high byte = palette color index. */
#define CELL(type, color) ((uint16_t)((type) | ((color) << 8)))
#define CELL_TYPE(c)      ((c) & 0xFF)
#define CELL_COLOR(c)     ((c) >> 8)
#define FALLS(t)          ((t) >= SAND)
#define SOLID(t)          ((t) == STONE || (t) == IRON || (t) == GLASS || TREE(t))  /* static, drained by crumbling */
#define DAMP(t)           ((t) == WET_DIRT || (t) == MOSS)
#define EARTH(t)          ((t) == DIRT || DAMP(t))    /* the blaster spares these */
#define TREE(t)           ((t) == WOOD || (t) == LEAF || (t) == ROOT)

/* Color bit on a leaf that has turned brown on a thirsty tree. */
#define WITHERED          0x80

/* Color bit on a raindrop that hasn't landed yet, so puddle water trickling
 * around doesn't count as rain for keeping moss alive. */
#define RAIN_FRESH        0x80

/* Color bit on water working its way down through sand or moss. Sand
 * doesn't sink through it, or it would just swap straight back. */
#define SEEPING           0x40
#define LOOSE(t)          (FALLS(t) || (t) == WATER)   /* blastable, drainable */

/* The grid is split into CHUNK x CHUNK blocks. Blocks where nothing has
 * moved for a couple of steps go to sleep and aren't scanned, and the
 * renderer only redraws blocks that changed. */
#define CHUNK_SHIFT 4
#define CHUNK       (1 << CHUNK_SHIFT)

#define MAX_EMITTERS 3

/* Pours in bursts at random spots, pausing between them. */
typedef struct {
    float x, v;
    int burst_left;         /* steps remaining in current burst */
    int idle_left;          /* steps to wait before the next burst */
    int burst_width;        /* half-width of the stream */
    int burst_rate;         /* grains attempted per step */
} Emitter;

/* Sound-driven destroyer: rides the pile surface and blasts grains away,
 * harder the louder the audio is. */
typedef struct {
    float x, v;
    float y;                /* smoothed, follows the surface */
    float level;            /* 0..1, set by the caller each frame */
    float radius;           /* current blast radius, for the renderer */
} Blaster;

/* A lightning bolt, for the renderer to draw: the corners of up to
 * BOLT_LINES jagged lines, the first from the blaster to where it struck,
 * the rest forks off it. Line i is points start[i] .. start[i + 1] - 1. */
#define BOLT_MAX   192
#define BOLT_LINES 3
typedef struct {
    int16_t x[BOLT_MAX], y[BOLT_MAX];
    int start[BOLT_LINES + 1];
    int lines;
    int tx, ty, radius;     /* the struck disc */
    int box[4];             /* cell bounds of everything: x0, y0, x1, y1 */
    uint32_t id;            /* bumped on each strike, so the renderer sees new ones */
} Bolt;

/* A tree grown from struck moss. It keeps its slot from planting until
 * the last of it has fallen, since it has to keep track of its water. */
#define MAX_TREES      16
#define TREE_CELLS     192
#define TREE_ROOTS     3
enum { TREE_FREE, TREE_GROWING, TREE_GROWN, TREE_WITHERING, TREE_FALLING };
typedef struct {
    int stage;
    int prev_stage;         /* growing or grown, to go back to if rescued */
    int x, y;               /* base of the trunk */
    int height, trunk;      /* target trunk height, trunk cells so far */
    int canopy, ring;       /* canopy radius, leaf rings so far */
    int top;                /* y of the top of the trunk */
    int16_t rx[TREE_ROOTS], ry[TREE_ROOTS], rleft[TREE_ROOTS], rlen[TREE_ROOTS];
    int8_t rdir[TREE_ROOTS];   /* -1 or 1: the way each root spreads */
    uint32_t grow_at;       /* wall-clock second of the next trunk/leaf tick */
    uint32_t root_at[TREE_ROOTS];
    uint32_t decay_at;      /* next withering, recovering or falling tick */
    uint32_t watered_at;    /* last time water reached it */
    uint32_t checked_at;    /* last time it looked for water */
    uint32_t dead_at;       /* all leaves brown: falls at this time if still dry */
    int fall_secs;          /* seconds between cells coming down */
    int n;                  /* cells in the list below */
    int16_t cx[TREE_CELLS], cy[TREE_CELLS];
} Tree;

enum { PHASE_FILL, PHASE_DRAIN };

typedef struct {
    int w, h;
    uint16_t *cells;
    uint32_t frame;
    uint32_t rng;

    int cw, ch;             /* grid size in chunks */
    uint8_t *awake;         /* per chunk: steps left before it sleeps */
    uint8_t *changed;       /* per chunk: needs redrawing; renderer clears */
    uint8_t *moved;         /* per cell: step number of its last sideways move */

    Emitter em[MAX_EMITTERS];
    int n_emitters;
    float hue;              /* palette position, drifts over time */

    int grains;             /* falling cells, kept as a running total */
    int solids;             /* stone, glass and tree cells, running total */
    float fill_reset;       /* start draining at this fraction full */
    int phase;
    int drain_steps;

    int blaster_on;
    Blaster bl;

    int water;              /* water cells, running total */
    float water_rate;       /* drops per step, set by the caller */
    float water_acc;
    int raining;            /* set by the caller; slows evaporation */

    int seed_radius;        /* sand within this many cells turns to dirt */
    uint32_t slip;          /* out of 1024: chance a stuck grain slides 2 over */

    /* Wet dirt and moss age by the wall clock, so they keep drying while
     * the desktop is covered and the simulation isn't stepping. */
    uint32_t clock;         /* seconds, set by the caller */
    uint32_t *since;        /* per cell: when it got wet / last saw rain */
    int damp;               /* wet dirt + moss cells, running total */
    int rw, rh;             /* rain map size in tiles */
    uint32_t *rain_at;      /* per tile: when rain last landed there */
    int moss_rain_radius;   /* rain this close (cells) keeps moss alive */

    /* Lightning: the blaster also drifts when lightning is on, since it's
     * where bolts come from. */
    int lightning_on;
    int lightning_reach;    /* cells from the blaster a bolt can land */
    int strike_radius;      /* cells around the impact that change */
    int glass_strength;     /* cells of load glass holds before breaking */
    int glass;              /* glass cells, running total */
    Bolt bolt;

    /* Trees: while any is alive the drain waits, pouring at a trickle,
     * until the screen is drain_overfill full. */
    Tree tree[MAX_TREES];
    int trees;              /* slots in use */
    int tree_minutes;       /* time to grow fully */
    int tree_thirst_minutes;/* time without water before it starts dying */
    float drain_overfill;
} Sim;

int  sim_init(Sim *s, int w, int h, uint32_t seed, int n_emitters);
void sim_free(Sim *s);
void sim_reset(Sim *s);
void sim_start_drain(Sim *s);
void sim_step(Sim *s);
void sim_paint(Sim *s, int cx, int cy, int radius, int type);
void sim_drop_seed(Sim *s);     /* from a random spot along the top */

/* Lightning from the blaster to something nearby. Returns 0 if it struck,
 * -1 if there was nothing in reach (the strike is used up either way). */
int  sim_strike(Sim *s);

/* Chunks currently being simulated, for --profile. */
int  sim_awake_chunks(const Sim *s);

#endif
