#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>

#include "screenkey_display.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Per-display context stored in the LVGL display's user data so the flush
 * callback can reach the Zephyr display device. Zephyr's own LVGL glue keeps an
 * equivalent struct, but only for chosen zephyr,display. */
struct screenkey_display_context {
    const struct device *display_dev;
};

static struct screenkey_display_context contexts[SCREENKEY_DISPLAY_EXTRA_COUNT];
static size_t context_count;

static void screenkey_display_flush_cb(lv_display_t *display, const lv_area_t *area,
                                       uint8_t *px_map) {
    const struct screenkey_display_context *context =
        (const struct screenkey_display_context *)lv_display_get_user_data(display);
    const uint16_t width = area->x2 - area->x1 + 1;
    const uint16_t height = area->y2 - area->y1 + 1;

    const struct display_buffer_descriptor descriptor = {
        .buf_size = (uint32_t)width * 2U * height,
        .width = width,
        .pitch = width,
        .height = height,
        .frame_incomplete = !lv_display_flush_is_last(display),
    };

    display_write(context->display_dev, area->x1, area->y1, &descriptor, px_map);
    lv_display_flush_ready(display);
}

lv_display_t *screenkey_display_register(const struct device *display_dev, uint8_t *render_buffer,
                                         size_t render_buffer_size) {
    if (!device_is_ready(display_dev)) {
        LOG_ERR("ScreenKey display device not ready");
        return NULL;
    }

    if (context_count >= ARRAY_SIZE(contexts)) {
        LOG_ERR("no ScreenKey display context left");
        return NULL;
    }

    struct display_capabilities capabilities;
    display_get_capabilities(display_dev, &capabilities);

    if (capabilities.current_pixel_format != PIXEL_FORMAT_RGB_565 &&
        capabilities.current_pixel_format != PIXEL_FORMAT_BGR_565) {
        LOG_ERR("unsupported ScreenKey pixel format %d", capabilities.current_pixel_format);
        return NULL;
    }

    lv_display_t *display =
        lv_display_create(capabilities.x_resolution, capabilities.y_resolution);
    if (display == NULL) {
        LOG_ERR("failed to create ScreenKey LVGL display");
        return NULL;
    }

    struct screenkey_display_context *context = &contexts[context_count++];
    context->display_dev = display_dev;

    lv_display_set_user_data(display, context);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, screenkey_display_flush_cb);
    lv_display_set_buffers(display, render_buffer, NULL, render_buffer_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    display_blanking_off(display_dev);

    return display;
}
