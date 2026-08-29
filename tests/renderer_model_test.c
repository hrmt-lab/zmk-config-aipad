#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "screenkey_renderer_model.h"

static int segment_length(const struct screenkey_renderer_segment *segment) {
    return segment->width > segment->height ? segment->width : segment->height;
}

static void test_state_mapping(void) {
    assert(screenkey_renderer_mode_for_state(false, 0, 0) == SCREENKEY_RENDERER_OFF);
    assert(screenkey_renderer_mode_for_state(true, 1, 0) == SCREENKEY_RENDERER_AVAILABLE);
    assert(screenkey_renderer_mode_for_state(true, 2, 0) ==
           SCREENKEY_RENDERER_WORKING_MOVING);
    assert(screenkey_renderer_mode_for_state(true, 2, 1) ==
           SCREENKEY_RENDERER_WORKING_MOVING);
    assert(screenkey_renderer_mode_for_state(true, 2, 2) ==
           SCREENKEY_RENDERER_WORKING_MOVING);
    assert(screenkey_renderer_mode_for_state(true, 2, 3) ==
           SCREENKEY_RENDERER_WORKING_MOVING);
    assert(screenkey_renderer_mode_for_state(true, 2, 0xff) ==
           SCREENKEY_RENDERER_WORKING_MOVING);
    assert(screenkey_renderer_mode_for_state(true, 3, 0) ==
           SCREENKEY_RENDERER_WAITING_APPROVAL);
    assert(screenkey_renderer_mode_for_state(true, 4, 0) ==
           SCREENKEY_RENDERER_WAITING_INPUT);
    assert(screenkey_renderer_mode_for_state(true, 5, 0) == SCREENKEY_RENDERER_COMPLETED);
    assert(screenkey_renderer_mode_for_state(true, 6, 0) == SCREENKEY_RENDERER_ERROR);
    assert(screenkey_renderer_mode_for_state(true, 0, 0) == SCREENKEY_RENDERER_OFF);
    assert(screenkey_renderer_mode_for_state(true, 7, 0) == SCREENKEY_RENDERER_OFF);
}

static void test_breath_opacity_wave(void) {
    uint8_t minimum = UINT8_MAX;
    uint8_t maximum = 0;

    for (uint8_t frame = 0; frame < SCREENKEY_RENDERER_FRAME_COUNT; frame++) {
        const uint8_t opacity = screenkey_renderer_breath_opacity(frame);
        assert(opacity >= 64);
        if (frame > 0 && frame <= 10) {
            assert(opacity > screenkey_renderer_breath_opacity(frame - 1));
        }
        if (frame > 10) {
            assert(opacity < screenkey_renderer_breath_opacity(frame - 1));
        }
        if (opacity < minimum) {
            minimum = opacity;
        }
        if (opacity > maximum) {
            maximum = opacity;
        }
    }

    assert(minimum == 64);
    assert(maximum == 255);
    assert(screenkey_renderer_breath_opacity(0) == 64);
    assert(screenkey_renderer_breath_opacity(10) == 255);
    assert(screenkey_renderer_breath_opacity(20) == 64);
    assert(screenkey_renderer_breath_opacity(21) == screenkey_renderer_breath_opacity(1));
}

static void test_working_geometry(void) {
    bool saw_split_segment = false;

    for (uint8_t frame = 0; frame < SCREENKEY_RENDERER_FRAME_COUNT; frame++) {
        struct screenkey_renderer_segment segments[SCREENKEY_RENDERER_MAX_SEGMENTS] = {0};
        const size_t count = screenkey_renderer_working_segments(frame, segments);
        int total_length = 0;

        assert(count >= 1);
        assert(count <= SCREENKEY_RENDERER_MAX_SEGMENTS);
        saw_split_segment |= count > 1;

        for (size_t index = 0; index < count; index++) {
            const struct screenkey_renderer_segment *segment = &segments[index];
            assert(segment->x >= 2);
            assert(segment->y >= 2);
            assert(segment->x + segment->width <= 126);
            assert(segment->y + segment->height <= 126);
            assert(segment->width == 4 || segment->height == 4);
            total_length += segment_length(segment);
        }

        assert(total_length == 124);
    }

    assert(saw_split_segment);
}

