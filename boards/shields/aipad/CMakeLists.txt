if(CONFIG_SHIELD_AIPAD)
  target_sources(app PRIVATE
    src/claude_code_logo.c
    src/codex_logo.c
    src/encoder_probe.c
    src/screenkey_display.c
    src/screenkey_renderer_model.c
    src/status_screen.c
  )
endif()
