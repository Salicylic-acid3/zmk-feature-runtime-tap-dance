/*
 * Copyright (c) 2026 Salicylic_acid3
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <cormoran/zmk/custom_settings.h>
#include <keebon/zmk/runtime_tap_dance.h>
#include <zmk/behavior.h>

LOG_MODULE_REGISTER(zmk_runtime_tap_dance, CONFIG_ZMK_RUNTIME_TAP_DANCE_LOG_LEVEL);

#define TAP_COUNT CONFIG_ZMK_RUNTIME_TAP_DANCE_MAX_TAPS
#define SLOT_COUNT CONFIG_ZMK_RUNTIME_TAP_DANCE_COUNT

/*
 * One array setting and one int setting per slot.
 *
 * The keys are built the same way in both places: spelled out here by the
 * preprocessor ("tap_dance0/taps") and rebuilt at runtime with snprintf when
 * a slot is looked up. Keeping the two in step is what key_for() below is
 * for -- there is no way to ask a setting for its own key at runtime.
 */

/* A tap that has not been configured: no behavior, no parameters. */
#define TAP_DEFAULT_ITEM(i, _)                                                                     \
    {                                                                                              \
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR, .behavior_value = {                        \
            .behavior_id = 0,                                                                      \
            .param1 = 0,                                                                           \
            .param2 = 0,                                                                           \
        }                                                                                          \
    }

/* One defaults array shared by every slot. The registration macro takes a
 * pointer, so there is no reason for each slot to carry its own copy of the
 * same zeroes -- and building it inside the per-slot macro would nest one
 * LISTIFY inside another, which does not reliably expand. */
static const struct zmk_custom_setting_value tap_dance_taps_defaults[TAP_COUNT] = {
    LISTIFY(TAP_COUNT, TAP_DEFAULT_ITEM, (, ), _)};

#define TAP_DANCE_SLOT_DEFINE(n, _)                                                                \
    ZMK_CUSTOM_SETTING_ARRAY_DEFINE(                                                               \
        tap_dance_taps_##n, ZMK_RUNTIME_TAP_DANCE_SUBSYSTEM_ID, "tap_dance" #n "/taps",            \
        ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR, TAP_COUNT,                                         \
        /* A new slot starts empty, so it is inert rather than doing                                \
           something nobody asked for. */                                                          \
        0, tap_dance_taps_defaults, ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,             \
        ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_PERMISSION_SECURE,              \
        /* No constraint: a BEHAVIOR value is already checked against the                          \
           behavior table and the behavior's parameter metadata by the value                       \
           type itself. ZMK_CUSTOM_SETTING_BEHAVIOR_ID is for an INT32 setting                     \
           that holds a behavior id, and applied here it rejected every write                      \
           with -EINVAL ("Invalid request") -- no tap could ever be set. */                        \
        ZMK_CUSTOM_SETTING_NO_CONSTRAINT);                                                         \
    /* What each tap count does when the key is still held once the dance                          \
       is decided -- one tap then hold, two taps then hold. Same length as                         \
       taps; an element holding &none means "no hold action for that count",                       \
       so the tap binding is held instead, as before. */                                          \
    ZMK_CUSTOM_SETTING_ARRAY_DEFINE(                                                               \
        tap_dance_holds_##n, ZMK_RUNTIME_TAP_DANCE_SUBSYSTEM_ID, "tap_dance" #n "/holds",          \
        ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR, TAP_COUNT, 0, tap_dance_taps_defaults,             \
        ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC, ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,     \
        ZMK_CUSTOM_SETTING_PERMISSION_SECURE, ZMK_CUSTOM_SETTING_NO_CONSTRAINT);                   \
    ZMK_CUSTOM_SETTING_DEFINE(                                                                     \
        tap_dance_term_##n, ZMK_RUNTIME_TAP_DANCE_SUBSYSTEM_ID, "tap_dance" #n "/term",            \
        ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,                                                       \
        ZMK_CUSTOM_SETTING_VALUE_INT32(CONFIG_ZMK_RUNTIME_TAP_DANCE_DEFAULT_TERM_MS),              \
        ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC, ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,     \
        ZMK_CUSTOM_SETTING_PERMISSION_SECURE, ZMK_CUSTOM_SETTING_RANGE_INT32(50, 1000));

LISTIFY(SLOT_COUNT, TAP_DANCE_SLOT_DEFINE, (), _)

/*
 * The capacity, published as a setting because nothing else carries it.
 *
 * An array setting's RPC value reports the array's *current* length, not how
 * long it may become, and the protocol has no field for the maximum. Without
 * this the app cannot tell a slot with room left from a full one, so it would
 * have to offer "add a tap" until the firmware refused -- an error where a
 * disabled button belongs.
 *
 * It is writable only in the sense that every setting is: the range pins it to
 * the one value that is true, so a write of anything else is rejected. The
 * number is decided at build time by Kconfig and cannot be otherwise.
 */
ZMK_CUSTOM_SETTING_DEFINE(tap_dance_max_taps, ZMK_RUNTIME_TAP_DANCE_SUBSYSTEM_ID, "max_taps",
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                          ZMK_CUSTOM_SETTING_VALUE_INT32(TAP_COUNT),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
                          ZMK_CUSTOM_SETTING_RANGE_INT32(TAP_COUNT, TAP_COUNT));

