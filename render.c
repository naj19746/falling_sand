#include "render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#define BG_COLOR   0xFF14161Cu
#define RING_COLOR 0xFFFFF4E0u
#define SEED_COLOR 0xFF8CE85Au
#define BOLT_COLOR 0xFFE8F0FFu
#define FLASH_COLOR 0xFFFFFFFFu

/* Frames a lightning bolt stays on screen, and how many of those it's
 * drawn solid (the rest it flickers) and shows its forks. */
/* About the most memory the image takes: as many full-width rows of chunks
 * as fit in this, and at least one. */
#define IMAGE_BYTES (2 << 20)

/* A tossed scoop of sand flies off the screen over TOSS_STEPS sim steps,
 * rising TOSS_HEIGHT cells. */
#define TOSS_STEPS  30
#define TOSS_HEIGHT 10

#define BOLT_FRAMES 6
#define BOLT_SOLID  2
#define BOLT_FORKS  3

typedef struct {
    const char *name;
    int n;
    uint8_t keys[8][3];
} PaletteDef;

static const PaletteDef palettes[] = {
    {"desert", 5, {{236, 204, 148}, {214, 164, 104}, {186, 118,  78},
                   {242, 222, 176}, {168, 108,  72}}},
    {"pastel", 6, {{255, 179, 186}, {255, 223, 186}, {255, 255, 186},
                   {186, 255, 201}, {186, 225, 255}, {214, 186, 255}}},
    {"neon",   6, {{255,   0, 128}, {255, 120,   0}, {255, 230,   0},
                   {  0, 255, 140}, {  0, 200, 255}, {170,  60, 255}}},
    {"mono",   4, {{230, 230, 230}, {160, 160, 160}, { 90,  90,  90},
                   {200, 200, 200}}},
};

const char *const render_palettes[] = {"desert", "pastel", "neon", "mono", NULL};

/* Interpolate cyclically between key colors so drifting through the
 * indices gives smooth color bands. */
static void build_palette(uint32_t *pal, const PaletteDef *p)
{
    for (int i = 0; i < 256; i++) {
        float pos = i * p->n / 256.0f;
        int a = (int)pos % p->n;
        int b = (a + 1) % p->n;
        float t = pos - (int)pos;
        uint32_t rgb = 0;
        for (int ch = 0; ch < 3; ch++) {
            int v = (int)(p->keys[a][ch] + (p->keys[b][ch] - p->keys[a][ch]) * t);
            rgb = (rgb << 8) | (uint32_t)v;
        }
        pal[i] = 0xFF000000u | rgb;
    }
}

/* Slightly varied grays so ledges and debris have some texture. */
static void build_stone(uint32_t *pal)
{
    for (int i = 0; i < 256; i++) {
        int v = 70 + (i * 7 % 23) - 11;
        pal[i] = 0xFF000000u | (uint32_t)(v << 16 | (v + 4) << 8 | (v + 14));
    }
}

/* Cool gray metal, slightly varied per color index. */
static void build_iron(uint32_t *pal)
{
    for (int i = 0; i < 256; i++) {
        int v = 150 + (i * 11 % 29) - 14;
        pal[i] = 0xFF000000u | (uint32_t)(v << 16 | v << 8 | (v + 20));
    }
}

static int shm_failed;
static int shm_error_handler(Display *d, XErrorEvent *e)
{
    (void)d;
    (void)e;
    shm_failed = 1;
    return 0;
}

/* Shared memory image, so frames aren't copied through the X socket.
 * Falls back to a plain XImage (e.g. on a remote display). */
