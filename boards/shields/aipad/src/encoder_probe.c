#include <zephyr/kernel.h>

#include "encoder_probe.h"

#if IS_ENABLED(CONFIG_AIPAD_ENCODER_PROBE)

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(screenkey_renderer, CONFIG_ZMK_LOG_LEVEL);

/* Finds the encoder's real pins by watching which lines move.
 *
 * Every pin on the 24 way FPC is accounted for by the matrix, the panels or the
 * backlight, and all of those are confirmed working - so whatever the encoder is
 * actually wired to has to be one of the few pins left over. Rather than guess
 * from the schematic a second time (SCLK and CS-3 were already read the wrong
 * way round), this samples those pins and logs a line whenever any of them
 * changes. Turn the knob: the two that toggle together are the encoder.
 *
 * P0.09 and P0.04 belong to the EC11 driver, which configures them as inputs, so
 * they are only read here, never reconfigured. P0.05 and P0.31 have no owner and
 * are taken over as plain inputs - no internal pull, so the board's own 10K
 * pull-ups decide the level and a closed contact reads low.
 *
 * Build with CONFIG_GPIO_HOGS=n as well. Otherwise the hog that parks the
 * WS2812B data line holds P0.05 low and hides an encoder channel wired there. */
struct probe_pin {
    const struct device *port;
    gpio_pin_t pin;
    /* What the pin is put back to after the characterisation below. The two
     * encoder channels belong to the EC11 driver, which configures them as
     * plain inputs: the board's own pull-up sets the level, and an internal one
     * would divide against the series resistor. */
    gpio_flags_t resting;
    const char *name;
};

static const struct probe_pin probe_pins[] = {
    {DEVICE_DT_GET(DT_NODELABEL(gpio0)), 9, GPIO_INPUT, "P0.09"},
    {DEVICE_DT_GET(DT_NODELABEL(gpio0)), 4, GPIO_INPUT, "P0.04"},
    {DEVICE_DT_GET(DT_NODELABEL(gpio0)), 5, GPIO_INPUT, "P0.05"},
    {DEVICE_DT_GET(DT_NODELABEL(gpio0)), 31, GPIO_INPUT, "P0.31"},
};

BUILD_ASSERT(ARRAY_SIZE(probe_pins) == 4, "the log line below prints exactly four levels");

#define PROBE_POLL_MS 5

/* A running total beats reading individual lines: the first run of this probe
 * produced two change lines and no way to tell whether the knob had been turned
 * once or not at all. A count per pin says at a glance which channels move. The
 * summary also repeats the device readiness, which is logged at boot and never
 * survives to the USB backend. */
#define PROBE_SUMMARY_MS 5000
#define PROBE_STACK_SIZE 640
#define PROBE_PRIORITY 10

static int probe_level(size_t index) {
    return gpio_pin_get_raw(probe_pins[index].port, probe_pins[index].pin);
}

/* Reads each line once with the internal pull-up and once with the internal
 * pull-down, which says whether the line is wired to anything at all.
 *
 * Edge counts alone cannot tell an open circuit from a knob nobody turned. The
 * board puts a 10K pull-up on each encoder channel, and that beats the
 * nRF52840's ~13K internal pull-down, so a connected line still reads high when
 * pulled down - exactly how the four chip selects behave in the panel walk. A
 * line with nothing on the far end simply follows whichever internal pull is
 * applied. */
static const char *probe_classify(int with_pull_up, int with_pull_down) {
    if (with_pull_up == 1 && with_pull_down == 1) {
        return "external pull-up present: the line reaches the board";
    }
    if (with_pull_up == 1 && with_pull_down == 0) {
        return "floating: nothing on the far end";
    }
    if (with_pull_up == 0 && with_pull_down == 0) {
        return "held low: contact closed, or shorted to GND";
    }
    return "inconsistent";
}