static void test_working_start_follows_upright_orientation(void) {
    struct screenkey_renderer_segment segments[SCREENKEY_RENDERER_MAX_SEGMENTS] = {0};

    size_t count = screenkey_renderer_working_segments(0, segments);
    assert(count == 1);
    assert(segments[0].x == 2);
    assert(segments[0].y == 2);
    assert(segments[0].width == 4);
    assert(segments[0].height == 124);

    count = screenkey_renderer_working_segments(1, segments);
    assert(count == 2);
    assert(segments[0].x == 2);
    assert(segments[0].y == 2);
    assert(segments[0].width == 4);
    assert(segments[0].height == 100);
    assert(segments[1].x == 2);
    assert(segments[1].y == 2);
    assert(segments[1].width == 24);
    assert(segments[1].height == 4);
}

static void test_logo_selection(void) {
    assert(screenkey_renderer_logo_for_client_type(2) == SCREENKEY_RENDERER_LOGO_CLAUDE_CODE);
    assert(screenkey_renderer_logo_for_client_type(1) == SCREENKEY_RENDERER_LOGO_CODEX);
    assert(screenkey_renderer_logo_for_client_type(0) == SCREENKEY_RENDERER_LOGO_CODEX);
    assert(screenkey_renderer_logo_for_client_type(3) == SCREENKEY_RENDERER_LOGO_CODEX);
    assert(screenkey_renderer_logo_for_client_type(0xff) == SCREENKEY_RENDERER_LOGO_CODEX);
}

static void test_slot_maps_to_matching_screen(void) {
    assert(SCREENKEY_RENDERER_SCREEN_COUNT == 4);
    for (uint8_t slot = 0; slot < SCREENKEY_RENDERER_SCREEN_COUNT; slot++) {
        assert(screenkey_renderer_screen_for_slot(slot) == (int)slot);
    }
}

static void test_slots_without_a_screen_are_ignored(void) {
    /* The host can address eight logical slots. Slots this keyboard has no
     * screen for must be reported as absent, never folded onto screen 0. */
    for (uint8_t slot = SCREENKEY_RENDERER_SCREEN_COUNT; slot <= 7; slot++) {
        assert(screenkey_renderer_screen_for_slot(slot) == SCREENKEY_RENDERER_NO_SCREEN);
    }
    assert(screenkey_renderer_screen_for_slot(8) == SCREENKEY_RENDERER_NO_SCREEN);
    assert(screenkey_renderer_screen_for_slot(0xff) == SCREENKEY_RENDERER_NO_SCREEN);
}

