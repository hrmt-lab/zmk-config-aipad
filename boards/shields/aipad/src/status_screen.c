#include <stdbool.h>
#include <stdint.h>

#include <lvgl.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <rawhid_app/ai_client_state.h>
#include <rawhid_app/events/ai_client_state_changed.h>
#include <zmk/display.h>
#include <zmk/event_manager.h>

#include "claude_code_logo.h"
#include "codex_logo.h"
#include "screenkey_display.h"
#include "screenkey_renderer_model.h"

LOG_MODULE_REGISTER(screenkey_renderer, CONFIG_ZMK_LOG_LEVEL);

#define BACKLIGHT_NODE DT_NODELABEL(aipad_backlight)
#define BORDER_OBJECT_COUNT 4
#define ANIMATION_PERIOD_MS 100
#define BLINK_PERIOD_MS 1000
#define PANEL_SIZE 128

/* Partial render buffer for each screen this file drives. Screen 0 keeps using
 * the buffer Zephyr's LVGL glue allocates for chosen zephyr,display. Sixteen
 * full lines of RGB565 is the same order of magnitude as the default
 * LV_Z_VDB_SIZE for this panel; with four ScreenKeys that is three buffers of
 * 4 KB. Drop the line count first if RAM gets tight. */
#define EXTRA_BUFFER_LINES 16
#define EXTRA_BUFFER_SIZE (PANEL_SIZE * EXTRA_BUFFER_LINES * 2)

/* Everything that used to be a single set of file statics is now per physical
 * ScreenKey, so every slot animates and clears completely independently. */
struct screenkey_screen {
    bool backlight_requested;
    lv_obj_t *logo;
    lv_obj_t *full_border[BORDER_OBJECT_COUNT];
    lv_obj_t *working_segments[SCREENKEY_RENDERER_MAX_SEGMENTS];
    lv_timer_t *state_timer;
    enum screenkey_renderer_mode current_mode;
    uint8_t animation_frame;
    bool blink_visible;
    uint32_t display_generation;
    uint32_t timer_generation;
};

struct screenkey_event_state {
    struct rawhid_app_ai_client_state state;
    uint32_t state_generation;
    enum rawhid_app_ai_client_state_event_reason reason;
};

/* SCREENKEY_RENDERER_OFF is the first enumerator, so the implicit zero
 * initialisation already puts every screen in the off state. Spelling out one
 * initialiser per screen would have to be kept in step with
 * SCREENKEY_RENDERER_SCREEN_COUNT by hand. */
static struct screenkey_screen screens[SCREENKEY_RENDERER_SCREEN_COUNT];

/* The panels this file attaches to LVGL itself, in display slot order: entry n
 * is screen n + 1. Screen 0 is chosen zephyr,display and ZMK owns it. */
static const struct device *const extra_lcds[SCREENKEY_DISPLAY_EXTRA_COUNT] = {
    DEVICE_DT_GET(DT_NODELABEL(aipad_lcd1)),
    DEVICE_DT_GET(DT_NODELABEL(aipad_lcd2)),
    DEVICE_DT_GET(DT_NODELABEL(aipad_lcd3)),
};

static uint8_t extra_buffers[SCREENKEY_DISPLAY_EXTRA_COUNT][EXTRA_BUFFER_SIZE] __aligned(4);

/* All four panels share one PWM backlight channel, so it stays on while any
 * screen still has something to show and only goes dark once every screen is
 * off. */
static const struct device *const backlight = DEVICE_DT_GET(DT_PARENT(BACKLIGHT_NODE));
static const uint8_t backlight_index = DT_NODE_CHILD_IDX(BACKLIGHT_NODE);

#if IS_ENABLED(CONFIG_AIPAD_DISPLAY_SELFTEST)
/* Full brightness during bring-up so a working but dim backlight is not read as
 * a dead one. 100 is the ceiling of the LED API: led_pwm turns it into a pulse
 * as long as the period, and pwm_nrfx then holds P0.15 statically high instead
 * of running the PWM peripheral at all. Nothing in software can go brighter -
 * the panels' actual light output is set by the PAM2804 current sense resistors
 * R2 / R5 / R10 / R12 (5R6). */
#define BACKLIGHT_BRIGHTNESS 100

