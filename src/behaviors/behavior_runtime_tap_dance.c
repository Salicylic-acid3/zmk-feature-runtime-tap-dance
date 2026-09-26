/*
 * Copyright (c) 2022 The ZMK Contributors
 * Copyright (c) 2026 Salicylic_acid3
 *
 * SPDX-License-Identifier: MIT
 *
 * Adapted from ZMK's app/src/behaviors/behavior_tap_dance.c.
 *
 * The state machine is upstream's and is deliberately left alone: it already
 * handles the split case, the "another key was pressed, decide now"
 * interruption, and the timer races around a tap that lands while the work
 * item is already running. Reimplementing that would be inventing new bugs.
 *
 * What differs is only where a binding comes from. Upstream keeps a fixed
 * array on the devicetree node, so every instance is one tap dance and
 * changing it means rebuilding. Here the node is a single behavior taking a
 * slot index as its parameter, and the taps for that slot are read out of
 * custom settings on each press -- which is what lets them be edited from
 * the app.
 *
 * Three consequences fall out of that and are worth knowing:
 *
 *  - the tap count is per slot and read live, so a slot reconfigured while a
 *    tap dance is mid-sequence uses whatever it reads next press;
 *  - a slot with no taps is inert, because a slot exists from first boot
 *    whether or not anyone has filled it in;
 *  - upstream's `ignore-key-positions` is not carried over. It is a
 *    devicetree list, and there is nowhere to put it that the app can edit
 *    yet. Its absence means an interrupting key always decides the dance,
 *    which is upstream's behaviour with an empty ignore list.
 *
 * One addition upstream does not have: a hold action per tap count. When the
 * dance is decided while the key is still down -- the wait ran out or another
 * key was pressed -- and the slot has a hold binding for that count, that is
 * what gets pressed and later released, instead of the tap binding. So one
 * key can be "tap: Escape, hold: Control, double-tap-hold: a layer". A slot
 * without hold actions behaves exactly as upstream: the tap binding is held.
 *
 * That changes one thing about the last tap. Upstream fires the last tap
 * the instant it is pressed, since nothing longer can follow; with a hold
 * action configured for it, the press has to wait out the term to learn
 * whether it is a tap or a hold, the same as every earlier count.
 */

#define DT_DRV_COMPAT keebon_zmk_behavior_runtime_tap_dance

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <keebon/zmk/runtime_tap_dance.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>

