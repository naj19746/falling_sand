#ifndef RENDER_H
#define RENDER_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <stdint.h>

#include "sim.h"

/* Draws straight into the X window, sending only the chunks that changed.
 * Presenting the whole window every frame made X and the compositor
 * redraw the entire screen 30 times a second. X keeps the window's pixels,
 * so the image only has to hold what's being sent: it's a few chunk rows
 * tall, and changed chunks are packed into it, sent, and the next lot
 * packed in after X is done reading. */
typedef struct {
    Display        *dpy;
    Window          win;
    GC              gc;
    XImage         *img;
    XShmSegmentInfo shm;
    int             use_shm;

    /* Pixel color for each cell, by type then color byte. */
    uint32_t        lut[32][256];
    int             w, h;           /* grid size */
    int             pw, ph;         /* window size in pixels */
    int             scale;
    int             cw, ch;         /* grid size in chunks */
    uint8_t        *dirty;          /* per chunk: redraw this frame */
    int             acw, ach;       /* image size in chunks */
    int            *slot;           /* per chunk: where in the image, or -1 */
    int            *runs;           /* runs of chunks in the image: cx, cy, n, slot */
    int             nruns;
    int             ring[4];        /* chunk box the ring covered last frame */
    int             have_ring;
    uint32_t        bolt_id;        /* last strike seen */
    int             bolt_frames;    /* frames left to show it */
    int             bolt_box[4];    /* chunk box it covered last frame */
    int             have_bolt;
    int             carry_at[MAX_CRITTERS][2];  /* cell a carried cell was drawn at, or -1 */

    /* --profile counters, reset by the caller */
    long            chunks_drawn;
    long            rects_sent;
} Render;

/* Names accepted by render_init, NULL-terminated. */
extern const char *const render_palettes[];

int  render_init(Render *r, unsigned long xwin, int w, int h, int scale,
                 const char *palette);
void render_free(Render *r);
void render_invalidate(Render *r);          /* redraw everything next frame */
void render_draw(Render *r, Sim *s);

#endif