static int create_image(Render *r, Visual *vis, int depth, int iw, int ih)
{
    if (XShmQueryExtension(r->dpy)) {
        r->img = XShmCreateImage(r->dpy, vis, depth, ZPixmap, NULL, &r->shm, iw, ih);
        if (r->img) {
            r->shm.shmid = shmget(IPC_PRIVATE, (size_t)r->img->bytes_per_line * ih,
                                  IPC_CREAT | 0600);
            if (r->shm.shmid >= 0) {
                r->shm.shmaddr = r->img->data = shmat(r->shm.shmid, NULL, 0);
                r->shm.readOnly = False;

                shm_failed = 0;
                XErrorHandler old = XSetErrorHandler(shm_error_handler);
                XShmAttach(r->dpy, &r->shm);
                XSync(r->dpy, False);
                XSetErrorHandler(old);
                /* Freed automatically once both sides detach. */
                shmctl(r->shm.shmid, IPC_RMID, NULL);

                if (!shm_failed) {
                    r->use_shm = 1;
                    return 0;
                }
                shmdt(r->shm.shmaddr);
            }
            r->img->data = NULL;
            XDestroyImage(r->img);
            r->img = NULL;
        }
    }

    char *data = malloc((size_t)iw * ih * 4);
    if (!data)
        return -1;
    r->img = XCreateImage(r->dpy, vis, depth, ZPixmap, 0, data, iw, ih, 32, 0);
    if (!r->img) {
        free(data);
        return -1;
    }
    return 0;
}

/* Blues with a little variation, so pools shimmer slightly. */
static void build_water(uint32_t *pal)
{
    for (int i = 0; i < 256; i++) {
        int j = (i * 13 % 29) - 14;
        int r = 38 + j / 2, g = 104 + j, b = 196 + j;
        pal[i] = 0xFF000000u | (uint32_t)(r << 16 | g << 8 | b);
    }
}

/* Dark browns, so patches of dirt stand out from any sand palette; wet dirt
 * is darker still, and moss is a muted green. Also used for tree bark, tree
 * roots and leaves. All share the same texture per color index, so a patch
 * keeps its grain as it changes. */
static void build_earth(uint32_t *pal, int r0, int g0, int b0)
{
    for (int i = 0; i < 256; i++) {
        int j = (i * 11 % 31) - 15;
        int r = r0 + j * 3 / 4, g = g0 + j * 3 / 4, b = b0 + j / 2;
        pal[i] = 0xFF000000u | (uint32_t)(r << 16 | g << 8 | b);
    }
}

/* Withered leaves: orange sliding toward dull brown, varied per index. */
static void build_autumn(uint32_t *pal)
{
    static const float top[3] = {210, 120, 40}, bot[3] = {120, 80, 45};
    for (int i = 0; i < 256; i++) {
        float t = (i * 37 % 64) / 63.0f;
        uint32_t rgb = 0;
        for (int ch = 0; ch < 3; ch++)
            rgb = (rgb << 8) | (uint32_t)(top[ch] + (bot[ch] - top[ch]) * t);
        pal[i] = 0xFF000000u | rgb;
    }
}

/* Pale cyan, 45% of the way to the background, with a little of the sand
 * palette mixed in so the color byte the sand had shows as faint banding.
 * Shards are the same, a little brighter. */
static void build_glass(uint32_t *glass, uint32_t *shard, const uint32_t *sand)
{
    static const int base[3] = {190, 235, 245};
    for (int i = 0; i < 256; i++) {
        uint32_t g = 0, sh = 0;
        for (int ch = 0; ch < 3; ch++) {
            int shift = 16 - ch * 8;
            int sv = (int)(sand[i] >> shift & 0xFF);
            int bg = (int)(BG_COLOR >> shift & 0xFF);
            int v = (base[ch] * 4 + sv) / 5;
            v = v + (bg - v) * 45 / 100;
            int b = v + 30 > 255 ? 255 : v + 30;
            g = g << 8 | (uint32_t)v;
            sh = sh << 8 | (uint32_t)b;
        }
        glass[i] = 0xFF000000u | g;
        shard[i] = 0xFF000000u | sh;
    }
}

/* Wheat by how much food is left in it: pale green when eaten down,
 * deep gold when full. The rest of the color byte is texture. */
static void build_wheat(uint32_t *pal)
{
    static const int lo[3] = {130, 170, 80}, hi[3] = {214, 166, 44};
    for (int i = 0; i < 256; i++) {
        int k = i & WHEAT_MAX, j = ((i >> 3) * 11 % 17) - 8;
        uint32_t rgb = 0;
        for (int ch = 0; ch < 3; ch++)
            rgb = rgb << 8 | (uint32_t)(lo[ch] + (hi[ch] - lo[ch]) * k / WHEAT_MAX + j);
        pal[i] = 0xFF000000u | rgb;
    }
}