LOG_MODULE_DECLARE(zmk_runtime_tap_dance, CONFIG_ZMK_RUNTIME_TAP_DANCE_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define MAX_HELD CONFIG_ZMK_RUNTIME_TAP_DANCE_MAX_HELD
#define POSITION_FREE UINT32_MAX

struct active_tap_dance {
    /* Which configured tap dance this is -- the behavior's binding
     * parameter. Upstream stores a config pointer here instead. */
    uint32_t slot;
    int counter;
    uint32_t position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    uint8_t source;
#endif
    bool is_pressed;
    /* Which binding was pressed, so the release matches it. */
    bool hold_used;

    bool timer_started;
    bool timer_cancelled;
    bool tap_dance_decided;
    int64_t release_at;
    struct k_work_delayable release_timer;
};

static struct active_tap_dance active_tap_dances[MAX_HELD] = {};

static struct active_tap_dance *find_tap_dance(uint32_t position) {
    for (int i = 0; i < MAX_HELD; i++) {
        if (active_tap_dances[i].position == position && !active_tap_dances[i].timer_cancelled) {
            return &active_tap_dances[i];
        }
    }
    return NULL;
}

static int new_tap_dance(struct zmk_behavior_binding_event *event, uint32_t slot,
                         struct active_tap_dance **tap_dance) {
    for (int i = 0; i < MAX_HELD; i++) {
        struct active_tap_dance *const ref_dance = &active_tap_dances[i];
        if (ref_dance->position == POSITION_FREE) {
            ref_dance->slot = slot;
            ref_dance->counter = 0;
            ref_dance->position = event->position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
            ref_dance->source = event->source;
#endif
            ref_dance->release_at = 0;
            ref_dance->is_pressed = true;
            ref_dance->hold_used = false;
            ref_dance->timer_started = true;
            ref_dance->timer_cancelled = false;
            ref_dance->tap_dance_decided = false;
            *tap_dance = ref_dance;
            return 0;
        }
    }
    return -ENOMEM;
}

static void clear_tap_dance(struct active_tap_dance *tap_dance) {
    tap_dance->position = POSITION_FREE;
}

static int stop_timer(struct active_tap_dance *tap_dance) {
    int timer_cancel_result = k_work_cancel_delayable(&tap_dance->release_timer);
    if (timer_cancel_result == -EINPROGRESS) {
        // too late to cancel, we'll let the timer handler clear up.
        tap_dance->timer_cancelled = true;
    }
    return timer_cancel_result;
}

static void reset_timer(struct active_tap_dance *tap_dance,
                        struct zmk_behavior_binding_event event) {
    tap_dance->release_at = event.timestamp + zmk_runtime_tap_dance_term_ms(tap_dance->slot);
    int32_t ms_left = tap_dance->release_at - k_uptime_get();
    if (ms_left > 0) {
        k_work_schedule(&tap_dance->release_timer, K_MSEC(ms_left));
    }
}

/* True when the slot has a hold action for the count the dance is at. */
static bool has_hold_binding(const struct active_tap_dance *tap_dance) {
    struct zmk_behavior_binding binding;
    return tap_dance->counter >= 1 &&
           zmk_runtime_tap_dance_hold_binding(tap_dance->slot, tap_dance->counter - 1,
                                              &binding) == 0;
}

/* Build the event and binding for whichever tap the dance settled on: the
 * hold action when `hold` and the slot has one for this count, otherwise the
 * tap binding. Returns false when the slot has nothing stored for that tap,
 * which is not an error -- an unconfigured slot simply does nothing. */
static bool binding_for(struct active_tap_dance *tap_dance, int64_t timestamp, bool hold,
                        struct zmk_behavior_binding *binding,
                        struct zmk_behavior_binding_event *event) {
    if (tap_dance->counter < 1) {
        return false;
    }
    const uint32_t index = tap_dance->counter - 1;
    if (!hold || zmk_runtime_tap_dance_hold_binding(tap_dance->slot, index, binding) < 0) {
        if (zmk_runtime_tap_dance_binding(tap_dance->slot, index, binding) < 0) {
            return false;
        }
    }
    *event = (struct zmk_behavior_binding_event){
        .position = tap_dance->position,
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = tap_dance->source,
#endif
    };
    return true;
}

static int press_tap_dance_behavior(struct active_tap_dance *tap_dance, int64_t timestamp) {
    tap_dance->tap_dance_decided = true;
    /* Decided with the key still down: this is a hold, if the slot has a
     * hold action for this count. Decided after release: a tap. */
    tap_dance->hold_used = tap_dance->is_pressed && has_hold_binding(tap_dance);

    struct zmk_behavior_binding binding;
    struct zmk_behavior_binding_event event;
    if (!binding_for(tap_dance, timestamp, tap_dance->hold_used, &binding, &event)) {
        LOG_DBG("tap dance slot %u has nothing for tap %d", tap_dance->slot, tap_dance->counter);
        return 0;
    }
    return zmk_behavior_invoke_binding(&binding, event, true);
}

static int release_tap_dance_behavior(struct active_tap_dance *tap_dance, int64_t timestamp) {
    struct zmk_behavior_binding binding;
    struct zmk_behavior_binding_event event;
    bool have_binding = binding_for(tap_dance, timestamp, tap_dance->hold_used, &binding, &event);

    /* Cleared before invoking, exactly as upstream does: the slot has to be
     * free again before anything the binding triggers can look for it. */
    clear_tap_dance(tap_dance);

    if (!have_binding) {
        return 0;
    }
    return zmk_behavior_invoke_binding(&binding, event, false);
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
/* The parameter is a slot index. Published as a range so the app offers a
 * picker over the configured slots instead of leaving the parameter at 0 --
 * which is what an empty metadata set did: only `&rtd 0` was ever reachable
 * from the keymap editor. */
static const struct behavior_parameter_value_metadata slot_param_values[] = {
    {
        .display_name = "Tap dance",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
        .range = {.min = 0, .max = CONFIG_ZMK_RUNTIME_TAP_DANCE_COUNT - 1},
    },
};

static const struct behavior_parameter_metadata_set slot_metadata_set = {
    .param1_values = slot_param_values,
    .param1_values_len = ARRAY_SIZE(slot_param_values),
};

static const struct behavior_parameter_metadata slot_metadata = {
    .sets_len = 1,
    .sets = &slot_metadata_set,
};

static int runtime_tap_dance_parameter_metadata(const struct device *dev,
                                                struct behavior_parameter_metadata *metadata) {
    ARG_UNUSED(dev);
    *metadata = slot_metadata;
    return 0;
}
#endif /* CONFIG_ZMK_BEHAVIOR_METADATA */

static int on_tap_dance_binding_pressed(struct zmk_behavior_binding *binding,
                                        struct zmk_behavior_binding_event event) {
    const uint32_t slot = binding->param1;
    const uint32_t taps = zmk_runtime_tap_dance_tap_count(slot);

    if (taps == 0) {
        /* Nothing configured. Bubble rather than swallowing the press, so
         * the key is visibly inert instead of mysteriously dead. */
        LOG_DBG("tap dance slot %u is not configured", slot);
        return ZMK_BEHAVIOR_TRANSPARENT;
    }

    struct active_tap_dance *tap_dance = find_tap_dance(event.position);
    if (tap_dance == NULL) {
        if (new_tap_dance(&event, slot, &tap_dance) == -ENOMEM) {
            LOG_ERR("no room for another tap dance; raise "
                    "CONFIG_ZMK_RUNTIME_TAP_DANCE_MAX_HELD");
            return ZMK_BEHAVIOR_OPAQUE;
        }
    }

    tap_dance->is_pressed = true;
    stop_timer(tap_dance);

    if (tap_dance->counter < (int)taps) {
        tap_dance->counter++;
    }
    if (tap_dance->counter == (int)taps && !has_hold_binding(tap_dance)) {
        /* The last tap needs no waiting: there is nothing longer to become.
         * Unless it has a hold action -- then it still has to become either
         * a tap or a hold, which only the release or the timer can say. */
        press_tap_dance_behavior(tap_dance, event.timestamp);
        return ZMK_EV_EVENT_BUBBLE;
    }

    reset_timer(tap_dance, event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_tap_dance_binding_released(struct zmk_behavior_binding *binding,
                                         struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);

    struct active_tap_dance *tap_dance = find_tap_dance(event.position);
    if (tap_dance == NULL) {
        /* Reached when the press bubbled because the slot was unconfigured;
         * there is no dance to release. */
        return ZMK_BEHAVIOR_TRANSPARENT;
    }

    tap_dance->is_pressed = false;
    if (tap_dance->tap_dance_decided) {
        release_tap_dance_behavior(tap_dance, event.timestamp);
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static void behavior_runtime_tap_dance_timer_handler(struct k_work *item) {
    struct k_work_delayable *d_work = k_work_delayable_from_work(item);
    struct active_tap_dance *tap_dance =
        CONTAINER_OF(d_work, struct active_tap_dance, release_timer);

    if (tap_dance->position == POSITION_FREE || tap_dance->timer_cancelled) {
        return;
    }

    press_tap_dance_behavior(tap_dance, tap_dance->release_at);
    if (tap_dance->is_pressed) {
        /* Still held: the release comes from the key, not from here. */
        return;
    }
    release_tap_dance_behavior(tap_dance, tap_dance->release_at);
}

static const struct behavior_driver_api behavior_runtime_tap_dance_driver_api = {
    .binding_pressed = on_tap_dance_binding_pressed,
    .binding_released = on_tap_dance_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = runtime_tap_dance_parameter_metadata,
#endif
};

static int tap_dance_position_state_changed_listener(const zmk_event_t *eh);

ZMK_LISTENER(behavior_runtime_tap_dance, tap_dance_position_state_changed_listener);
ZMK_SUBSCRIPTION(behavior_runtime_tap_dance, zmk_position_state_changed);

/* A press anywhere else ends the wait: whatever the counter reached is what
 * the user meant, because they have moved on to another key. */
static int tap_dance_position_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL || !ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    for (int i = 0; i < MAX_HELD; i++) {
        struct active_tap_dance *tap_dance = &active_tap_dances[i];
        if (tap_dance->position == POSITION_FREE || tap_dance->position == ev->position) {
            continue;
        }

        stop_timer(tap_dance);
        if (!tap_dance->tap_dance_decided) {
            press_tap_dance_behavior(tap_dance, ev->timestamp);
            if (!tap_dance->is_pressed) {
                release_tap_dance_behavior(tap_dance, ev->timestamp);
            }
            return ZMK_EV_EVENT_BUBBLE;
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int behavior_runtime_tap_dance_init(const struct device *dev) {
    ARG_UNUSED(dev);

    static bool init_first_run = true;
    if (init_first_run) {
        for (int i = 0; i < MAX_HELD; i++) {
            k_work_init_delayable(&active_tap_dances[i].release_timer,
                                  behavior_runtime_tap_dance_timer_handler);
            clear_tap_dance(&active_tap_dances[i]);
        }
        init_first_run = false;
    }
    return 0;
}

#define TAP_DANCE_INST(n)                                                                          \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_runtime_tap_dance_init, NULL, NULL, NULL, POST_KERNEL,     \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_runtime_tap_dance_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TAP_DANCE_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
