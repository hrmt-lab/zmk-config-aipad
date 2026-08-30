#include "screenkey_renderer_model.h"

#define BORDER_INSET 2
#define BORDER_THICKNESS 4
#define BORDER_SIDE_LENGTH 124
#define BORDER_PERIMETER (BORDER_SIDE_LENGTH * 4)
#define WORKING_LINE_LENGTH (BORDER_PERIMETER / 4)
#define WORKING_START_OFFSET (BORDER_SIDE_LENGTH * 3)
#define BREATH_OPACITY_MIN 64
#define BREATH_OPACITY_MAX 255
#define BREATH_HALF_FRAME_COUNT (SCREENKEY_RENDERER_FRAME_COUNT / 2)

enum screenkey_renderer_mode screenkey_renderer_mode_for_state(bool session_active,
                                                                uint8_t activity_state,
                                                                uint8_t work_phase) {
    if (!session_active) {
        return SCREENKEY_RENDERER_OFF;
    }

    switch (activity_state) {
    case 1:
        return SCREENKEY_RENDERER_AVAILABLE;
    case 2:
        /* Every work phase shares the moving line: the phase is reported for
         * host-side diagnostics, not for a separate WORKING appearance. */
        (void)work_phase;
        return SCREENKEY_RENDERER_WORKING_MOVING;
    case 3:
        return SCREENKEY_RENDERER_WAITING_APPROVAL;
    case 4:
        return SCREENKEY_RENDERER_WAITING_INPUT;
    case 5:
        return SCREENKEY_RENDERER_COMPLETED;
    case 6:
        return SCREENKEY_RENDERER_ERROR;
    default:
        return SCREENKEY_RENDERER_OFF;
    }
}

enum screenkey_renderer_logo screenkey_renderer_logo_for_client_type(uint8_t client_type) {
    /* Only the Claude Code client type gets its own logo. Anything else keeps
     * the existing Codex artwork so unknown clients never blank the screen. */
    return client_type == 2 ? SCREENKEY_RENDERER_LOGO_CLAUDE_CODE
                            : SCREENKEY_RENDERER_LOGO_CODEX;
}

int screenkey_renderer_screen_for_slot(uint8_t display_slot) {
    /* Slot n is drawn on physical screen n. The host may address up to eight
     * logical slots, so anything beyond this shield's screens is ignored rather
     * than folded onto screen 0. */
    if (display_slot >= SCREENKEY_RENDERER_SCREEN_COUNT) {
        return SCREENKEY_RENDERER_NO_SCREEN;
    }
    return (int)display_slot;
}

uint8_t screenkey_renderer_breath_opacity(uint8_t frame) {
    const uint8_t normalized_frame = frame % SCREENKEY_RENDERER_FRAME_COUNT;
    const uint8_t distance = normalized_frame <= BREATH_HALF_FRAME_COUNT
                                 ? normalized_frame
                                 : SCREENKEY_RENDERER_FRAME_COUNT - normalized_frame;

    return BREATH_OPACITY_MIN +
           ((BREATH_OPACITY_MAX - BREATH_OPACITY_MIN) * distance) /
               BREATH_HALF_FRAME_COUNT;
}

static struct screenkey_renderer_segment segment_for_edge(uint8_t edge, int16_t offset,
                                                           int16_t length) {
    switch (edge) {
    case 0:
        return (struct screenkey_renderer_segment){
            .x = BORDER_INSET + offset,
            .y = BORDER_INSET,
            .width = length,
            .height = BORDER_THICKNESS,
        };
    case 1:
        return (struct screenkey_renderer_segment){
            .x = 128 - BORDER_INSET - BORDER_THICKNESS,
            .y = BORDER_INSET + offset,
            .width = BORDER_THICKNESS,
            .height = length,
        };
    case 2:
        return (struct screenkey_renderer_segment){
            .x = 128 - BORDER_INSET - offset - length,
            .y = 128 - BORDER_INSET - BORDER_THICKNESS,
            .width = length,
            .height = BORDER_THICKNESS,
        };
    default:
        return (struct screenkey_renderer_segment){
            .x = BORDER_INSET,
            .y = 128 - BORDER_INSET - offset - length,
            .width = BORDER_THICKNESS,
            .height = length,
        };
    }
}

size_t screenkey_renderer_working_segments(
    uint8_t frame, struct screenkey_renderer_segment segments[SCREENKEY_RENDERER_MAX_SEGMENTS]) {
    const int16_t normalized_frame = frame % SCREENKEY_RENDERER_FRAME_COUNT;
    int16_t cursor = WORKING_START_OFFSET +
                     (normalized_frame * BORDER_PERIMETER) / SCREENKEY_RENDERER_FRAME_COUNT;
    int16_t remaining = WORKING_LINE_LENGTH;
    size_t count = 0;

    while (remaining > 0 && count < SCREENKEY_RENDERER_MAX_SEGMENTS) {
        cursor %= BORDER_PERIMETER;
        const uint8_t edge = cursor / BORDER_SIDE_LENGTH;
        const int16_t offset = cursor % BORDER_SIDE_LENGTH;
        const int16_t available = BORDER_SIDE_LENGTH - offset;
        const int16_t length = remaining < available ? remaining : available;

        segments[count++] = segment_for_edge(edge, offset, length);
        cursor += length;
        remaining -= length;
    }

    return count;
}

static uint8_t scale8(uint8_t value, uint8_t level) {
    return (uint8_t)(((uint16_t)value * level) / 255U);
}