/* Critter parts: a pale head over a violet body, which stands out from
 * every sand palette and the dirt and moss. Each slot is tinted a little
 * differently so neighbors can be told apart. */
static void build_critter(uint32_t *pal)
{
    static const int part[6][3] = {
        {242, 238, 224},    /* head */
        { 24,  20,  28},    /* eye */
        {150, 104, 190},    /* body */
        { 92,  62, 120},    /* leg */
        {118,  84, 150},    /* leg, other step */
        {200, 232, 255},    /* spark */
    };
    for (int i = 0; i < 256; i++) {
        int p = i >> 5, j = (i & 31) * 7 % 15 - 7;
        if (p > PART_SPARK)
            p = PART_BODY;
        int jj = p == PART_EYE || p == PART_SPARK ? 0 : j;
        pal[i] = 0xFF000000u | (uint32_t)((part[p][0] + jj) << 16 |
                                          (part[p][1] + jj / 2) << 8 | part[p][2]);
    }
}

/* Stone and iron a critter placed: a little warmer and with a darker
 * line along the bottom of every other cell's texture, so walls read as
 * laid bricks. */
static void build_built(uint32_t *pal)
{
    for (int i = BUILT; i < 256; i++) {
        uint32_t c = pal[i & ~BUILT];
        int r = (int)(c >> 16 & 0xFF), g = (int)(c >> 8 & 0xFF), b = (int)(c & 0xFF);
        int d = i & 1 ? 0 : 12;
        r = r + 14 - d; g = g + 4 - d; b = b - 8 - d;
        pal[i] = 0xFF000000u | (uint32_t)((r < 0 ? 0 : r > 255 ? 255 : r) << 16 |
                                          (g < 0 ? 0 : g) << 8 | (b < 0 ? 0 : b));
    }
}

/* Everything but the X side: image size in chunks, chunk maps and color
 * tables. Needs w, h, scale, cw, ch, pw and ph set. */
static int render_setup(Render *r, const PaletteDef *pd)
{
    int cpx = CHUNK * r->scale;
    r->acw = (r->pw + cpx - 1) / cpx;
    r->ach = (int)(IMAGE_BYTES / ((size_t)r->acw * cpx * cpx * 4));
    if (r->ach < 1)
        r->ach = 1;
    if (r->ach > r->ch)
        r->ach = r->ch;

    r->slot = malloc((size_t)r->cw * r->ch * sizeof *r->slot);
    r->runs = malloc((size_t)r->cw * r->ch * 4 * sizeof *r->runs);
    if (!r->slot || !r->runs)
        return -1;
    for (int i = 0; i < r->cw * r->ch; i++)
        r->slot[i] = -1;

    r->dirty = calloc((size_t)r->cw * r->ch, 1);
    if (!r->dirty)
        return -1;
    for (int i = 0; i < MAX_CRITTERS; i++)
        r->carry_at[i][0] = r->carry_at[i][1] = r->toss_at[i][0] = r->toss_at[i][1] = -1;

    uint32_t autumn[256];
    for (int t = 0; t < 32; t++)
        for (int i = 0; i < 256; i++)
            r->lut[t][i] = BG_COLOR;
    build_palette(r->lut[SAND], pd);
    build_stone(r->lut[STONE]);
    build_iron(r->lut[IRON]);
    memcpy(r->lut[DEBRIS], r->lut[STONE], sizeof r->lut[DEBRIS]);
    build_water(r->lut[WATER]);
    build_earth(r->lut[DIRT],     96, 64, 40);
    build_earth(r->lut[WET_DIRT], 58, 40, 30);
    build_earth(r->lut[MOSS],     72, 122, 52);
    build_earth(r->lut[WOOD],     70, 46, 30);
    build_earth(r->lut[ROOT],     84, 46, 34);
    build_earth(r->lut[LEAF],     64, 168, 56);
    build_autumn(autumn);
    for (int i = WITHERED; i < 256; i++)
        if (i & WITHERED)
            r->lut[LEAF][i] = autumn[i];
    build_glass(r->lut[GLASS], r->lut[SHARD], r->lut[SAND]);
    for (int i = 0; i < 256; i++)
        r->lut[SEED][i] = SEED_COLOR;
    build_earth(r->lut[BODY], 112, 112, 116);
    build_wheat(r->lut[WHEAT]);
    build_critter(r->lut[CRITTER]);
    build_built(r->lut[STONE]);
    build_built(r->lut[IRON]);
    return 0;
}

