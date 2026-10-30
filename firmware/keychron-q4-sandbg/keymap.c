/* Copyright 2022 @ Keychron (https://www.keychron.com)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* Keychron Q4 keymap with sandbg's per-key LED protocol.
 *
 * sandbg talks to the raw HID interface (usage page 0xFF60, usage 0x61)
 * with 32-byte reports. Stock VIA firmware can't set individual LEDs, so
 * this keymap adds a small custom effect that copies colors received over
 * raw HID. After 3 seconds without a report it gives the LEDs back to the
 * effect that was running before.
 *
 * See README.md in this directory. The keymap below is Keychron's stock Q4
 * keymap (keymaps/keychron) with the remaps that were saved on this
 * keyboard through VIA: IJKL arrows and Print Screen on Fn+. on Windows,
 * screenshot on Fn+L on Mac, and NKRO toggle on Fn+N.
 */

#include QMK_KEYBOARD_H
#include "keychron_common.h"
#include "raw_hid.h"
#include "timer.h"
#include <string.h>

#ifndef RGB_MATRIX_LED_COUNT
#    define RGB_MATRIX_LED_COUNT DRIVER_LED_TOTAL
#endif

#define SB_HELLO0     'S'
#define SB_HELLO1     'B'
#define SB_VERSION    1
#define SB_CMD_PING   'P'
#define SB_CMD_COLOR  'C'
#define SB_CMD_STOP   'R'
#define SB_CHUNK      8      /* report: 6 header + 8*3 colors + checksum */
#define SB_TIMEOUT_MS 3000

static bool     sb_active;
static bool     sb_engaged;
static bool     sb_was_enabled;
static uint8_t  sb_saved_mode;
static hsv_t    sb_saved_hsv;
static uint32_t sb_last_report;

/* Written by raw_hid_receive(), read by the custom effect. */
uint8_t sandbg_leds[RGB_MATRIX_LED_COUNT][3];

static uint8_t sb_sum(const uint8_t *b, uint8_t n)
{
    uint8_t sum = 0;
    for (uint8_t i = 0; i < n; i++)
        sum += b[i];
    return sum;
}

static void sandbg_receive(uint8_t *data, uint8_t length)
{
    if (length < 32)
        return;
    if (data[0] != SB_HELLO0 || data[1] != SB_HELLO1 ||
        data[2] != SB_VERSION || sb_sum(data, 31) != data[31])
        return;

    if (data[3] == SB_CMD_PING) {
        uint8_t out[32];
        memset(out, 0, sizeof out);
        out[0] = SB_HELLO0;
        out[1] = SB_HELLO1;
        out[2] = SB_VERSION;
        out[3] = SB_CMD_PING;
        out[4] = SB_VERSION;
        out[5] = (uint8_t)RGB_MATRIX_LED_COUNT;
        out[6] = SB_CHUNK;
        out[31] = sb_sum(out, 31);
        raw_hid_send(out, (uint8_t)sizeof out);
    } else if (data[3] == SB_CMD_COLOR) {
        uint8_t start = data[4];
        uint8_t count = data[5];
        if (count > SB_CHUNK || start >= RGB_MATRIX_LED_COUNT ||
            count > RGB_MATRIX_LED_COUNT - start)
            return;
        for (uint8_t i = 0; i < count; i++) {
            sandbg_leds[start + i][0] = data[6 + i * 3];
            sandbg_leds[start + i][1] = data[7 + i * 3];
            sandbg_leds[start + i][2] = data[8 + i * 3];
        }
        sb_active = true;
        sb_last_report = timer_read32();
    } else if (data[3] == SB_CMD_STOP) {
        sb_active = false;
    }
}

/* QMK 2025 added a src argument (RAW_HID_SRC_USB) to this callback. Keychron's
 * current tree has it; older QMK trees don't. */
#ifdef RAW_HID_SRC_USB
void raw_hid_receive(uint8_t src, uint8_t *data, uint8_t length)
{
    (void)src;
    sandbg_receive(data, length);
}
#else
void raw_hid_receive(uint8_t *data, uint8_t length)
{
    sandbg_receive(data, length);
}
#endif

/* Take the matrix over while sandbg is talking, and hand it back with the
 * previous effect and brightness when it stops. */
void housekeeping_task_user(void)
{
    if (sb_active && timer_elapsed32(sb_last_report) > SB_TIMEOUT_MS)
        sb_active = false;

    if (sb_active && !sb_engaged) {
        sb_saved_mode = rgb_matrix_get_mode();
        sb_saved_hsv = rgb_matrix_get_hsv();
        sb_was_enabled = rgb_matrix_is_enabled();
        if (!sb_was_enabled)
            rgb_matrix_enable_noeeprom();
        rgb_matrix_sethsv_noeeprom(sb_saved_hsv.h, sb_saved_hsv.s, 255);
        rgb_matrix_mode_noeeprom(RGB_MATRIX_CUSTOM_sandbg);
        sb_engaged = true;
    } else if (!sb_active && sb_engaged) {
        rgb_matrix_mode_noeeprom(sb_saved_mode);
        rgb_matrix_sethsv_noeeprom(sb_saved_hsv.h, sb_saved_hsv.s, sb_saved_hsv.v);
        if (!sb_was_enabled)
            rgb_matrix_disable_noeeprom();
        sb_engaged = false;
    } else if (sb_active && sb_engaged) {
        /* Lights-out or an effect key must not steal the matrix back. */
        if (!rgb_matrix_is_enabled())
            rgb_matrix_enable_noeeprom();
        if (rgb_matrix_get_mode() != RGB_MATRIX_CUSTOM_sandbg)
            rgb_matrix_mode_noeeprom(RGB_MATRIX_CUSTOM_sandbg);
    }
}

