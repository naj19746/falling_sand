#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

#include "sim.h"

/* Per-key LEDs on a Keychron Q4. Each key samples the part of the screen
 * it sits over: piles glow amber and brighten as they grow, drizzle
 * flashes single keys while steady rain glows across the board, moss grows
 * as green spots, and a strike flashes the lot.
 *
 * The keyboard needs the sandbg QMK keymap: stock VIA firmware only exposes
 * whole-keyboard effects. See README.md. */
#define KB_MAX_LEDS 64

enum { KB_OFF, KB_HID, KB_PREVIEW };

typedef struct {
    int backend;
    int w, h;                   /* sim grid size */
    int n_layout;               /* keys in the layout table */
    int n_leds;                 /* LEDs the firmware reports */
    float brightness;           /* 0-1, from the config */

    uint8_t leds[KB_MAX_LEDS][3];
    uint8_t sent[KB_MAX_LEDS][3];
    float sand_trail[KB_MAX_LEDS];  /* decaying falling-sand glow */
    float rain_trail[KB_MAX_LEDS];  /* decaying falling-water glow */
    float flash;                    /* lightning, fades fast */
    float rain_level;               /* smoothed falling drops per column */
    uint8_t gamma[256];             /* LED values for perceived brightness */

    uint32_t area[KB_MAX_LEDS];     /* screen cells sampled by each key */
    int16_t *rowkey;                /* 5 * w: key index by row and screen x */
    uint8_t *row_at;                /* h: keyboard row by screen y */
    uint8_t *support;               /* w: scratch for the column scan */

    uint32_t bolt_id;
    int have_bolt;
    uint32_t last_scan, last_send, next_try;

    int fd;
    int connected;
    int warned_no_firmware;
    int warned_permission;
    int warned_disconnected;
    char device[64];                /* "" = find a Keychron */
} Keyboard;

/* mode is "auto", "off" or "preview". Returns 0 even when no keyboard is
 * found: the rest of sandbg carries on. */
int  keyboard_init(Keyboard *k, int w, int h, const char *mode,
                   const char *device, float brightness);
void keyboard_free(Keyboard *k);
void keyboard_update(Keyboard *k, const Sim *s, uint32_t now_ms);
void keyboard_hold(Keyboard *k, const Sim *s, uint32_t now_ms);  /* keepalive while covered */

/* --keyboard: list the Keychron hidraw devices and whether the sandbg
 * keymap is answering. Returns 0 if a device was found. */
int  keyboard_probe(const char *device);

#endif
