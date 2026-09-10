zephyr_include_directories(include)

if(CONFIG_ZMK_RUNTIME_TAP_DANCE)
    target_sources(app PRIVATE
        src/runtime_tap_dance.c
        src/behaviors/behavior_runtime_tap_dance.c
    )
endif()