static void test_led_indication_priority(void) {
    const enum screenkey_renderer_mode all_three[] = {
        SCREENKEY_RENDERER_WAITING_APPROVAL,
        SCREENKEY_RENDERER_WAITING_INPUT,
        SCREENKEY_RENDERER_ERROR,
    };
    assert(screenkey_led_indication_for_modes(all_three, 3) == SCREENKEY_LED_WAITING_APPROVAL);

    const enum screenkey_renderer_mode input_and_error[] = {
        SCREENKEY_RENDERER_WAITING_INPUT,
        SCREENKEY_RENDERER_ERROR,
    };
    assert(screenkey_led_indication_for_modes(input_and_error, 2) ==
           SCREENKEY_LED_WAITING_INPUT);

    const enum screenkey_renderer_mode error_only[] = {SCREENKEY_RENDERER_ERROR};
    assert(screenkey_led_indication_for_modes(error_only, 1) == SCREENKEY_LED_ERROR);

    const enum screenkey_renderer_mode nothing_relevant[] = {
        SCREENKEY_RENDERER_WORKING_MOVING,
        SCREENKEY_RENDERER_AVAILABLE,
        SCREENKEY_RENDERER_OFF,
    };
    assert(screenkey_led_indication_for_modes(nothing_relevant, 3) == SCREENKEY_LED_OFF);

    /* COMPLETED is the one mode below the attention states: it lights the
     * chain on its own, but loses to every state that asks the user to act. */
    const enum screenkey_renderer_mode completed_only[] = {
        SCREENKEY_RENDERER_COMPLETED,
        SCREENKEY_RENDERER_WORKING_MOVING,
        SCREENKEY_RENDERER_AVAILABLE,
        SCREENKEY_RENDERER_OFF,
    };
    assert(screenkey_led_indication_for_modes(completed_only, 4) == SCREENKEY_LED_COMPLETED);

    const enum screenkey_renderer_mode completed_and_approval[] = {
        SCREENKEY_RENDERER_COMPLETED,
        SCREENKEY_RENDERER_WAITING_APPROVAL,
    };
    assert(screenkey_led_indication_for_modes(completed_and_approval, 2) ==
           SCREENKEY_LED_WAITING_APPROVAL);

    const enum screenkey_renderer_mode completed_and_input[] = {
        SCREENKEY_RENDERER_COMPLETED,
        SCREENKEY_RENDERER_WAITING_INPUT,
    };
    assert(screenkey_led_indication_for_modes(completed_and_input, 2) ==
           SCREENKEY_LED_WAITING_INPUT);

    const enum screenkey_renderer_mode completed_and_error[] = {
        SCREENKEY_RENDERER_COMPLETED,
        SCREENKEY_RENDERER_ERROR,
    };
    assert(screenkey_led_indication_for_modes(completed_and_error, 2) == SCREENKEY_LED_ERROR);

    const enum screenkey_renderer_mode approval_amid_working[] = {
        SCREENKEY_RENDERER_WAITING_APPROVAL,
        SCREENKEY_RENDERER_WORKING_MOVING,
        SCREENKEY_RENDERER_WORKING_MOVING,
        SCREENKEY_RENDERER_WORKING_MOVING,
    };
    assert(screenkey_led_indication_for_modes(approval_amid_working, 4) ==
           SCREENKEY_LED_WAITING_APPROVAL);

    assert(screenkey_led_indication_for_modes(nothing_relevant, 0) == SCREENKEY_LED_OFF);
}

static void test_led_indication_order_independent(void) {
    /* Four distinguishable modes, each contributing a different (or no)
     * outcome. Every one of the 4! = 24 orderings must still resolve to
     * WAITING_APPROVAL, since that mode is present and outranks the rest
     * regardless of where it sits in the array. */
    const enum screenkey_renderer_mode values[4] = {
        SCREENKEY_RENDERER_WAITING_APPROVAL,
        SCREENKEY_RENDERER_WAITING_INPUT,
        SCREENKEY_RENDERER_ERROR,
        SCREENKEY_RENDERER_OFF,
    };

    for (size_t a = 0; a < 4; a++) {
        for (size_t b = 0; b < 4; b++) {
            if (b == a) {
                continue;
            }
            for (size_t c = 0; c < 4; c++) {
                if (c == a || c == b) {
                    continue;
                }
                for (size_t d = 0; d < 4; d++) {
                    if (d == a || d == b || d == c) {
                        continue;
                    }
                    const enum screenkey_renderer_mode permutation[4] = {
                        values[a],
                        values[b],
                        values[c],
                        values[d],
                    };
                    assert(screenkey_led_indication_for_modes(permutation, 4) ==
                           SCREENKEY_LED_WAITING_APPROVAL);
                }
            }
        }
    }
}