enum layers {
    MAC_BASE,
    WIN_BASE,
    MAC_FN,
    WIN_FN,
    _FN2,
};

#define FN_WIN MO(MAC_FN)
#define FN_MAC MO(WIN_FN)
#define FN2 MO(_FN2)

// clang-format off
const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [MAC_BASE] = LAYOUT_ansi_61(
        KC_ESC,   KC_1,     KC_2,     KC_3,     KC_4,     KC_5,     KC_6,     KC_7,     KC_8,     KC_9,     KC_0,     KC_MINS,  KC_EQL,   KC_BSPC,
        KC_TAB,   KC_Q,     KC_W,     KC_E,     KC_R,     KC_T,     KC_Y,     KC_U,     KC_I,     KC_O,     KC_P,     KC_LBRC,  KC_RBRC,  KC_BSLS,
        KC_CAPS,  KC_A,     KC_S,     KC_D,     KC_F,     KC_G,     KC_H,     KC_J,     KC_K,     KC_L,     KC_SCLN,  KC_QUOT,            KC_ENT,
        KC_LSFT,            KC_Z,     KC_X,     KC_C,     KC_V,     KC_B,     KC_N,     KC_M,     KC_COMM,  KC_DOT,   KC_SLSH,            KC_RSFT,
        KC_LCTL,  KC_LOPTN, KC_LCMMD,                               KC_SPC,                                 KC_RCMMD, FN_WIN,   FN2,      KC_RCTL),

    [WIN_BASE] = LAYOUT_ansi_61(
        KC_ESC,   KC_1,     KC_2,     KC_3,     KC_4,     KC_5,     KC_6,     KC_7,     KC_8,     KC_9,     KC_0,     KC_MINS,  KC_EQL,   KC_BSPC,
        KC_TAB,   KC_Q,     KC_W,     KC_E,     KC_R,     KC_T,     KC_Y,     KC_U,     KC_I,     KC_O,     KC_P,     KC_LBRC,  KC_RBRC,  KC_BSLS,
        KC_CAPS,  KC_A,     KC_S,     KC_D,     KC_F,     KC_G,     KC_H,     KC_J,     KC_K,     KC_L,     KC_SCLN,  KC_QUOT,            KC_ENT,
        KC_LSFT,            KC_Z,     KC_X,     KC_C,     KC_V,     KC_B,     KC_N,     KC_M,     KC_COMM,  KC_DOT,   KC_SLSH,            KC_RSFT,
        KC_LCTL,  KC_LGUI,  KC_LALT,                                KC_SPC,                                 KC_RALT,  FN_MAC,   FN2,      KC_RCTL),

    [MAC_FN] = LAYOUT_ansi_61(
        KC_GRV,   KC_BRID,  KC_BRIU,  KC_MCTL,  KC_LPAD,  UG_VALD,  UG_VALU,  KC_MPRV,  KC_MPLY,  KC_MNXT,  KC_MUTE,  KC_VOLD,  KC_VOLU,  UG_NEXT,
        UG_TOGG,  UG_NEXT,  UG_VALU,  UG_HUEU,  UG_SATU,  UG_SPDU,  _______,  _______,  _______,  _______,  KC_INS,   KC_PGUP,  KC_HOME,  _______,
        _______,  UG_PREV,  UG_VALD,  UG_HUED,  UG_SATD,  UG_SPDD,  _______,  _______,  KC_UP,    KC_SNAP,  KC_PGDN,  KC_END,             _______,
        _______,            _______,  _______,  _______,  _______,  _______,  NK_TOGG,  KC_LEFT,  KC_DOWN,  KC_RIGHT, KC_DEL,             _______,
        _______,  _______,  _______,                                _______,                                _______,  _______,  _______,  _______),

    [WIN_FN] = LAYOUT_ansi_61(
        KC_GRV,   KC_BRID,  KC_BRIU,  KC_TASK,  KC_FILE,  UG_VALD,  UG_VALU,  KC_MPRV,  KC_MPLY,  KC_MNXT,  KC_MUTE,  KC_VOLD,  KC_VOLU,  UG_NEXT,
        UG_TOGG,  UG_NEXT,  UG_VALU,  UG_HUEU,  UG_SATU,  UG_SPDU,  _______,  _______,  KC_UP,    KC_SCRL,  KC_INS,   KC_PGUP,  KC_HOME,  _______,
        _______,  UG_PREV,  UG_VALD,  UG_HUED,  UG_SATD,  UG_SPDD,  _______,  KC_LEFT,  KC_DOWN,  KC_RGHT,  KC_PGDN,  KC_END,             _______,
        _______,            _______,  _______,  _______,  _______,  _______,  NK_TOGG,  _______,  _______,  KC_PSCR,  KC_DEL,             _______,
        _______,  _______,  _______,                                _______,                                _______,  _______,  _______,  _______),

    [_FN2] = LAYOUT_ansi_61(
        KC_TILD,  KC_F1,    KC_F2,    KC_F3,    KC_F4,    KC_F5,    KC_F6,    KC_F7,    KC_F8,    KC_F9,    KC_F10,   KC_F11,   KC_F12,   _______,
        UG_TOGG,  UG_NEXT,  UG_VALU,  UG_HUEU,  UG_SATU,  UG_SPDU,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,
        _______,  UG_PREV,  UG_VALD,  UG_HUED,  UG_SATD,  UG_SPDD,  _______,  _______,  _______,  _______,  _______,  _______,            _______,
        _______,            _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,            _______,
        _______,  _______,  _______,                                _______,                                _______,  _______,  _______,  _______)
};