/* The beacon is deliberately slow. With the LED current set low in hardware the
 * panels are dim even at 100%, and a fast blink on a dim backlight is easy to
 * mistake for nothing at all. A full second per phase over six cycles gives
 * twelve seconds to catch it and removes the "was that a flicker?" doubt. */
#define BEACON_CYCLES 6
#define BEACON_PHASE_MS 1000
#else
#define BACKLIGHT_BRIGHTNESS 39
#endif

static void set_backlight(struct screenkey_screen *screen, bool enabled) {
    screen->backlight_requested = enabled;

    if (!device_is_ready(backlight)) {
        LOG_ERR("ScreenKey backlight device is not ready");
        return;
    }

    bool any_requested = false;
    for (size_t index = 0; index < SCREENKEY_RENDERER_SCREEN_COUNT; index++) {
        any_requested = any_requested || screens[index].backlight_requested;
    }

    const int err = any_requested
                        ? led_set_brightness(backlight, backlight_index, BACKLIGHT_BRIGHTNESS)
                        : led_off(backlight, backlight_index);
    if (err < 0) {
        LOG_ERR("Failed to update ScreenKey backlight: %d", err);
    }
}

#if IS_ENABLED(CONFIG_AIPAD_DISPLAY_SELFTEST)

/* Backlight beacon: blink slowly at boot, then leave the backlight on.
 *
 * This runs from SYS_INIT and touches only the PWM LED, so it is independent of
 * the display drivers, LVGL, USB and the host. It matters because ZMK's display
 * module returns early when the chosen display is not ready, which means
 * zmk_display_status_screen() is never called and neither the on-screen self
 * test nor its logs ever appear. The beacon still runs in that case, so:
 *
 *   blinks  -> firmware runs and the PWM backlight circuit works; look at the
 *              panel and SPI side next
 *   nothing -> firmware is not running, or the backlight circuit / P0.15 is
 *              the problem; the display is not implicated yet
 */
static int screenkey_backlight_beacon(void) {
    if (!device_is_ready(backlight)) {
        LOG_ERR("ScreenKey backlight device is not ready at boot");
        return -ENODEV;
    }

    int on_err = 0;
    int off_err = 0;

    /* Slow, symmetric blink. Each phase is long enough to be unmistakable, and
     * because 100% duty drives the pin high while off drives it low, the two
     * phases together cover an active-high and an active-low backlight: one of
     * them must light the panel if the LED circuit works at all. The counted log
     * lines let a dim blink be matched against the serial output rather than
     * judged by eye alone. */
    for (int blink = 0; blink < BEACON_CYCLES; blink++) {
        on_err = led_set_brightness(backlight, backlight_index, BACKLIGHT_BRIGHTNESS);
        LOG_INF("ScreenKey backlight beacon %d/%d: P0.15 driven HIGH", blink + 1, BEACON_CYCLES);
        k_msleep(BEACON_PHASE_MS);
        off_err = led_off(backlight, backlight_index);
        LOG_INF("ScreenKey backlight beacon %d/%d: P0.15 driven LOW", blink + 1, BEACON_CYCLES);
        k_msleep(BEACON_PHASE_MS);
    }

    /* Left on so a panel that never reaches the renderer still shows whatever
     * the controller is putting out instead of looking dead. */
    on_err = led_set_brightness(backlight, backlight_index, BACKLIGHT_BRIGHTNESS);

    /* The return codes matter: if the driver accepted every call and the panel
     * is still dark, the PWM pin is doing its job and the fault is downstream
     * in the backlight circuit rather than in software. */
    LOG_INF("ScreenKey backlight beacon finished: brightness=%d%% on_err=%d off_err=%d, "
            "backlight left on",
            BACKLIGHT_BRIGHTNESS, on_err, off_err);
    return 0;
}

