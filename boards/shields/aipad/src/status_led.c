#include <zephyr/kernel.h>

#include "status_led.h"

#if IS_ENABLED(CONFIG_AIPAD_STATUS_LED) && DT_NODE_HAS_STATUS(DT_NODELABEL(aipad_leds), okay)

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include <rawhid_app/ai_client_state.h>
#include <rawhid_app/events/ai_client_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/workqueue.h>

#include "screenkey_renderer_model.h"

LOG_MODULE_DECLARE(screenkey_renderer, CONFIG_ZMK_LOG_LEVEL);

#define LED_COUNT DT_PROP(DT_NODELABEL(aipad_leds), chain_length)
BUILD_ASSERT(LED_COUNT == 4,
             "status_led.c assumes the 4-pixel WS2812B chain declared in aipad.overlay");

static const struct device *const strip = DEVICE_DT_GET(DT_NODELABEL(aipad_leds));

/* Folded AI indication and the per-slot modes that feed it.
 *
 * rawhid_app_ai_client_state_changed is raised from two different threads:
 * the usbd work queue for a normal state update, and the system work queue
 * for the Host's 15 second timeout path. The led_tick work below that reads
 * published_indication runs on a third thread (the lowprio work queue), so
 * both slot_modes[] and published_indication need a mutex even though each
 * individual writer already runs on its own work queue.
 *
 * slot_modes[] itself is only read and written by status_led_state_cb() and
 * the seed loop in status_led_init(), both compiled out under the chain
 * probe (see the guard further down), so it is declared conditionally too -
 * otherwise it would sit unused and warn under that config. */
#if !IS_ENABLED(CONFIG_AIPAD_STATUS_LED_CHAIN_PROBE)
static enum screenkey_renderer_mode slot_modes[SCREENKEY_RENDERER_SCREEN_COUNT];
#endif
static enum screenkey_led_indication published_indication;
/* Uptime in milliseconds at which a published COMPLETED stops being shown.
 * Unlike every other indication, COMPLETED ends on a clock rather than on the
 * next host update: the border in status_screen.c hides itself after
 * SCREENKEY_COMPLETED_HOLD_MS, and the chain has to go dark with it instead of
 * holding green until the host happens to say something else. Read by
 * led_tick_cb on the lowprio queue, written by the event callbacks, so it
 * shares led_mutex with published_indication - the two are always read
 * together and must agree. */
static int64_t completed_expiry;
static K_MUTEX_DEFINE(led_mutex);

/* Everything below is touched only by led_tick_cb, which always runs on the
 * lowprio work queue, so none of it needs locking. */
static bool led_tick_first = true;
static enum screenkey_led_indication last_indication;
static uint8_t frame;
static struct screenkey_led_color last_written;
static bool write_failed_logged;

/* A static indication (screenkey_led_period_ms() == 0, i.e. OFF or
 * COMPLETED) never ticks again on its own once it settles, so a single
 * write is the chain's only chance to show it. If that one SPI transfer
 * gets corrupted or dropped, the WS2812 chain latches the garbled or stale
 * frame until the board loses VDD - which is exactly the "AI finished but
 * the LED never turned off" failure this guards against. Once a static
 * colour is freshly settled, static_confirm_remaining is seeded with
 * LED_STATIC_CONFIRM_WRITES and led_tick_cb keeps forcing the same write
 * out - bypassing the usual "skip if unchanged" shortcut - every
 * LED_STATIC_CONFIRM_INTERVAL_MS until it reaches zero. */
#define LED_STATIC_CONFIRM_WRITES 3
#define LED_STATIC_CONFIRM_INTERVAL_MS 200
static uint8_t static_confirm_remaining;

/* Diagnostic snapshot for the boot report further down. strip_ready and
 * init_ok are written exactly once, from status_led_init() on the system
 * init thread, before anything below ever runs on the lowprio queue. The
 * remaining fields are written only from led_write_pixels() and
 * led_tick_cb(), both of which always run on the lowprio work queue - the
 * same queue the report itself runs on - so none of this needs a lock
 * either. published_indication is the one exception and still goes through
 * led_mutex, both here and in the report below. */