static void probe_characterize(char *summary, size_t summary_size) {
    size_t written = 0;

    for (size_t index = 0; index < ARRAY_SIZE(probe_pins); index++) {
        const struct probe_pin *pin = &probe_pins[index];

        gpio_pin_configure(pin->port, pin->pin, GPIO_INPUT | GPIO_PULL_UP);
        k_busy_wait(500);
        const int with_pull_up = gpio_pin_get_raw(pin->port, pin->pin);

        gpio_pin_configure(pin->port, pin->pin, GPIO_INPUT | GPIO_PULL_DOWN);
        k_busy_wait(500);
        const int with_pull_down = gpio_pin_get_raw(pin->port, pin->pin);

        gpio_pin_configure(pin->port, pin->pin, pin->resting);

        LOG_INF("encoder probe: %s up=%d down=%d -> %s", pin->name, with_pull_up, with_pull_down,
                probe_classify(with_pull_up, with_pull_down));

        const int len = snprintk(summary + written, summary_size - written, "%s%s=%d/%d",
                                 written == 0 ? "" : " ", pin->name, with_pull_up, with_pull_down);
        if (len > 0 && (size_t)len < summary_size - written) {
            written += (size_t)len;
        }
    }
}

static void probe_thread_fn(void *unused1, void *unused2, void *unused3) {
    ARG_UNUSED(unused1);
    ARG_UNUSED(unused2);
    ARG_UNUSED(unused3);

    for (size_t index = 0; index < ARRAY_SIZE(probe_pins); index++) {
        if (!device_is_ready(probe_pins[index].port)) {
            LOG_ERR("encoder probe: GPIO port not ready");
            return;
        }
    }

    /* probe_characterize() leaves every pin in its resting configuration, so
     * there is nothing to set up here beyond checking the ports. */

    /* If the EC11 device never came up, nothing downstream can work and the pin
     * levels below are beside the point, so say so before the first sample. */
    LOG_INF("encoder probe: alps,ec11 device is %s",
            device_is_ready(DEVICE_DT_GET(DT_NODELABEL(encoder))) ? "ready" : "NOT READY");
    LOG_INF("encoder probe: turn the knob; the two pins that toggle together are the encoder");

    static char characterization[80];
    probe_characterize(characterization, sizeof(characterization));

    int last[ARRAY_SIZE(probe_pins)] = {-1, -1, -1, -1};
    uint32_t transitions[ARRAY_SIZE(probe_pins)] = {0};
    int64_t next_summary = k_uptime_get() + PROBE_SUMMARY_MS;

    while (true) {
        int now[ARRAY_SIZE(probe_pins)];
        bool changed = false;

        for (size_t index = 0; index < ARRAY_SIZE(probe_pins); index++) {
            now[index] = probe_level(index);
            if (now[index] != last[index]) {
                changed = true;
                /* The very first pass has last[] at -1, which is a change from
                 * nothing rather than a real edge. */
                if (last[index] >= 0) {
                    transitions[index]++;
                }
            }
        }

        if (changed) {
            LOG_INF("encoder probe: P0.09=%d P0.04=%d P0.05=%d P0.31=%d", now[0], now[1], now[2],
                    now[3]);
            memcpy(last, now, sizeof(last));
        }

        if (k_uptime_get() >= next_summary) {
            LOG_INF("encoder probe: ec11 %s | now P0.09=%d P0.04=%d P0.05=%d P0.31=%d",
                    device_is_ready(DEVICE_DT_GET(DT_NODELABEL(encoder))) ? "ready" : "NOT READY",
                    last[0], last[1], last[2], last[3]);
            LOG_INF("encoder probe: edges P0.09=%u P0.04=%u P0.05=%u P0.31=%u | up/down %s",
                    transitions[0], transitions[1], transitions[2], transitions[3],
                    characterization);
            next_summary = k_uptime_get() + PROBE_SUMMARY_MS;
        }

        k_msleep(PROBE_POLL_MS);
    }
}

K_THREAD_DEFINE(aipad_encoder_probe, PROBE_STACK_SIZE, probe_thread_fn, NULL, NULL, NULL,
                PROBE_PRIORITY, 0, 0);

#endif /* CONFIG_AIPAD_ENCODER_PROBE */