int render_init(Render *r, unsigned long xwin, int w, int h, int scale,
                const char *palette)
{
    const PaletteDef *pd = NULL;
    for (size_t i = 0; i < sizeof palettes / sizeof palettes[0]; i++)
        if (strcmp(palettes[i].name, palette) == 0)
            pd = &palettes[i];
    if (!pd) {
        fprintf(stderr, "render: unknown palette '%s'\n", palette);
        return -1;
    }

    memset(r, 0, sizeof *r);
    r->w = w;
    r->h = h;
    r->scale = scale;
    r->cw = (w + CHUNK - 1) / CHUNK;
    r->ch = (h + CHUNK - 1) / CHUNK;
    r->win = xwin;

    r->dpy = XOpenDisplay(NULL);
    if (!r->dpy) {
        fprintf(stderr, "render: can't open X display\n");
        return -1;
    }

    XWindowAttributes attr;
    if (!XGetWindowAttributes(r->dpy, r->win, &attr)) {
        fprintf(stderr, "render: bad window 0x%lx\n", xwin);
        return -1;
    }
    /* Pixels are written as 0xAARRGGBB words. */
    Visual *vis = attr.visual;
    if ((attr.depth != 24 && attr.depth != 32) || vis->red_mask != 0xFF0000 ||
        vis->green_mask != 0xFF00 || vis->blue_mask != 0xFF) {
        fprintf(stderr, "render: unsupported visual (depth %d)\n", attr.depth);
        return -1;
    }

    r->pw = attr.width;
    r->ph = attr.height;
    if (render_setup(r, pd) != 0)
        return -1;
    int cpx = CHUNK * scale;
    if (create_image(r, vis, attr.depth, r->acw * cpx, r->ach * cpx) != 0) {
        fprintf(stderr, "render: can't create image\n");
        return -1;
    }
    r->gc = XCreateGC(r->dpy, r->win, 0, NULL);
    render_invalidate(r);
    return 0;
}

void render_free(Render *r)
{
    if (!r->dpy)
        return;
    if (r->img) {
        if (r->use_shm) {
            XShmDetach(r->dpy, &r->shm);
            shmdt(r->shm.shmaddr);
            r->img->data = NULL;
        }
        XDestroyImage(r->img);      /* frees malloc'd data in the fallback */
    }
    if (r->gc)
        XFreeGC(r->dpy, r->gc);
    XCloseDisplay(r->dpy);
    free(r->dirty);
    free(r->slot);
    free(r->runs);
    r->dpy = NULL;
}

void render_invalidate(Render *r)
{
    memset(r->dirty, 1, (size_t)r->cw * r->ch);
}

static inline uint32_t cell_color(const Render *r, uint16_t c)
{
    return r->lut[CELL_TYPE(c) & 31][CELL_COLOR(c)];
}

/* 50/50 blend toward the ring color. */
static inline uint32_t blend(uint32_t a, uint32_t b)
{
    return 0xFF000000u | (((a & 0xFEFEFE) >> 1) + ((b & 0xFEFEFE) >> 1));
}

/* Top left pixel in the image of chunk slot sl. */
static inline char *slot_pixels(const Render *r, int sl)
{
    int cpx = CHUNK * r->scale;
    return r->img->data + (size_t)(sl / r->acw) * cpx * r->img->bytes_per_line
                        + (size_t)(sl % r->acw) * cpx * 4;
}

/* Paint one grid cell as a scale x scale block, clipped to the window, if
 * its chunk is in the image now. */
static inline void put_cell(Render *r, int gx, int gy, uint32_t color)
{
    int sl = r->slot[(gy >> CHUNK_SHIFT) * r->cw + (gx >> CHUNK_SHIFT)];
    if (sl < 0)
        return;
    int px = gx * r->scale, py = gy * r->scale;
    int pw = r->scale, ph = r->scale;
    if (px + pw > r->pw) pw = r->pw - px;
    if (py + ph > r->ph) ph = r->ph - py;
    char *base = slot_pixels(r, sl) + (size_t)((gy & (CHUNK - 1)) * r->scale) * r->img->bytes_per_line
                                    + (size_t)((gx & (CHUNK - 1)) * r->scale) * 4;
    for (int j = 0; j < ph; j++) {
        uint32_t *dst = (uint32_t *)(base + (size_t)j * r->img->bytes_per_line);
        for (int i = 0; i < pw; i++)
            dst[i] = color;
    }
}

