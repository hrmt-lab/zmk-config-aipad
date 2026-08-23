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

int main(void) {
    test_state_mapping();
    test_breath_opacity_wave();
    test_working_geometry();
    test_working_start_follows_upright_orientation();
    test_logo_selection();
    test_slot_maps_to_matching_screen();
    test_slots_without_a_screen_are_ignored();
    return 0;
}
