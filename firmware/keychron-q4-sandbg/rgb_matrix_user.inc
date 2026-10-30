/* Custom RGB matrix effect that shows the colors sandbg sends over raw HID.
 * Do not add #pragma once: QMK includes this file more than once. */

RGB_MATRIX_EFFECT(sandbg)

#ifdef RGB_MATRIX_CUSTOM_EFFECT_IMPLS
extern uint8_t sandbg_leds[RGB_MATRIX_LED_COUNT][3];

static bool sandbg(effect_params_t *params)
{
    RGB_MATRIX_USE_LIMITS(led_min, led_max);
    for (uint8_t i = led_min; i < led_max; i++)
        rgb_matrix_set_color(i, sandbg_leds[i][0], sandbg_leds[i][1],
                             sandbg_leds[i][2]);
    return rgb_matrix_check_finished_leds(led_max);
}
#endif