static void test_led_blink_shape(void) {
    const enum screenkey_led_indication blinking[] = {SCREENKEY_LED_WAITING_APPROVAL,
                                                        SCREENKEY_LED_ERROR};

    for (size_t i = 0; i < 2; i++) {
        const enum screenkey_led_indication indication = blinking[i];
        const struct screenkey_led_color lit = screenkey_led_color_for(indication, 0);

        assert(lit.r > 0 || lit.g > 0 || lit.b > 0);

        for (uint8_t frame = 0; frame < 10; frame++) {
            const struct screenkey_led_color color = screenkey_led_color_for(indication, frame);
            assert(color.r == lit.r);
            assert(color.g == lit.g);
            assert(color.b == lit.b);
        }
        for (uint8_t frame = 10; frame < 20; frame++) {
            const struct screenkey_led_color color = screenkey_led_color_for(indication, frame);
            assert(color.r == 0);
            assert(color.g == 0);
            assert(color.b == 0);
        }

        const struct screenkey_led_color wrapped = screenkey_led_color_for(indication, 20);
        assert(wrapped.r == lit.r);
        assert(wrapped.g == lit.g);
        assert(wrapped.b == lit.b);
    }
}

static void test_led_completed_shape(void) {
    /* Steady green for the whole hold: no frame, including the wrap points the
     * blink and breath waves turn on, may change the colour. */
    const struct screenkey_led_color lit = screenkey_led_color_for(SCREENKEY_LED_COMPLETED, 0);

    assert(lit.g > lit.r);
    assert(lit.g > lit.b);
    assert(lit.r > 0 && lit.b > 0);

    for (uint8_t frame = 0; frame <= 40; frame++) {
        const struct screenkey_led_color color =
            screenkey_led_color_for(SCREENKEY_LED_COMPLETED, frame);
        assert(color.r == lit.r);
        assert(color.g == lit.g);
        assert(color.b == lit.b);
    }

    /* The border is drawn in 0x22C55E, so the chain must be that same green
     * scaled onto the LED brightness ceiling - not a different green. */
    assert(lit.r == (uint8_t)((0x22 * SCREENKEY_LED_MAX_LEVEL) / 255));
    assert(lit.g == (uint8_t)((0xC5 * SCREENKEY_LED_MAX_LEVEL) / 255));
    assert(lit.b == (uint8_t)((0x5E * SCREENKEY_LED_MAX_LEVEL) / 255));

    /* Static, so no animation period; one frame, so the caller's frame
     * counter folds without dividing by zero. */
    assert(screenkey_led_period_ms(SCREENKEY_LED_COMPLETED) == 0);
    assert(screenkey_led_frame_count(SCREENKEY_LED_COMPLETED) == 1);
}

static void test_led_breath_shape(void) {
    /* WAITING_INPUT's base color (0xF97316) has a nonzero red channel, so it
     * is a reliable stand-in for overall brightness across the wave. */
    uint8_t previous_red = screenkey_led_color_for(SCREENKEY_LED_WAITING_INPUT, 0).r;
    const struct screenkey_led_color at_zero = screenkey_led_color_for(SCREENKEY_LED_WAITING_INPUT, 0);
    const struct screenkey_led_color at_peak = screenkey_led_color_for(SCREENKEY_LED_WAITING_INPUT, 10);
    const struct screenkey_led_color at_wrap = screenkey_led_color_for(SCREENKEY_LED_WAITING_INPUT, 20);

    assert(at_zero.r == at_wrap.r);
    assert(at_zero.r < at_peak.r);

    for (uint8_t frame = 0; frame <= 20; frame++) {
        const struct screenkey_led_color color = screenkey_led_color_for(SCREENKEY_LED_WAITING_INPUT, frame);

        /* Breathing must stay visibly distinct from blinking: it never goes
         * fully dark, unlike the blink indications' off half-cycle. */
        assert(color.r > 0 || color.g > 0 || color.b > 0);

        if (frame > 0 && frame <= 10) {
            assert(color.r >= previous_red);
        }
        if (frame > 10) {
            assert(color.r <= previous_red);
        }
        previous_red = color.r;
    }
}

