zephyr_include_directories(include)

if(CONFIG_ZMK_RUNTIME_TAP_DANCE)
    target_sources(app PRIVATE
        src/runtime_tap_dance.c
        src/behaviors/behavior_runtime_tap_dance.c
    )

    if(CONFIG_ZMK_RUNTIME_TAP_DANCE_STUDIO_RPC)
        target_sources(app PRIVATE src/studio/runtime_tap_dance_subsystem.c)
    endif()
endif()