static struct {
    bool strip_ready;
    bool init_ok;
    int last_write_err;
    uint32_t write_count;
    uint32_t tick_count;
    /* Number of times led_tick_cb forced an early 200ms retry because a
     * write came back with an error, on top of (not instead of) the normal
     * static-colour confirm resends. Non-zero here means the strip has
     * actually dropped transfers on this boot, whether or not a stuck LED
     * was ever seen. */
    uint32_t retry_reschedules;
} led_diag;

static void led_tick_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(led_tick, led_tick_cb);

static int led_write_pixels(struct led_rgb *pixels) {
    const int err = led_strip_update_rgb(strip, pixels, LED_COUNT);

    /* Recorded on every call, success or failure, so the boot report below
     * can show whether writes are happening at all - not just whether the
     * last one that happened to get logged failed. */
    led_diag.last_write_err = err;
    led_diag.write_count++;

    if (err < 0 && !write_failed_logged) {
        /* Logged once: the tick can run at up to 10 Hz and a failing SPI
         * transfer would otherwise flood the log forever. */
        LOG_ERR("aipad status LED: strip update failed: %d", err);
        write_failed_logged = true;
    }

    return err;
}

/* Returns the led_strip_update_rgb() result so callers can tell a real write
 * from a lie. last_written is the record led_tick_cb trusts to decide
 * whether the chain already shows a given colour, so it must only move to
 * `color` once that colour has actually left the MCU: updating it
 * unconditionally would let a single corrupted or dropped SPI transfer
 * convince the driver the strip is already showing (for example) black, when
 * the WS2812 chain is still latched on whatever it displayed before - and
 * because WS2812 holds its last frame across anything short of a VDD drop,
 * that would stay wrong until the board is power-cycled. Leaving
 * last_written stale on failure instead makes the next tick's
 * "color != last_written" check true again, so it retries on its own. */
static int write_all(struct screenkey_led_color color) {
    /* Designated initializers only: CONFIG_LED_STRIP_RGB_SCRATCH prepends a
     * scratch member to struct led_rgb when enabled, and positional
     * initialization would silently shift r/g/b into the wrong fields if
     * that config ever flips on for this board. */
    struct led_rgb pixels[LED_COUNT];
    for (size_t i = 0; i < LED_COUNT; i++) {
        pixels[i] = (struct led_rgb){.r = color.r, .g = color.g, .b = color.b};
    }
    const int err = led_write_pixels(pixels);
    if (err == 0) {
        last_written = color;
    }
    return err;
}

#if IS_ENABLED(CONFIG_AIPAD_STATUS_LED_SELFTEST)

#define SELFTEST_STEP_MS 200
#define SELFTEST_STEP_COUNT (3 * LED_COUNT) /* red, green, blue x each chain position */

/* Step SELFTEST_STEP_COUNT + 1: every pixel white at once, held for this
 * long. See the comment in selftest_advance() for why this phase exists. */
#define SELFTEST_WHITE_MS 3000
#define SELFTEST_WHITE_STEP (SELFTEST_STEP_COUNT + 1)

static uint8_t selftest_step;

static void write_selftest_pixel(uint8_t position, struct led_rgb color) {
    struct led_rgb pixels[LED_COUNT];
    for (size_t i = 0; i < LED_COUNT; i++) {
        pixels[i] = (struct led_rgb){.r = 0, .g = 0, .b = 0};
    }
    pixels[position] = color;
    led_write_pixels(pixels);
}

/* Wiring self test: exactly one pixel lit at a time, cycling red, green,
 * blue across all four chain positions before handing off to normal
 * operation. Runs as steps of led_tick rather than a blocking loop in
 * SYS_INIT so a slow SPI bus can never stall boot.
 *
 * Returns true while the test still owns the strip (the caller must return
 * without touching the normal render path), false once it is done. */