/* Draw a chunk into image slot sl. Each row of cells is drawn as one row
 * of pixels, then copied down. */
static void draw_chunk(Render *r, const Sim *s, int cx, int cy, int sl)
{
    int x0 = cx * CHUNK, y0 = cy * CHUNK;
    int x1 = x0 + CHUNK < r->w ? x0 + CHUNK : r->w;
    int y1 = y0 + CHUNK < r->h ? y0 + CHUNK : r->h;
    int sc = r->scale, bpl = r->img->bytes_per_line;
    int pw = (x1 - x0) * sc;
    if (x0 * sc + pw > r->pw)
        pw = r->pw - x0 * sc;
    char *base = slot_pixels(r, sl);
    for (int y = y0; y < y1; y++, base += (size_t)sc * bpl) {
        int ph = sc;
        if (y * sc + ph > r->ph)
            ph = r->ph - y * sc;
        if (ph <= 0)
            break;
        uint32_t *dst = (uint32_t *)base;
        const uint16_t *row = s->cells + y * s->w;
        int i = 0;
        for (int x = x0; x < x1 && i < pw; x++) {
            uint32_t c = cell_color(r, row[x]);
            for (int k = 0; k < sc && i < pw; k++)
                dst[i++] = c;
        }
        for (int j = 1; j < ph; j++)
            memcpy(base + (size_t)j * bpl, dst, (size_t)pw * 4);
    }
}

static void draw_ring(Render *r, const Sim *s, const Blaster *b)
{
    float rad = b->radius;
    /* Enough points around the circumference to leave no gaps. */
    int steps = (int)(rad * 7) + 8;
    int last = -1;
    for (int i = 0; i < steps; i++) {
        float a = i * 6.2831853f / steps;
        int x = (int)(b->x + rad * __builtin_cosf(a));
        int y = (int)(b->y + rad * __builtin_sinf(a));
        if (x < 0 || x >= r->w || y < 0 || y >= r->h)
            continue;
        int idx = y * r->w + x;
        if (idx == last)
            continue;
        put_cell(r, x, y, blend(cell_color(r, s->cells[idx]), RING_COLOR));
        last = idx;
    }
}