enum screenkey_led_indication
screenkey_led_indication_for_modes(const enum screenkey_renderer_mode *modes, size_t count) {
    bool saw_waiting_approval = false;
    bool saw_waiting_input = false;
    bool saw_error = false;
    bool saw_completed = false;

    /* Scan the whole array before deciding anything: returning early on the
     * first match seen would make the result depend on array order, and the
     * caller cannot guarantee any particular order across screens. */
    for (size_t index = 0; index < count; index++) {
        switch (modes[index]) {
        case SCREENKEY_RENDERER_WAITING_APPROVAL:
            saw_waiting_approval = true;
            break;
        case SCREENKEY_RENDERER_WAITING_INPUT:
            saw_waiting_input = true;
            break;
        case SCREENKEY_RENDERER_ERROR:
            saw_error = true;
            break;
        case SCREENKEY_RENDERER_COMPLETED:
            saw_completed = true;
            break;
        default:
            /* OFF / AVAILABLE / WORKING_MOVING never need the shared LED, so
             * they contribute nothing. */
            break;
        }
    }

    if (saw_waiting_approval) {
        return SCREENKEY_LED_WAITING_APPROVAL;
    }
    if (saw_waiting_input) {
        return SCREENKEY_LED_WAITING_INPUT;
    }
    if (saw_error) {
        return SCREENKEY_LED_ERROR;
    }
    /* Ranked last on purpose: completion is a notice, not a request for the
     * user to act, so it must never hide an approval, input or error state on
     * another screen. */
    if (saw_completed) {
        return SCREENKEY_LED_COMPLETED;
    }
    return SCREENKEY_LED_OFF;
}

uint16_t screenkey_led_period_ms(enum screenkey_led_indication indication) {
    /* OFF and COMPLETED are both static: neither animates, so a zero tick
     * period tells the caller to stop rescheduling instead of ticking an
     * unchanging LED forever. COMPLETED still ends on its own, but that is a
     * single wake-up at SCREENKEY_COMPLETED_HOLD_MS, which the driver
     * schedules itself rather than an animation period. */
    return (indication == SCREENKEY_LED_OFF || indication == SCREENKEY_LED_COMPLETED) ? 0 : 100;
}

uint8_t screenkey_led_frame_count(enum screenkey_led_indication indication) {
    switch (indication) {
    case SCREENKEY_LED_OFF:
        return 0;
    case SCREENKEY_LED_WAITING_INPUT:
        return SCREENKEY_RENDERER_FRAME_COUNT;
    case SCREENKEY_LED_COMPLETED:
        /* One frame, not zero: the colour is constant, but the caller folds
         * its frame counter through this value and must not divide by zero. */
        return 1;
    case SCREENKEY_LED_WAITING_APPROVAL:
    case SCREENKEY_LED_ERROR:
    default:
        /* Blink shares the same 2000ms cycle as the breathing wave so every
         * non-OFF indication satisfies period_ms * frame_count == CYCLE_MS. */
        return 20;
    }
}

struct screenkey_led_color screenkey_led_color_for(enum screenkey_led_indication indication,
                                                   uint8_t frame) {
    if (indication == SCREENKEY_LED_OFF) {
        return (struct screenkey_led_color){0, 0, 0};
    }

    struct screenkey_led_color base;
    switch (indication) {
    case SCREENKEY_LED_WAITING_APPROVAL:
        base = (struct screenkey_led_color){0xFA, 0xCC, 0x15};
        break;
    case SCREENKEY_LED_WAITING_INPUT:
        base = (struct screenkey_led_color){0xF9, 0x73, 0x16};
        break;
    case SCREENKEY_LED_COMPLETED:
        /* A pure green, deliberately NOT the 0x22C55E the finished border is
         * drawn in. Feeding a screen colour straight to the chain does not
         * reproduce it: the panel applies an sRGB transfer curve, while a
         * WS2812 channel is linear PWM, so 0x22C55E's blue (0x5E, 48% of its
         * green) lands far brighter on the LED than it looks on the panel and
         * turns the green visibly cyan. The green channel keeps the same
         * 0xC5 it had, so only the cast changes, not the brightness. The
         * border in status_screen.c still uses 0x22C55E - matching the two by
         * eye is what makes them read as one indication, not matching their
         * hex. */
        base = (struct screenkey_led_color){0x00, 0xC5, 0x00};
        break;
    case SCREENKEY_LED_ERROR:
    default:
        base = (struct screenkey_led_color){0xEF, 0x44, 0x44};
        break;
    }

    uint8_t level;
    if (indication == SCREENKEY_LED_COMPLETED) {
        /* Steady at the ceiling for the whole hold: completion is reported by
         * how long the green stays lit, not by any movement in it. */
        level = SCREENKEY_LED_MAX_LEVEL;
    } else if (indication == SCREENKEY_LED_WAITING_INPUT) {
        /* Reuse the same triangle wave the per-screen border breathes with,
         * just rescaled onto the LED's lower brightness ceiling. */
        level = scale8(SCREENKEY_LED_MAX_LEVEL, screenkey_renderer_breath_opacity(frame));
    } else {
        /* Blink: half the cycle fully on at the 40% ceiling, half fully
         * dark, folded onto the same 20-frame cycle as the breathing wave. */
        const uint8_t normalized_frame = frame % SCREENKEY_RENDERER_FRAME_COUNT;
        level = normalized_frame < (SCREENKEY_RENDERER_FRAME_COUNT / 2) ? SCREENKEY_LED_MAX_LEVEL
                                                                        : 0;
    }

    return (struct screenkey_led_color){
        .r = scale8(base.r, level),
        .g = scale8(base.g, level),
        .b = scale8(base.b, level),
    };
}
