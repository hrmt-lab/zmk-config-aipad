#pragma once

#include <zephyr/device.h>

#include <lvgl.h>

#include "screenkey_renderer_model.h"

/* Screens this module has to attach to LVGL itself: every physical ScreenKey
 * except screen 0, which ZMK already drives through chosen zephyr,display. */
#define SCREENKEY_DISPLAY_EXTRA_COUNT (SCREENKEY_RENDERER_SCREEN_COUNT - 1)

/* Registers an additional LVGL display for a Zephyr display device.
 *
 * ZMK's display module and Zephyr's LVGL glue both bind exactly one display,
 * the one in chosen zephyr,display. Every ScreenKey after the first therefore
 * has to be attached to LVGL here. Returns NULL if the device is not ready or
 * LVGL could not create the display. */
lv_display_t *screenkey_display_register(const struct device *display_dev, uint8_t *render_buffer,
                                         size_t render_buffer_size);
