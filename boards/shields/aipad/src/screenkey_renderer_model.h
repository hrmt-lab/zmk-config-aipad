#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCREENKEY_RENDERER_FRAME_COUNT 20
#define SCREENKEY_RENDERER_MAX_SEGMENTS 3

/* Physical ScreenKeys on this shield. Logical display slot n is drawn on
 * physical screen n; any higher slot has no screen here and is ignored.
 * Must stay in step with CONFIG_RAWHID_APP_AI_CLIENT_DISPLAY_SLOT_COUNT in
 * aipad.conf and with the number of panels declared in aipad.overlay. */
#define SCREENKEY_RENDERER_SCREEN_COUNT 4
#define SCREENKEY_RENDERER_NO_SCREEN (-1)

enum screenkey_renderer_mode {
    SCREENKEY_RENDERER_OFF,
    SCREENKEY_RENDERER_AVAILABLE,
    SCREENKEY_RENDERER_WORKING_MOVING,
    SCREENKEY_RENDERER_WAITING_APPROVAL,
    SCREENKEY_RENDERER_WAITING_INPUT,
    SCREENKEY_RENDERER_COMPLETED,
    SCREENKEY_RENDERER_ERROR,
};

enum screenkey_renderer_logo {
    SCREENKEY_RENDERER_LOGO_CODEX,
    SCREENKEY_RENDERER_LOGO_CLAUDE_CODE,
};

struct screenkey_renderer_segment {
    int16_t x;
    int16_t y;
    int16_t width;
    int16_t height;
};

enum screenkey_renderer_mode screenkey_renderer_mode_for_state(bool session_active,
                                                                uint8_t activity_state,
                                                                uint8_t work_phase);

enum screenkey_renderer_logo screenkey_renderer_logo_for_client_type(uint8_t client_type);

/* Maps a logical display slot to a physical screen index, or
 * SCREENKEY_RENDERER_NO_SCREEN when this shield has no screen for that slot. */
int screenkey_renderer_screen_for_slot(uint8_t display_slot);

uint8_t screenkey_renderer_breath_opacity(uint8_t frame);

size_t screenkey_renderer_working_segments(
    uint8_t frame, struct screenkey_renderer_segment segments[SCREENKEY_RENDERER_MAX_SEGMENTS]);