static void test_led_color_wraps(void) {
    /* API contract, not an implementation detail: regardless of how each
     * indication folds the frame internally (blink and breath use different
     * mechanisms), the observable color must repeat with period frame_count. */
    const enum screenkey_led_indication indications[] = {
        SCREENKEY_LED_OFF,
        SCREENKEY_LED_WAITING_APPROVAL,
        SCREENKEY_LED_WAITING_INPUT,
        SCREENKEY_LED_ERROR,
    };

    for (size_t i = 0; i < 4; i++) {
        const enum screenkey_led_indication indication = indications[i];
        const uint8_t frame_count = screenkey_led_frame_count(indication);

        for (uint16_t frame = 0; frame <= 255; frame++) {
            const struct screenkey_led_color color = screenkey_led_color_for(indication, (uint8_t)frame);

            if (indication == SCREENKEY_LED_OFF) {
                assert(color.r == 0);
                assert(color.g == 0);
                assert(color.b == 0);
                continue;
            }

            const struct screenkey_led_color folded =
                screenkey_led_color_for(indication, (uint8_t)(frame % frame_count));
            assert(color.r == folded.r);
            assert(color.g == folded.g);
            assert(color.b == folded.b);
        }
    }
}

static void test_led_color_max_40_percent(void) {
    struct base_color {
        enum screenkey_led_indication indication;
        uint8_t r, g, b;
    };
    const struct base_color bases[] = {
        {SCREENKEY_LED_WAITING_APPROVAL, 0xFA, 0xCC, 0x15},
        {SCREENKEY_LED_WAITING_INPUT, 0xF9, 0x73, 0x16},
        {SCREENKEY_LED_ERROR, 0xEF, 0x44, 0x44},
    };

    for (size_t i = 0; i < 3; i++) {
        const struct base_color *base = &bases[i];
        const uint8_t max_r = (uint8_t)(((uint16_t)base->r * SCREENKEY_LED_MAX_LEVEL) / 255U);
        const uint8_t max_g = (uint8_t)(((uint16_t)base->g * SCREENKEY_LED_MAX_LEVEL) / 255U);
        const uint8_t max_b = (uint8_t)(((uint16_t)base->b * SCREENKEY_LED_MAX_LEVEL) / 255U);

        for (uint16_t frame = 0; frame <= 255; frame++) {
            const struct screenkey_led_color color =
                screenkey_led_color_for(base->indication, (uint8_t)frame);
            assert(color.r <= max_r);
            assert(color.g <= max_g);
            assert(color.b <= max_b);
        }
    }
}

static void test_led_period_matches_cycle(void) {
    const enum screenkey_led_indication non_off[] = {
        SCREENKEY_LED_WAITING_APPROVAL,
        SCREENKEY_LED_WAITING_INPUT,
        SCREENKEY_LED_ERROR,
    };

    for (size_t i = 0; i < 3; i++) {
        const enum screenkey_led_indication indication = non_off[i];
        const uint16_t period = screenkey_led_period_ms(indication);
        const uint8_t frame_count = screenkey_led_frame_count(indication);
        assert((uint32_t)period * (uint32_t)frame_count == SCREENKEY_LED_CYCLE_MS);
    }

    assert(screenkey_led_period_ms(SCREENKEY_LED_OFF) == 0);
    assert(screenkey_led_frame_count(SCREENKEY_LED_OFF) == 0);
}

static void test_led_off_is_black(void) {
    const uint8_t frames[] = {0, 1, 10, 19, 20, 100, 255};

    for (size_t i = 0; i < sizeof(frames) / sizeof(frames[0]); i++) {
        const struct screenkey_led_color color = screenkey_led_color_for(SCREENKEY_LED_OFF, frames[i]);
        assert(color.r == 0);
        assert(color.g == 0);
        assert(color.b == 0);
    }
}

int main(void) {
    test_state_mapping();
    test_breath_opacity_wave();
    test_working_geometry();
    test_working_start_follows_upright_orientation();
    test_logo_selection();
    test_slot_maps_to_matching_screen();
    test_slots_without_a_screen_are_ignored();
    test_led_indication_priority();
    test_led_indication_order_independent();
    test_led_blink_shape();
    test_led_completed_shape();
    test_led_breath_shape();
    test_led_color_wraps();
    test_led_color_max_40_percent();
    test_led_period_matches_cycle();
    test_led_off_is_black();
    return 0;
}