static bool selftest_advance(void) {
    if (selftest_step > SELFTEST_WHITE_STEP) {
        return false;
    }

    if (selftest_step == SELFTEST_WHITE_STEP) {
        /* Forced all-white phase, held long enough that it cannot be missed.
         * Diagnostic aid for "the LEDs don't light on real hardware": the
         * chase above only ever lights one pixel at a time for 200 ms, so a
         * working strip can still go unnoticed if the observer blinks, looks
         * away, or the strip is dim/at a bad angle. Lighting every pixel
         * white at once for a few full seconds removes that risk and splits
         * the failure cleanly:
         *   - chase not seen, white seen    -> per-pixel timing/logic issue,
         *                                       not wiring or power
         *   - neither chase nor white seen  -> wiring or power, not software
         * Half scale rather than 255: this is the only moment all twelve
         * channels are lit at once, and VCC_3V3 feeds the MCU as well as the
         * chain on this board - a rail that has already browned out once
         * here. 128 is still unmistakable to the eye while roughly halving
         * the worst-case draw. Raise it only with a current meter inline. */
        write_all((struct screenkey_led_color){.r = 128, .g = 128, .b = 128});
        selftest_step++;
        k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &led_tick,
                                    K_MSEC(SELFTEST_WHITE_MS));
        return true;
    }

    if (selftest_step == SELFTEST_STEP_COUNT) {
        /* One extra step purely to blank the strip. Doing it in the same tick
         * that lit the twelfth pixel would leave that pixel on screen for no
         * time at all, and the last position is exactly the one a broken
         * DIN-DOUT link fails to reach - the step this test exists to show. */
        write_all((struct screenkey_led_color){.r = 0, .g = 0, .b = 0});
        selftest_step++;
        k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &led_tick, K_NO_WAIT);
        return true;
    }

    const uint8_t color_index = selftest_step / LED_COUNT; /* 0=red 1=green 2=blue */
    const uint8_t position = selftest_step % LED_COUNT;

    /* Full scale (255), not SCREENKEY_LED_MAX_LEVEL: only one pixel is ever
     * lit during this test, so full brightness still stays near 20 mA and is
     * harmless, and a wiring fault is easier to read off a meter or a phone
     * camera at maximum signal than at the dimmed operating level. */
    struct led_rgb color = {0};
    switch (color_index) {
    case 0:
        color.r = 255;
        break;
    case 1:
        color.g = 255;
        break;
    default:
        color.b = 255;
        break;
    }

    write_selftest_pixel(position, color);
    selftest_step++;
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &led_tick,
                                K_MSEC(SELFTEST_STEP_MS));
    return true;
}

#endif /* CONFIG_AIPAD_STATUS_LED_SELFTEST */

#if IS_ENABLED(CONFIG_AIPAD_STATUS_LED_CHAIN_PROBE)

/* Each phase holds for this long, rewriting the same frame every
 * PROBE_REFRESH_MS so a chain link that is dropping frames rather than
 * simply not receiving them at all can be told apart: a pixel with no signal
 * never changes across the repeated write, while one with a marginal signal
 * occasionally catches a refresh and briefly shows the right colour before
 * glitching again. */
#define PROBE_PHASE_MS 2000
#define PROBE_REFRESH_MS 500
#define PROBE_REFRESHES_PER_PHASE (PROBE_PHASE_MS / PROBE_REFRESH_MS)

enum probe_phase {
    PROBE_PHASE_ALL_RED = 0,
    PROBE_PHASE_ALL_GREEN,
    PROBE_PHASE_ALL_BLUE,
    PROBE_PHASE_ALL_WHITE,
    PROBE_PHASE_WALK_0,
    PROBE_PHASE_WALK_1,
    PROBE_PHASE_WALK_2,
    PROBE_PHASE_WALK_3,
    PROBE_PHASE_DISTINCT,
    PROBE_PHASE_ALL_OFF,
    PROBE_PHASE_COUNT,
};

static uint8_t probe_phase;
static uint8_t probe_refresh;

