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