static void mark_box(Render *r, const int box[4])
{
    for (int cy = box[1]; cy <= box[3]; cy++)
        for (int cx = box[0]; cx <= box[2]; cx++)
            r->dirty[cy * r->cw + cx] = 1;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* An overlay drawn over the cells (the ring, a bolt) covers different
 * chunks each frame: redraw the chunk box (x0, y0, x1, y1 in cells) it
 * covers now and the one it covered last frame, which is then replaced. */
static void overlay_box(Render *r, int show, int x0, int y0, int x1, int y1,
                        int prev[4], int *have_prev)
{
    int box[4];
    if (show) {
        box[0] = clampi(x0 >> CHUNK_SHIFT, 0, r->cw - 1);
        box[1] = clampi(y0 >> CHUNK_SHIFT, 0, r->ch - 1);
        box[2] = clampi(x1 >> CHUNK_SHIFT, 0, r->cw - 1);
        box[3] = clampi(y1 >> CHUNK_SHIFT, 0, r->ch - 1);
        mark_box(r, box);
    }
    if (*have_prev)
        mark_box(r, prev);
    *have_prev = show;
    if (show)
        memcpy(prev, box, sizeof box);
}

/* Lightning: solid for the first frames, then flickering half-blended,
 * with the forks only early on and the struck disc flashing white. */
static void draw_bolt(Render *r, const Sim *s, int frame)
{
    const Bolt *b = &s->bolt;
    if (frame == 4)                         /* the flicker */
        return;
    int solid = frame <= BOLT_SOLID;
    int lines = frame <= BOLT_FORKS ? b->lines : (b->lines > 0);

    if (solid) {
        int rad = b->radius;
        for (int y = b->ty - rad; y <= b->ty + rad; y++)
            for (int x = b->tx - rad; x <= b->tx + rad; x++) {
                int dx = x - b->tx, dy = y - b->ty;
                if (x >= 0 && x < r->w && y >= 0 && y < r->h && dx * dx + dy * dy <= rad * rad)
                    put_cell(r, x, y, FLASH_COLOR);
            }
    }

    for (int l = 0; l < lines; l++)
        for (int i = b->start[l]; i + 1 < b->start[l + 1]; i++) {
            /* Bresenham between consecutive corners. */
            int x = b->x[i], y = b->y[i], x1 = b->x[i + 1], y1 = b->y[i + 1];
            int dx = abs(x1 - x), sx = x < x1 ? 1 : -1;
            int dy = -abs(y1 - y), sy = y < y1 ? 1 : -1;
            int err = dx + dy;
            for (;;) {
                if (x >= 0 && x < r->w && y >= 0 && y < r->h)
                    put_cell(r, x, y, solid ? BOLT_COLOR
                             : blend(cell_color(r, s->cells[y * s->w + x]), BOLT_COLOR));
                if (x == x1 && y == y1)
                    break;
                int e2 = 2 * err;
                if (e2 >= dy) { err += dy; x += sx; }
                if (e2 <= dx) { err += dx; y += sy; }
            }
        }
}

/* Where critter i's carried cell shows: just over its head, on the side
 * it faces. Returns 0 if it isn't carrying anything. */
static int carry_spot(const Render *r, const Sim *s, int i, int *x, int *y)
{
    const Critter *c = &s->crit[i];
    if (c->state == CR_FREE || c->state == CR_AWAY || !c->carry)
        return 0;
    *x = c->x + (c->w > 1 && c->face > 0);
    *y = c->y - c->h;
    return *x >= 0 && *x < r->w && *y >= 0 && *y < r->h;
}

/* Where critter i's last toss is in its flight: an arc up and off the
 * side of the screen over TOSS_STEPS sim steps. Returns 0 once it's gone
 * (or off the screen). */
static int toss_spot(const Render *r, const Sim *s, int i, int *x, int *y)
{
    const Toss *t = &s->toss[i];
    uint32_t age = s->frame - t->frame;
    if (!t->frame || age >= TOSS_STEPS)
        return 0;
    int reach = (t->dir < 0 ? t->x : r->w - t->x) + 2;
    int k = (int)age, n = TOSS_STEPS;
    *x = t->x + t->dir * reach * k / n;
    *y = t->y - TOSS_HEIGHT * 4 * k * (n - k) / (n * n);
    return *x >= 0 && *x < r->w && *y >= 0 && *y < r->h;
}

static void draw_carried(Render *r, const Sim *s)
{
    for (int i = 0; i < MAX_CRITTERS; i++) {
        int x, y;
        if (carry_spot(r, s, i, &x, &y))
            put_cell(r, x, y, cell_color(r, s->crit[i].carry));
        if (toss_spot(r, s, i, &x, &y))
            put_cell(r, x, y, cell_color(r, s->toss[i].cell));
    }
}

/* Redraw the chunk an overlay cell left and the one it's in now, only
 * when it moves. */
static void mark_moved(Render *r, int x, int y, int prev[2])
{
    if (x == prev[0] && y == prev[1])
        return;
    if (prev[0] >= 0)
        r->dirty[(prev[1] >> CHUNK_SHIFT) * r->cw + (prev[0] >> CHUNK_SHIFT)] = 1;
    if (x >= 0)
        r->dirty[(y >> CHUNK_SHIFT) * r->cw + (x >> CHUNK_SHIFT)] = 1;
    prev[0] = x;
    prev[1] = y;
}

/* Carried and tossed cells move with their critters. */
static void mark_carried(Render *r, const Sim *s)
{
    for (int i = 0; i < MAX_CRITTERS; i++) {
        int x, y;
        if (!carry_spot(r, s, i, &x, &y))
            x = y = -1;
        mark_moved(r, x, y, r->carry_at[i]);
        if (!toss_spot(r, s, i, &x, &y))
            x = y = -1;
        mark_moved(r, x, y, r->toss_at[i]);
    }
}

/* Draw the overlays over the chunks in the image and send them. */
static void flush(Render *r, const Sim *s, int show_ring, int show_bolt, int bolt_frame)
{
    if (!r->nruns)
        return;
    if (show_ring)
        draw_ring(r, s, &s->bl);
    draw_carried(r, s);
    if (show_bolt)
        draw_bolt(r, s, bolt_frame);

    int cpx = CHUNK * r->scale;
    for (int k = 0; k < r->nruns; k++) {
        const int *run = r->runs + 4 * k;
        int px = run[0] * cpx, py = run[1] * cpx;
        int pw = run[2] * cpx, ph = cpx;
        int sx = run[3] % r->acw * cpx, sy = run[3] / r->acw * cpx;
        for (int i = 0; i < run[2]; i++)
            r->slot[run[1] * r->cw + run[0] + i] = -1;
        if (px + pw > r->pw) pw = r->pw - px;
        if (py + ph > r->ph) ph = r->ph - py;
        if (pw <= 0 || ph <= 0)
            continue;
        if (r->use_shm)
            XShmPutImage(r->dpy, r->win, r->gc, r->img, sx, sy, px, py, pw, ph, False);
        else
            XPutImage(r->dpy, r->win, r->gc, r->img, sx, sy, px, py, pw, ph);
        r->rects_sent++;
    }
    r->nruns = 0;
}

void render_draw(Render *r, Sim *s)
{
    int n = r->cw * r->ch;
    for (int i = 0; i < n; i++) {
        r->dirty[i] |= s->changed[i];
        s->changed[i] = 0;
    }

    /* The ring moves every frame: redraw where it was and where it is. */
    int show_ring = s->blaster_on && s->bl.radius >= 1.5f;
    int rad = (int)s->bl.radius + 1;
    overlay_box(r, show_ring, (int)s->bl.x - rad, (int)s->bl.y - rad,
                (int)s->bl.x + rad, (int)s->bl.y + rad, r->ring, &r->have_ring);

    /* A new strike shows for BOLT_FRAMES frames. */
    if (s->bolt.id != r->bolt_id) {
        r->bolt_id = s->bolt.id;
        r->bolt_frames = BOLT_FRAMES;
    }
    int show_bolt = r->bolt_frames > 0;
    const int *bb = s->bolt.box;
    overlay_box(r, show_bolt, bb[0], bb[1], bb[2], bb[3], r->bolt_box, &r->have_bolt);

    int bolt_frame = show_bolt ? BOLT_FRAMES + 1 - r->bolt_frames-- : 0;
    mark_carried(r, s);

    /* Pack each horizontal run of dirty chunks into the image, wrapping
     * to the next row of it when one doesn't fit; once it's full, send
     * what's there and start over. */
    int slots = r->acw * r->ach, next = 0, synced = 1;
    for (int cy = 0; cy < r->ch; cy++) {
        uint8_t *row = r->dirty + cy * r->cw;
        for (int cx = 0; cx < r->cw; ) {
            if (!row[cx]) {
                cx++;
                continue;
            }
            int n = 0;
            while (cx + n < r->cw && row[cx + n] && n < r->acw)
                row[cx + n++] = 0;

            if (next % r->acw + n > r->acw)
                next += r->acw - next % r->acw;
            if (next + n > slots) {
                flush(r, s, show_ring, show_bolt, bolt_frame);
                next = 0;
                synced = 0;
            }
            /* Wait for X to finish reading the shared image before it's
             * overwritten. */
            if (!synced) {
                XSync(r->dpy, False);
                synced = 1;
            }
            int *run = r->runs + 4 * r->nruns++;
            run[0] = cx;
            run[1] = cy;
            run[2] = n;
            run[3] = next;
            for (int i = 0; i < n; i++) {
                r->slot[cy * r->cw + cx + i] = next + i;
                draw_chunk(r, s, cx + i, cy, next + i);
            }
            r->chunks_drawn += n;
            next += n;
            cx += n;
        }
    }
    flush(r, s, show_ring, show_bolt, bolt_frame);

    /* And before it's touched again next frame. */
    if (next)
        XSync(r->dpy, False);
}