static const char *probe_phase_name(enum probe_phase phase) {
    switch (phase) {
    case PROBE_PHASE_ALL_RED:
        return "ALL RED";
    case PROBE_PHASE_ALL_GREEN:
        return "ALL GREEN";
    case PROBE_PHASE_ALL_BLUE:
        return "ALL BLUE";
    case PROBE_PHASE_ALL_WHITE:
        return "ALL WHITE";
    case PROBE_PHASE_WALK_0:
        return "WALK 0";
    case PROBE_PHASE_WALK_1:
        return "WALK 1";
    case PROBE_PHASE_WALK_2:
        return "WALK 2";
    case PROBE_PHASE_WALK_3:
        return "WALK 3";
    case PROBE_PHASE_DISTINCT:
        return "DISTINCT";
    case PROBE_PHASE_ALL_OFF:
        return "ALL OFF";
    default:
        return "?";
    }
}

/* Fills pixels[] for the given phase. Half scale (128), not 255, on every
 * phase that lights all four pixels at once - the same VCC_3V3 headroom
 * reason as SELFTEST_WHITE_MS above. Full scale (255) on the single-pixel
 * WALK phases, where only one pixel is ever lit and the draw stays near
 * 20 mA either way. */
static void probe_fill(enum probe_phase phase, struct led_rgb pixels[LED_COUNT]) {
    for (size_t i = 0; i < LED_COUNT; i++) {
        pixels[i] = (struct led_rgb){.r = 0, .g = 0, .b = 0};
    }

    switch (phase) {
    case PROBE_PHASE_ALL_RED:
        for (size_t i = 0; i < LED_COUNT; i++) {
            pixels[i].r = 128;
        }
        break;
    case PROBE_PHASE_ALL_GREEN:
        for (size_t i = 0; i < LED_COUNT; i++) {
            pixels[i].g = 128;
        }
        break;
    case PROBE_PHASE_ALL_BLUE:
        for (size_t i = 0; i < LED_COUNT; i++) {
            pixels[i].b = 128;
        }
        break;
    case PROBE_PHASE_ALL_WHITE:
        for (size_t i = 0; i < LED_COUNT; i++) {
            pixels[i] = (struct led_rgb){.r = 128, .g = 128, .b = 128};
        }
        break;
    case PROBE_PHASE_WALK_0:
    case PROBE_PHASE_WALK_1:
    case PROBE_PHASE_WALK_2:
    case PROBE_PHASE_WALK_3:
        pixels[phase - PROBE_PHASE_WALK_0] = (struct led_rgb){.r = 255, .g = 255, .b = 255};
        break;
    case PROBE_PHASE_DISTINCT:
        pixels[0] = (struct led_rgb){.r = 128, .g = 0, .b = 0};
        pixels[1] = (struct led_rgb){.r = 0, .g = 128, .b = 0};
        pixels[2] = (struct led_rgb){.r = 0, .g = 0, .b = 128};
        pixels[3] = (struct led_rgb){.r = 128, .g = 128, .b = 128};
        break;
    case PROBE_PHASE_ALL_OFF:
    default:
        /* Already zeroed above. */
        break;
    }
}

/* WS2812 chain diagnostic: cycles the ten fixed frames above, forever,
 * forcing a fresh SPI write every PROBE_REFRESH_MS regardless of whether the
 * frame "looks unchanged" from what write_all() last believed it wrote - see
 * docs/bring-up.md for how each phase's failure mode maps to a specific
 * wiring or power fault.
 *
 * Runs as steps of led_tick, same structure as selftest_advance() above, and
 * always returns true: normal rendering never runs while this is enabled.
 *
 * The CDC log backend under CONFIG_ZMK_USB_LOGGING is not up until roughly
 * 20 seconds after power-on (see docs/building.md), so on any single lap the
 * earliest phases can be missed in the log even though they still play out
 * on the strip. Because this loop never ends, every phase is guaranteed to
 * be logged eventually - just wait for the next lap. */
static bool probe_advance(void) {
    if (probe_refresh == 0) {
        LOG_INF("aipad LED probe: phase %u/%u %s write_count=%u last_err=%d", probe_phase + 1,
                (unsigned)PROBE_PHASE_COUNT, probe_phase_name((enum probe_phase)probe_phase),
                led_diag.write_count, led_diag.last_write_err);
    }

    struct led_rgb pixels[LED_COUNT];
    probe_fill((enum probe_phase)probe_phase, pixels);
    /* Go straight to led_write_pixels() rather than write_all(): this frame
     * must reach the SPI bus every single time, never skipped because it
     * looks unchanged from the last frame this driver believes it wrote. */
    led_write_pixels(pixels);

    probe_refresh++;
    if (probe_refresh >= PROBE_REFRESHES_PER_PHASE) {
        probe_refresh = 0;
        probe_phase = (probe_phase + 1) % PROBE_PHASE_COUNT;
    }

    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &led_tick,
                                K_MSEC(PROBE_REFRESH_MS));
    return true;
}