/* The holds arrays' capacity -- the same number, published separately because
 * its presence is how the app learns that this firmware has hold actions at
 * all: a holds array with nothing in it is invisible to ListSettings. */
ZMK_CUSTOM_SETTING_DEFINE(tap_dance_max_holds, ZMK_RUNTIME_TAP_DANCE_SUBSYSTEM_ID, "max_holds",
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                          ZMK_CUSTOM_SETTING_VALUE_INT32(TAP_COUNT),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
                          ZMK_CUSTOM_SETTING_RANGE_INT32(TAP_COUNT, TAP_COUNT));

/* Rebuild a slot's setting key. Mirrors the literals in the macro above. */
static int key_for(uint32_t slot, const char *suffix, char *out, size_t out_size) {
    int written = snprintf(out, out_size, "tap_dance%u/%s", slot, suffix);
    if (written < 0 || (size_t)written >= out_size) {
        return -ENAMETOOLONG;
    }
    return 0;
}

uint32_t zmk_runtime_tap_dance_tap_count(uint32_t slot) {
    if (slot >= SLOT_COUNT) {
        return 0;
    }

    char key[ZMK_RUNTIME_TAP_DANCE_KEY_MAX_LEN];
    if (key_for(slot, "taps", key, sizeof(key)) < 0) {
        return 0;
    }

    const struct zmk_custom_setting *setting =
        zmk_custom_setting_find_array(ZMK_RUNTIME_TAP_DANCE_SUBSYSTEM_ID, key);
    if (setting == NULL) {
        LOG_WRN("tap dance slot %u has no taps setting", slot);
        return 0;
    }

    uint32_t size = zmk_custom_setting_array_size(setting);
    return MIN(size, (uint32_t)TAP_COUNT);
}

static int read_binding(uint32_t slot, const char *array, uint32_t index,
                        struct zmk_behavior_binding *binding) {
    if (binding == NULL || slot >= SLOT_COUNT || index >= (uint32_t)TAP_COUNT) {
        return -EINVAL;
    }

    char key[ZMK_RUNTIME_TAP_DANCE_KEY_MAX_LEN];
    int ret = key_for(slot, array, key, sizeof(key));
    if (ret < 0) {
        return ret;
    }

    struct zmk_custom_setting_value value;
    ret = zmk_custom_setting_read_array_by_key(ZMK_RUNTIME_TAP_DANCE_SUBSYSTEM_ID, key, index,
                                               &value);
    if (ret < 0) {
        return ret;
    }
    if (value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR) {
        return -EINVAL;
    }

    /* Settings store a behavior by local ID; invoking one needs its device
     * name. A stored ID with no behavior behind it means this build does not
     * have what the slot was configured against -- report it rather than
     * invoking whatever happens to be at NULL. */
    const char *name = zmk_behavior_find_behavior_name_from_local_id(
        (zmk_behavior_local_id_t)value.behavior_value.behavior_id);
    if (name == NULL) {
        LOG_WRN("tap dance slot %u %s %u names behavior %u, which this build does not have", slot,
                array, index + 1, value.behavior_value.behavior_id);
        return -ENOENT;
    }

    binding->behavior_dev = name;
    binding->param1 = value.behavior_value.param1;
    binding->param2 = value.behavior_value.param2;
    return 0;
}

int zmk_runtime_tap_dance_binding(uint32_t slot, uint32_t index,
                                  struct zmk_behavior_binding *binding) {
    return read_binding(slot, "taps", index, binding);
}

int zmk_runtime_tap_dance_hold_binding(uint32_t slot, uint32_t index,
                                       struct zmk_behavior_binding *binding) {
    int ret = read_binding(slot, "holds", index, binding);
    if (ret < 0) {
        return ret;
    }
    /* The app fills a hold it has not been given with &none, which is how
     * "no hold action" is spelled in a behavior array. Report it as absent
     * so the caller falls back to holding the tap binding. */
    if (strcmp(binding->behavior_dev, "none") == 0) {
        return -ENOENT;
    }
    return 0;
}

uint32_t zmk_runtime_tap_dance_term_ms(uint32_t slot) {
    if (slot >= SLOT_COUNT) {
        return CONFIG_ZMK_RUNTIME_TAP_DANCE_DEFAULT_TERM_MS;
    }

    char key[ZMK_RUNTIME_TAP_DANCE_KEY_MAX_LEN];
    if (key_for(slot, "term", key, sizeof(key)) < 0) {
        return CONFIG_ZMK_RUNTIME_TAP_DANCE_DEFAULT_TERM_MS;
    }

    struct zmk_custom_setting_value value;
    if (zmk_custom_setting_read_by_key(ZMK_RUNTIME_TAP_DANCE_SUBSYSTEM_ID, key, &value) < 0 ||
        value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 || value.int32_value <= 0) {
        return CONFIG_ZMK_RUNTIME_TAP_DANCE_DEFAULT_TERM_MS;
    }

    return (uint32_t)value.int32_value;
}