SYS_INIT(screenkey_backlight_beacon, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* Pin walk and short check on the eight panel signals.
 *
 * Everything on the MCU side can measure correct while the panel stays dead if
 * the signals never reach it: a broken trace, a cold joint, a solder bridge
 * between two 0.5 mm FPC pads, or an FPC inserted mirrored. Note that the FPC
 * carries GND on both pin 1 and pin 12, so a mirrored insertion still passes a
 * continuity check on those two while VDD lands on DC and the panel never
 * powers up.
 *
 * The short check needs no probing at all: with one pin driven high and the
 * rest held as pulled-down inputs, any other pin reading high is bridged to it.
 * The walk then holds each line high in turn, so a meter on an easier point of
 * the same net identifies it without touching the connector.
 *
 * Runs at POST_KERNEL 45, after the GPIO driver (40) but before SPI (50),
 * MIPI-DBI (80) and the display (85). That ordering is what lets it own SCLK
 * and MOSI, which pinctrl hands to SPIM later, and it makes toggling RESET
 * harmless because the panels are reset and configured afterwards.
 *
 * Port and pin numbers mirror the assignment table at the top of
 * aipad.overlay. */
struct screenkey_walk_pin {
    const struct device *port;
    gpio_pin_t pin;
    const char *name;
};

static const struct screenkey_walk_pin walk_pins[] = {
    {DEVICE_DT_GET(DT_NODELABEL(gpio1)), 5, "DC (P1.05, J1 pin 7)"},
    {DEVICE_DT_GET(DT_NODELABEL(gpio1)), 3, "SCLK (P1.03, J1 pin 5)"},
    {DEVICE_DT_GET(DT_NODELABEL(gpio1)), 12, "MOSI (P1.12/D7, J1 pin 10)"},
    {DEVICE_DT_GET(DT_NODELABEL(gpio0)), 2, "RESET (P0.02/D0, J1 pin 12)"},
    {DEVICE_DT_GET(DT_NODELABEL(gpio1)), 13, "CS1 (P1.13/D8, J1 pin 8)"},
    {DEVICE_DT_GET(DT_NODELABEL(gpio1)), 14, "CS2 (P1.14/D9, J1 pin 6)"},
    {DEVICE_DT_GET(DT_NODELABEL(gpio1)), 7, "CS3 (P1.07, J1 pin 9)"},
    {DEVICE_DT_GET(DT_NODELABEL(gpio1)), 15, "CS4 (P1.15/D10, J1 pin 4)"},
};

/* Time for a driven line to settle before its neighbours are sampled. Reading
 * straight after the edge sees capacitive crosstalk between adjacent 0.5 mm FPC
 * traces, not conduction, and reports it as a short. */
#define WALK_SETTLE_US 1000

/* How long each line is held high, so a meter can be put on an easier point of
 * the same net while the log names which line it is. */
#define WALK_HOLD_MS 1000

static bool walk_pin_reads_high(const struct screenkey_walk_pin *pin) {
    /* Sampled twice with a gap: one high reading can be a coupled transient,
     * two a millisecond apart cannot. */
    if (gpio_pin_get_raw(pin->port, pin->pin) != 1) {
        return false;
    }
    k_busy_wait(WALK_SETTLE_US);
    return gpio_pin_get_raw(pin->port, pin->pin) == 1;
}

/* What the walk found, kept so it can be logged again later.
 *
 * The walk runs at POST_KERNEL 45, long before the USB CDC backend exists, so
 * its log lines never reach the serial port - the first run of this self test
 * lost the whole boot sequence and only a fragment survived. The findings are
 * recorded here and replayed a few times once USB has had a chance to
 * enumerate, where they can actually be read. */
static struct {
    uint32_t unjudgeable;
    uint32_t shorted_to[ARRAY_SIZE(walk_pins)];
    bool ran;
} walk_result;

#define WALK_REPORT_REPEATS 3
#define WALK_REPORT_INTERVAL K_SECONDS(10)

static void walk_report_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(walk_report, walk_report_cb);

static void walk_report_cb(struct k_work *work) {
    ARG_UNUSED(work);
    static uint8_t reports;

    LOG_INF("--- ScreenKey boot report %u/%u (replayed for the USB log) ---", reports + 1,
            WALK_REPORT_REPEATS);

    LOG_INF("ScreenKey backlight: %s", device_is_ready(backlight) ? "ready" : "NOT READY");
    LOG_INF("ScreenKey lcd0: %s",
            device_is_ready(DEVICE_DT_GET(DT_CHOSEN(zephyr_display))) ? "ready" : "NOT READY");
    for (size_t index = 0; index < SCREENKEY_DISPLAY_EXTRA_COUNT; index++) {
        LOG_INF("ScreenKey lcd%u: %s", (unsigned int)(index + 1),
                device_is_ready(extra_lcds[index]) ? "ready" : "NOT READY");
    }

    if (walk_result.ran) {
        bool shorted = false;

        for (size_t index = 0; index < ARRAY_SIZE(walk_pins); index++) {
            if (walk_result.unjudgeable & BIT(index)) {
                LOG_WRN("ScreenKey short check: %s has an external pull-up and was not judged",
                        walk_pins[index].name);
            }
            for (size_t other = 0; other < ARRAY_SIZE(walk_pins); other++) {
                if (walk_result.shorted_to[index] & BIT(other)) {
                    LOG_ERR("ScreenKey short check: %s is shorted to %s", walk_pins[index].name,
                            walk_pins[other].name);
                    shorted = true;
                }
            }
        }

        LOG_INF("ScreenKey short check: %s",
                shorted ? "SHORTS FOUND, see errors above" : "no shorts between judgeable pins");
    } else {
        LOG_WRN("ScreenKey pin walk did not run");
    }

    if (++reports < WALK_REPORT_REPEATS) {
        k_work_schedule(&walk_report, WALK_REPORT_INTERVAL);
    }
}

static int screenkey_pin_walk(void) {
    bool unjudgeable[ARRAY_SIZE(walk_pins)] = {false};
    bool shorted = false;

    for (size_t index = 0; index < ARRAY_SIZE(walk_pins); index++) {
        if (!device_is_ready(walk_pins[index].port)) {
            LOG_ERR("ScreenKey pin walk: port for %s not ready", walk_pins[index].name);
            return -ENODEV;
        }
        gpio_pin_configure(walk_pins[index].port, walk_pins[index].pin,
                           GPIO_INPUT | GPIO_PULL_DOWN);
    }

    /* Baseline pass. Nothing is driven yet, so every line should read low. One
     * that still reads high has an external pull-up fighting the internal
     * pull-down: each CS line carries a 10K to VCC_3V3 (R1 / R3 / R9 / R11),
     * which against the nRF52840's ~13K pull-down sits near 1.9 V. That is
     * inside the indeterminate band between V_IL (0.99 V) and V_IH (2.31 V), so
     * the pin reads high or low depending on nothing in particular. Such a line
     * cannot be judged by this test; it is named once here and then left out of
     * the short check instead of being reported against every driven pin. */
    k_msleep(10);
    for (size_t index = 0; index < ARRAY_SIZE(walk_pins); index++) {
        if (walk_pin_reads_high(&walk_pins[index])) {
            unjudgeable[index] = true;
            walk_result.unjudgeable |= BIT(index);
            LOG_WRN("ScreenKey short check: %s reads high with nothing driven; it has an "
                    "external pull-up and is excluded from the short check",
                    walk_pins[index].name);
        }
    }

    for (size_t index = 0; index < ARRAY_SIZE(walk_pins); index++) {
        gpio_pin_configure(walk_pins[index].port, walk_pins[index].pin, GPIO_OUTPUT_LOW);
        gpio_pin_set_raw(walk_pins[index].port, walk_pins[index].pin, 1);
        k_busy_wait(WALK_SETTLE_US);

        for (size_t other = 0; other < ARRAY_SIZE(walk_pins); other++) {
            if (other == index || unjudgeable[other]) {
                continue;
            }
            if (walk_pin_reads_high(&walk_pins[other])) {
                walk_result.shorted_to[index] |= BIT(other);
                LOG_ERR("ScreenKey short check: %s is shorted to %s", walk_pins[index].name,
                        walk_pins[other].name);
                shorted = true;
            }
        }

        LOG_INF("ScreenKey pin walk: driving %s HIGH, all others LOW", walk_pins[index].name);
        k_msleep(WALK_HOLD_MS);

        gpio_pin_set_raw(walk_pins[index].port, walk_pins[index].pin, 0);
        gpio_pin_configure(walk_pins[index].port, walk_pins[index].pin,
                           GPIO_INPUT | GPIO_PULL_DOWN);
    }

    LOG_INF("ScreenKey short check: %s",
            shorted ? "SHORTS FOUND, see errors above" : "no shorts between judgeable pins");
    LOG_INF("ScreenKey pin walk finished; panel drivers take the pins from here");

    walk_result.ran = true;
    k_work_schedule(&walk_report, WALK_REPORT_INTERVAL);
    return 0;
}

SYS_INIT(screenkey_pin_walk, POST_KERNEL, 45);

#endif /* CONFIG_AIPAD_DISPLAY_SELFTEST */

static void set_object_color(lv_obj_t *object, lv_color_t color) {
    lv_obj_set_style_bg_color(object, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, LV_PART_MAIN);
}

static void set_object_color_and_opacity(lv_obj_t *object, lv_color_t color, uint8_t opacity) {
    lv_obj_set_style_bg_color(object, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(object, opacity, LV_PART_MAIN);
}

static void set_logo_for_client_type(struct screenkey_screen *screen, uint8_t client_type) {
    const bool claude_code = screenkey_renderer_logo_for_client_type(client_type) ==
                             SCREENKEY_RENDERER_LOGO_CLAUDE_CODE;

    lv_image_set_src(screen->logo,
                     claude_code ? &screenkey_claude_code_logo : &screenkey_codex_logo);
}

static void set_hidden(lv_obj_t *object, bool hidden) {
    if (hidden) {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

static void hide_full_border(struct screenkey_screen *screen) {
    for (size_t index = 0; index < BORDER_OBJECT_COUNT; index++) {
        set_hidden(screen->full_border[index], true);
    }
}

static void show_full_border(struct screenkey_screen *screen, lv_color_t color, bool visible) {
    for (size_t index = 0; index < BORDER_OBJECT_COUNT; index++) {
        set_object_color(screen->full_border[index], color);
        set_hidden(screen->full_border[index], !visible);
    }
}

static void show_breathing_border(struct screenkey_screen *screen, lv_color_t color) {
    const uint8_t opacity = screenkey_renderer_breath_opacity(screen->animation_frame);

    for (size_t index = 0; index < BORDER_OBJECT_COUNT; index++) {
        set_object_color_and_opacity(screen->full_border[index], color, opacity);
        set_hidden(screen->full_border[index], false);
    }
}

static void hide_working_segments(struct screenkey_screen *screen) {
    for (size_t index = 0; index < SCREENKEY_RENDERER_MAX_SEGMENTS; index++) {
        set_hidden(screen->working_segments[index], true);
    }
}

static void render_working_frame(struct screenkey_screen *screen) {
    struct screenkey_renderer_segment segments[SCREENKEY_RENDERER_MAX_SEGMENTS] = {0};
    const size_t count = screenkey_renderer_working_segments(screen->animation_frame, segments);
    const lv_color_t blue = lv_color_hex(0x3B82F6);

    hide_working_segments(screen);
    for (size_t index = 0; index < count; index++) {
        lv_obj_set_pos(screen->working_segments[index], segments[index].x, segments[index].y);
        lv_obj_set_size(screen->working_segments[index], segments[index].width,
                        segments[index].height);
        set_object_color(screen->working_segments[index], blue);
        set_hidden(screen->working_segments[index], false);
    }
}

static void stop_state_timer(struct screenkey_screen *screen) {
    if (screen->state_timer != NULL) {
        lv_timer_del(screen->state_timer);
        screen->state_timer = NULL;
    }
}

static void state_timer_cb(lv_timer_t *timer) {
    struct screenkey_screen *screen = (struct screenkey_screen *)lv_timer_get_user_data(timer);

    if (screen->timer_generation != screen->display_generation) {
        return;
    }

    switch (screen->current_mode) {
    case SCREENKEY_RENDERER_WORKING_MOVING:
        screen->animation_frame = (screen->animation_frame + 1) % SCREENKEY_RENDERER_FRAME_COUNT;
        render_working_frame(screen);
        break;
    case SCREENKEY_RENDERER_WAITING_INPUT:
        screen->animation_frame = (screen->animation_frame + 1) % SCREENKEY_RENDERER_FRAME_COUNT;
        show_breathing_border(screen, lv_color_hex(0xF97316));
        break;
    case SCREENKEY_RENDERER_WAITING_APPROVAL:
    case SCREENKEY_RENDERER_ERROR:
        screen->blink_visible = !screen->blink_visible;
        show_full_border(screen,
                         screen->current_mode == SCREENKEY_RENDERER_ERROR ? lv_color_hex(0xEF4444)
                                                                         : lv_color_hex(0xFACC15),
                         screen->blink_visible);
        break;
    case SCREENKEY_RENDERER_COMPLETED:
        hide_full_border(screen);
        screen->state_timer = NULL;
        break;
    default:
        break;
    }
}

static void start_timer(struct screenkey_screen *screen, uint32_t period_ms, bool one_shot) {
    screen->timer_generation = screen->display_generation;
    screen->state_timer = lv_timer_create(state_timer_cb, period_ms, screen);
    if (one_shot) {
        lv_timer_set_repeat_count(screen->state_timer, 1);
    }
}

#if IS_ENABLED(CONFIG_AIPAD_DISPLAY_SELFTEST)
static void render_selftest(struct screenkey_screen *screen, uint8_t index);
#endif

static void screenkey_render(struct screenkey_screen *screen, uint8_t display_slot,
                             struct screenkey_event_state event) {
    screen->display_generation++;
    screen->current_mode = screenkey_renderer_mode_for_state(
        event.state.session_active, event.state.activity_state, event.state.work_phase);

    stop_state_timer(screen);
    hide_full_border(screen);
    hide_working_segments(screen);

    if (screen->current_mode == SCREENKEY_RENDERER_OFF) {
#if IS_ENABLED(CONFIG_AIPAD_DISPLAY_SELFTEST)
        /* During bring-up the host reporting "no session" must not blank the
         * panel: an unlit screen and a broken screen look identical, and it
         * also drops the backlight pin back to 0 V, which makes a multimeter
         * reading on it meaningless. Keep the test pattern up instead. */
        LOG_INF("ScreenKey AI client display would clear: slot=%u reason=%u generation=%u; "
                "self test keeps the panel lit",
                display_slot, event.reason, event.state_generation);
        render_selftest(screen, display_slot);
        return;
#else
        set_hidden(screen->logo, true);
        set_backlight(screen, false);
        LOG_INF("ScreenKey AI client display cleared: slot=%u reason=%u generation=%u", display_slot,
                event.reason, event.state_generation);
        return;
#endif
    }

    set_logo_for_client_type(screen, event.state.client_type);
    set_hidden(screen->logo, false);
    set_backlight(screen, true);

    switch (screen->current_mode) {
    case SCREENKEY_RENDERER_AVAILABLE:
        break;
    case SCREENKEY_RENDERER_WORKING_MOVING:
        screen->animation_frame = 0;
        render_working_frame(screen);
        start_timer(screen, ANIMATION_PERIOD_MS, false);
        break;
    case SCREENKEY_RENDERER_WAITING_APPROVAL:
        screen->blink_visible = true;
        show_full_border(screen, lv_color_hex(0xFACC15), true);
        start_timer(screen, BLINK_PERIOD_MS, false);
        break;
    case SCREENKEY_RENDERER_WAITING_INPUT:
        screen->animation_frame = 0;
        show_breathing_border(screen, lv_color_hex(0xF97316));
        start_timer(screen, ANIMATION_PERIOD_MS, false);
        break;
    case SCREENKEY_RENDERER_COMPLETED:
        show_full_border(screen, lv_color_hex(0x22C55E), true);
        /* Shared with the LED hold in status_led.c so the border and the
         * chain always go dark together. */
        start_timer(screen, SCREENKEY_COMPLETED_HOLD_MS, true);
        break;
    case SCREENKEY_RENDERER_ERROR:
        screen->blink_visible = true;
        show_full_border(screen, lv_color_hex(0xEF4444), true);
        start_timer(screen, BLINK_PERIOD_MS, false);
        break;
    default:
        break;
    }

    LOG_INF("ScreenKey AI client display updated: slot=%u client=%u activity=%u phase=%u "
            "revision=%u generation=%u",
            display_slot, event.state.client_type, event.state.activity_state,
            event.state.work_phase, event.state.revision, event.state_generation);
}

/* Per-slot pending state. ZMK_DISPLAY_WIDGET_LISTENER keeps a single coalesced
 * snapshot, which would let a slot 1 event overwrite an unprocessed slot 0
 * event, so the listener is spelled out here with one pending entry per screen
 * instead. The threading contract is the same: the ZMK event callback only
 * records state, and all LVGL work happens on the display work queue. */
static K_MUTEX_DEFINE(pending_mutex);
static struct screenkey_event_state pending_state[SCREENKEY_RENDERER_SCREEN_COUNT];
static bool pending_dirty[SCREENKEY_RENDERER_SCREEN_COUNT];

static void screenkey_work_cb(struct k_work *work) {
    ARG_UNUSED(work);

    for (uint8_t index = 0; index < SCREENKEY_RENDERER_SCREEN_COUNT; index++) {
        struct screenkey_event_state event;
        bool dirty;

        k_mutex_lock(&pending_mutex, K_FOREVER);
        dirty = pending_dirty[index];
        event = pending_state[index];
        pending_dirty[index] = false;
        k_mutex_unlock(&pending_mutex);

        if (dirty) {
            screenkey_render(&screens[index], index, event);
        }
    }
}

K_WORK_DEFINE(screenkey_work, screenkey_work_cb);

static int screenkey_ai_state_cb(const zmk_event_t *eh) {
    if (!zmk_display_is_initialized()) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct rawhid_app_ai_client_state_changed *changed =
        as_rawhid_app_ai_client_state_changed(eh);
    if (changed == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    const int screen_index = screenkey_renderer_screen_for_slot(changed->display_slot);
    if (screen_index == SCREENKEY_RENDERER_NO_SCREEN) {
        LOG_DBG("ignoring AI client state for slot %u: no screen on this keyboard",
                changed->display_slot);
        return ZMK_EV_EVENT_BUBBLE;
    }

    k_mutex_lock(&pending_mutex, K_FOREVER);
    pending_state[screen_index] = (struct screenkey_event_state){
        .state = changed->state,
        .state_generation = changed->state_generation,
        .reason = changed->reason,
    };
    pending_dirty[screen_index] = true;
    k_mutex_unlock(&pending_mutex);

    k_work_submit_to_queue(zmk_display_work_q(), &screenkey_work);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(screenkey_ai_state, screenkey_ai_state_cb);
ZMK_SUBSCRIPTION(screenkey_ai_state, rawhid_app_ai_client_state_changed);

static lv_obj_t *create_rect(lv_obj_t *parent) {
    lv_obj_t *rect = lv_obj_create(parent);
    lv_obj_set_style_border_width(rect, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(rect, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(rect, 0, LV_PART_MAIN);
    lv_obj_clear_flag(rect, LV_OBJ_FLAG_SCROLLABLE);
    return rect;
}

static lv_obj_t *build_screen(struct screenkey_screen *screen) {
    lv_obj_t *root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(root, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(root, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(root, 0, LV_PART_MAIN);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    screen->logo = lv_image_create(root);
    lv_image_set_src(screen->logo, &screenkey_codex_logo);
    lv_obj_center(screen->logo);
    set_hidden(screen->logo, true);

    for (size_t index = 0; index < BORDER_OBJECT_COUNT; index++) {
        screen->full_border[index] = create_rect(root);
    }
    lv_obj_set_pos(screen->full_border[0], 2, 2);
    lv_obj_set_size(screen->full_border[0], 124, 4);
    lv_obj_set_pos(screen->full_border[1], 122, 2);
    lv_obj_set_size(screen->full_border[1], 4, 124);
    lv_obj_set_pos(screen->full_border[2], 2, 122);
    lv_obj_set_size(screen->full_border[2], 124, 4);
    lv_obj_set_pos(screen->full_border[3], 2, 2);
    lv_obj_set_size(screen->full_border[3], 4, 124);
    hide_full_border(screen);

    for (size_t index = 0; index < SCREENKEY_RENDERER_MAX_SEGMENTS; index++) {
        screen->working_segments[index] = create_rect(root);
    }
    hide_working_segments(screen);

    set_backlight(screen, false);
    return root;
}

#if IS_ENABLED(CONFIG_AIPAD_DISPLAY_SELFTEST)

/* Bring-up aid: draw something on a screen without waiting for the host.
 *
 * In normal operation a screen with no host state resolves to
 * SCREENKEY_RENDERER_OFF, which hides the logo and kills the backlight, so a
 * correctly working panel looks exactly like an unwired one. Each screen gets a
 * different logo and border colour so the panels can be told apart. */
static void render_selftest(struct screenkey_screen *screen, uint8_t index) {
    screen->display_generation++;
    screen->current_mode = SCREENKEY_RENDERER_AVAILABLE;
    stop_state_timer(screen);
    hide_working_segments(screen);

    /* Alternate the logo and the border colour by screen index. With four
     * panels in a row, neighbours are then never identical, so the physical
     * left-to-right order of CS1..CS4 can be read straight off the board. */
    const bool odd = (index % 2) != 0;

    set_logo_for_client_type(screen, odd ? 0x02 : 0x01);
    set_hidden(screen->logo, false);
    show_full_border(screen, odd ? lv_color_hex(0x22C55E) : lv_color_hex(0x3B82F6), true);
    set_backlight(screen, true);

    LOG_INF("ScreenKey self test drawn on screen %u", index);
}

#else

/* Seeds one screen from the Core's retained state for its slot, so a screen
 * that is added while a session is already running does not stay blank until
 * the next host update. */
static void seed_screen(uint8_t display_slot) {
    struct screenkey_event_state current = {0};

    rawhid_app_ai_client_state_get_slot(display_slot, &current.state, &current.state_generation);
    current.reason = current.state.session_active ? RAWHID_APP_AI_CLIENT_STATE_UPDATED
                                                  : RAWHID_APP_AI_CLIENT_STATE_SESSION_ENDED;
    screenkey_render(&screens[display_slot], display_slot, current);
}

#endif /* CONFIG_AIPAD_DISPLAY_SELFTEST */

lv_obj_t *zmk_display_status_screen(void) {
    /* Log what actually came up, so a dark panel can be told apart from a
     * driver that never initialized. One line per panel: with four of them a
     * single combined line is unreadable in a log capture. */
    LOG_INF("ScreenKey backlight: %s", device_is_ready(backlight) ? "ready" : "NOT READY");
    LOG_INF("ScreenKey lcd0: %s",
            device_is_ready(DEVICE_DT_GET(DT_CHOSEN(zephyr_display))) ? "ready" : "NOT READY");
    for (size_t index = 0; index < SCREENKEY_DISPLAY_EXTRA_COUNT; index++) {
        LOG_INF("ScreenKey lcd%u: %s", (unsigned int)(index + 1),
                device_is_ready(extra_lcds[index]) ? "ready" : "NOT READY");
    }

    /* Screen 0 is the display ZMK owns: build its screen and hand it back so
     * ZMK loads it as usual. */
    lv_obj_t *root = build_screen(&screens[0]);

    /* Screens 1..n-1 are ours. Attach each panel to LVGL, build a screen on it
     * and load it while it is the default display. A panel that fails to
     * register is skipped rather than fatal, so the remaining ScreenKeys still
     * work and the log says which one dropped out. */
    bool screen_available[SCREENKEY_RENDERER_SCREEN_COUNT] = {0};
    screen_available[0] = true;

    lv_display_t *previous_default = lv_display_get_default();

    for (size_t index = 0; index < SCREENKEY_DISPLAY_EXTRA_COUNT; index++) {
        const unsigned int screen_index = (unsigned int)(index + 1);

        lv_display_t *display = screenkey_display_register(extra_lcds[index], extra_buffers[index],
                                                           sizeof(extra_buffers[index]));
        if (display == NULL) {
            LOG_ERR("ScreenKey screen %u unavailable; slot %u will not be shown", screen_index,
                    screen_index);
            continue;
        }

        lv_display_set_default(display);
        lv_screen_load(build_screen(&screens[screen_index]));
        screen_available[screen_index] = true;
    }

    lv_display_set_default(previous_default);

    for (uint8_t index = 0; index < SCREENKEY_RENDERER_SCREEN_COUNT; index++) {
        if (!screen_available[index]) {
            continue;
        }
#if IS_ENABLED(CONFIG_AIPAD_DISPLAY_SELFTEST)
        render_selftest(&screens[index], index);
#else
        seed_screen(index);
#endif
    }

    return root;
}