#endif /* CONFIG_AIPAD_STATUS_LED_CHAIN_PROBE */

static void led_tick_cb(struct k_work *work) {
    ARG_UNUSED(work);
    led_diag.tick_count++;

#if IS_ENABLED(CONFIG_AIPAD_STATUS_LED_SELFTEST)
    if (selftest_advance()) {
        return;
    }
#endif

#if IS_ENABLED(CONFIG_AIPAD_STATUS_LED_CHAIN_PROBE)
    if (probe_advance()) {
        return;
    }
#endif

    k_mutex_lock(&led_mutex, K_FOREVER);
    enum screenkey_led_indication indication = published_indication;
    const int64_t expiry = completed_expiry;
    k_mutex_unlock(&led_mutex);

    /* Fold an elapsed completion hold into plain OFF before anything below
     * looks at the indication, so the blanking write and the "stop
     * rescheduling" decision both fall out of the existing paths. */
    int64_t remaining = 0;
    if (indication == SCREENKEY_LED_COMPLETED) {
        remaining = expiry - k_uptime_get();
        if (remaining <= 0) {
            indication = SCREENKEY_LED_OFF;
        }
    }

    const bool indication_is_new = led_tick_first || indication != last_indication;
    if (indication_is_new) {
        frame = 0;
        led_tick_first = false;
    } else {
        const uint8_t frame_count = screenkey_led_frame_count(indication);
        /* Always fold through frame_count rather than letting frame free-run:
         * a plain increment would eventually wrap uint8_t at 255->0, which
         * does not line up with an arbitrary frame_count and would glitch the
         * animation once a cycle. Guard divide-by-zero for an indication with
         * no frames (e.g. OFF, which never reschedules anyway). */
        frame = (frame_count != 0) ? (uint8_t)((frame + 1) % frame_count) : 0;
    }
    last_indication = indication;

    const struct screenkey_led_color color = screenkey_led_color_for(indication, frame);
    const bool color_changed =
        color.r != last_written.r || color.g != last_written.g || color.b != last_written.b;
    /* screenkey_led_period_ms() == 0 is the model's own definition of
     * "static": OFF and COMPLETED, the two indications that never get a
     * self-driven animation tick. Testing the period here instead of the
     * enum value keeps this file from having to know which indications are
     * static beyond what the model already says. */
    const bool is_static = screenkey_led_period_ms(indication) == 0;

    if (is_static) {
        /* A freshly-settled static colour - either the indication just
         * changed, or an already-static indication changed colour (OFF can't
         * do this, but it costs nothing to handle the general case) - starts
         * a fresh confirm run. Restarting on indication_is_new also covers
         * the "same colour, but the animation to a static one arrived on a
         * different route" case: WAITING_APPROVAL -> OFF via two different
         * slots is still one settle. */
        if (indication_is_new || color_changed) {
            static_confirm_remaining = LED_STATIC_CONFIRM_WRITES;
        }
    } else {
        /* Not static any more (or never was): any confirm run left over from
         * a previous static stretch no longer means anything. */
        static_confirm_remaining = 0;
    }

    /* Skip the SPI transfer only when the colour already matches what the
     * chain is believed to show AND no confirm resend is pending. The
     * blink/breathe cycle is SCREENKEY_LED_CYCLE_MS long against a much
     * shorter tick, so without this most ticks would rewrite an identical
     * value for nothing - but a static colour still has to go out
     * LED_STATIC_CONFIRM_WRITES times regardless of this shortcut, since the
     * whole point of the confirm run is to survive one dropped transfer,
     * i.e. exactly the case where last_written could not be trusted anyway. */
    bool write_attempted = false;
    int write_err = 0;
    if (color_changed || (is_static && static_confirm_remaining > 0)) {
        write_err = write_all(color);
        write_attempted = true;
        if (write_err == 0 && is_static && static_confirm_remaining > 0) {
            static_confirm_remaining--;
        }
    }

    if (write_attempted && write_err != 0) {
        /* A failed write is retried on its own short timer, independent of
         * whatever schedule the indication would normally get - including
         * OFF and an expired COMPLETED, which otherwise stop rescheduling
         * entirely. Without this, a write that fails on what was supposed to
         * be the *last* confirm resend (or on the one-shot OFF write) would
         * leave the chain lit or garbled with nothing left to notice and
         * retry. static_confirm_remaining is left untouched here (it was
         * only decremented above on success), so once a write finally does
         * go through, the confirm run still finishes its full count. */
        led_diag.retry_reschedules++;
        k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &led_tick,
                                    K_MSEC(LED_STATIC_CONFIRM_INTERVAL_MS));
        return;
    }

    if (indication == SCREENKEY_LED_COMPLETED) {
        /* Static colour, so no animation period. Ordinarily one wake-up at
         * the end of the hold is all that is needed, and that pass blanks
         * the chain through the OFF fold above - but while a confirm resend
         * is still pending, that would let the hold expire without ever
         * sending the 2nd/3rd copy of the green. Take whichever wake-up
         * comes first. */
        int64_t next_ms = remaining;
        if (static_confirm_remaining > 0 && (int64_t)LED_STATIC_CONFIRM_INTERVAL_MS < next_ms) {
            next_ms = LED_STATIC_CONFIRM_INTERVAL_MS;
        }
        k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &led_tick, K_MSEC(next_ms));
        return;
    }

    if (is_static) {
        /* Only OFF can reach here (COMPLETED already returned above). Keep
         * waking up while a confirm resend is pending; once the run is done,
         * go back to leaving the tick unscheduled, same as before - the next
         * state change wakes it again from status_led_state_cb(). */
        if (static_confirm_remaining > 0) {
            k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &led_tick,
                                        K_MSEC(LED_STATIC_CONFIRM_INTERVAL_MS));
        }
        return;
    }

    const uint16_t period = screenkey_led_period_ms(indication);
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &led_tick, K_MSEC(period));
}

/* Human-readable form of an indication for the boot report below. Mirrors
 * the enum values in screenkey_renderer_model.h: SCREENKEY_LED_OFF = 0,
 * WAITING_APPROVAL = 1, WAITING_INPUT = 2, ERROR = 3, COMPLETED = 4. */
static const char *led_indication_name(enum screenkey_led_indication indication) {
    switch (indication) {
    case SCREENKEY_LED_OFF:
        return "OFF";
    case SCREENKEY_LED_WAITING_APPROVAL:
        return "APPROVAL";
    case SCREENKEY_LED_WAITING_INPUT:
        return "INPUT";
    case SCREENKEY_LED_ERROR:
        return "ERROR";
    case SCREENKEY_LED_COMPLETED:
        return "COMPLETED";
    default:
        return "?";
    }
}

/* Boot report: replays what status_led_init() and the tick found out, a few
 * times, well after boot.
 *
 * status_led_init() runs from SYS_INIT at APPLICATION priority, long before
 * CONFIG_ZMK_USB_LOGGING's CDC backend is up - that takes roughly 20 seconds
 * after power-on - so LOG_ERR("... strip device not ready") and friends never
 * reach the USB log; they are either dropped or sit unseen in the ring
 * buffer. This mirrors walk_report_cb() in status_screen.c: the findings are
 * kept in led_diag and replayed here, on the lowprio work queue, once USB has
 * had a chance to enumerate. */
#define LED_REPORT_REPEATS 3
#define LED_REPORT_INTERVAL K_SECONDS(10)

static void led_report_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(led_report, led_report_cb);

static void led_report_cb(struct k_work *work) {
    ARG_UNUSED(work);
    static uint8_t reports;

    k_mutex_lock(&led_mutex, K_FOREVER);
    const enum screenkey_led_indication indication = published_indication;
    k_mutex_unlock(&led_mutex);

    /* Split into several short lines rather than one long one, matching the
     * example in the request that motivated this report - easier to read in
     * a terminal that wraps a serial capture at 80 columns. */
    LOG_INF("--- aipad status LED report %u/%u (replayed for the USB log) ---", reports + 1,
            LED_REPORT_REPEATS);
    LOG_INF("aipad status LED: strip ready=%s init=%s", led_diag.strip_ready ? "yes" : "no",
            led_diag.init_ok ? "ok" : "FAILED");
    LOG_INF("aipad status LED: ticks=%u writes=%u last_err=%d retries=%u", led_diag.tick_count,
            led_diag.write_count, led_diag.last_write_err, led_diag.retry_reschedules);
#if IS_ENABLED(CONFIG_AIPAD_STATUS_LED_SELFTEST)
    LOG_INF("aipad status LED: indication=%d (%s) selftest_step=%u", (int)indication,
            led_indication_name(indication), selftest_step);
#elif IS_ENABLED(CONFIG_AIPAD_STATUS_LED_CHAIN_PROBE)
    /* indication stays whatever status_led_init() left it at (OFF, since the
     * state listener below is compiled out for this config) - logged anyway
     * so this line has the same shape as the other two branches. */
    LOG_INF("aipad status LED: indication=%d (%s) probe_phase=%u/%u (%s)", (int)indication,
            led_indication_name(indication), probe_phase + 1, (unsigned)PROBE_PHASE_COUNT,
            probe_phase_name((enum probe_phase)probe_phase));
#else
    LOG_INF("aipad status LED: indication=%d (%s)", (int)indication,
            led_indication_name(indication));
#endif

    if (++reports < LED_REPORT_REPEATS) {
        k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &led_report,
                                    LED_REPORT_INTERVAL);
    }
}

#if !IS_ENABLED(CONFIG_AIPAD_STATUS_LED_CHAIN_PROBE)

/* Compiled out entirely while the chain probe is running: a host state
 * update landing mid-probe would call k_work_reschedule(..., K_NO_WAIT) on
 * led_tick and yank the next phase forward, corrupting the fixed 2 s/500 ms
 * cadence the probe depends on to be readable. */
static int status_led_state_cb(const zmk_event_t *eh) {
    /* zmk_display_is_initialized() is deliberately not checked here: the LED
     * indicator is meant to keep working even when the chosen display has
     * died, so it must not depend on the display subsystem being up. */
    const struct rawhid_app_ai_client_state_changed *changed =
        as_rawhid_app_ai_client_state_changed(eh);
    if (changed == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    const int screen_index = screenkey_renderer_screen_for_slot(changed->display_slot);
    if (screen_index == SCREENKEY_RENDERER_NO_SCREEN) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    const enum screenkey_renderer_mode mode = screenkey_renderer_mode_for_state(
        changed->state.session_active, changed->state.activity_state, changed->state.work_phase);

    k_mutex_lock(&led_mutex, K_FOREVER);
    /* Whether this event is the moment the slot finished, as opposed to some
     * later event that leaves it sitting in COMPLETED. Only the former
     * restarts the hold below. */
    const bool slot_became_completed =
        mode == SCREENKEY_RENDERER_COMPLETED && slot_modes[screen_index] != mode;
    slot_modes[screen_index] = mode;
    const enum screenkey_led_indication folded =
        screenkey_led_indication_for_modes(slot_modes, SCREENKEY_RENDERER_SCREEN_COUNT);
    const bool indication_changed = folded != published_indication;
    published_indication = folded;

    /* The hold tracks the most recent moment any slot finished, whatever the
     * fold happened to be showing then - the same way each screen's own
     * border timer starts when that screen finishes, regardless of the other
     * screens. Restarting it on the fold instead would relight the green when
     * an unrelated approval clears long after the completion it belongs to,
     * by which time that screen's border is already gone. */
    if (slot_became_completed) {
        completed_expiry = k_uptime_get() + SCREENKEY_COMPLETED_HOLD_MS;
    }
    const bool completion_needs_tick = slot_became_completed && folded == SCREENKEY_LED_COMPLETED;
    k_mutex_unlock(&led_mutex);

    /* Wake the tick when the folded indication changes, and also when a fresh
     * completion arrives while the fold is already COMPLETED - the tick may
     * have gone dark on an earlier hold and left nothing scheduled to notice
     * the new one. A slot moving between two modes that fold to the same
     * indication (e.g. WORKING_MOVING -> AVAILABLE, neither of which the LEDs
     * distinguish) would otherwise kick the tick unconditionally, resetting
     * frame mid cycle and making the blink visibly stutter for no visible
     * reason. */
    if (indication_changed || completion_needs_tick) {
        k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &led_tick, K_NO_WAIT);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(status_led_state, status_led_state_cb);
ZMK_SUBSCRIPTION(status_led_state, rawhid_app_ai_client_state_changed);

#endif /* !CONFIG_AIPAD_STATUS_LED_CHAIN_PROBE */

static int status_led_init(void) {
    led_diag.strip_ready = device_is_ready(strip);

    if (!led_diag.strip_ready) {
        LOG_ERR("aipad status LED: strip device not ready");
        led_diag.init_ok = false;
        /* Schedule the boot report even on this early-return path. Whether
         * the strip device ever came up is exactly what that report exists
         * to show, and the LOG_ERR just above will not have reached the USB
         * log (see the comment on led_report_cb). */
        k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &led_report,
                                    LED_REPORT_INTERVAL);
        return -ENODEV;
    }

    /* Unconditionally blank the strip once at boot, before looking at any
     * retained AI client state. WS2812B latches its last frame across
     * anything short of a VDD drop, and this board's VCC_3V3 does not fall on
     * an nRF52840 soft reset - so returning from DFU or recovering from a
     * watchdog reset would otherwise resume mid animation (e.g. stuck amber
     * for "waiting approval") instead of a clean boot. This also flushes
     * whatever the data line picked up during the float window before this
     * driver takes ownership of it. */
    write_all((struct screenkey_led_color){.r = 0, .g = 0, .b = 0});

#if !IS_ENABLED(CONFIG_AIPAD_STATUS_LED_CHAIN_PROBE)
    /* Seed every slot from the Core's retained state, mirroring seed_screen()
     * in status_screen.c: without this, reconnecting mid session leaves the
     * LEDs dark until the next host update instead of picking up the
     * in-progress indication immediately. Pointless while the chain probe is
     * running - its state listener is compiled out above and nothing ever
     * reads slot_modes/published_indication again - so it is skipped for
     * that config rather than seeding values no code path will look at. */
    k_mutex_lock(&led_mutex, K_FOREVER);
    for (uint8_t index = 0; index < SCREENKEY_RENDERER_SCREEN_COUNT; index++) {
        struct rawhid_app_ai_client_state state;
        uint32_t generation;
        const bool have_state = rawhid_app_ai_client_state_get_slot(index, &state, &generation);

        slot_modes[index] = have_state ? screenkey_renderer_mode_for_state(
                                             state.session_active, state.activity_state,
                                             state.work_phase)
                                       : SCREENKEY_RENDERER_OFF;
    }
    published_indication =
        screenkey_led_indication_for_modes(slot_modes, SCREENKEY_RENDERER_SCREEN_COUNT);
    /* A retained COMPLETED gets a fresh hold rather than an expired one: the
     * uptime clock restarts at zero on this reset, so any deadline recorded
     * before it is meaningless. Showing the green for one more hold matches
     * the screens, which redraw the finished border from the same retained
     * state, and it still ends on its own instead of latching forever. */
    if (published_indication == SCREENKEY_LED_COMPLETED) {
        completed_expiry = k_uptime_get() + SCREENKEY_COMPLETED_HOLD_MS;
    }
    k_mutex_unlock(&led_mutex);
#endif /* !CONFIG_AIPAD_STATUS_LED_CHAIN_PROBE */

    led_diag.init_ok = true;
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &led_tick, K_NO_WAIT);
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &led_report, LED_REPORT_INTERVAL);
    return 0;
}

SYS_INIT(status_led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* CONFIG_AIPAD_STATUS_LED && aipad_leds okay */
