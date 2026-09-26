/*
 * Copyright (c) 2026 Salicylic_acid3
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/behavior.h>

/*
 * Tap dance whose taps are settings, not devicetree.
 *
 * ZMK already has a tap-dance behavior, and its state machine is the right
 * one -- this module reuses it almost verbatim. The single thing that
 * changes is where the bindings come from: `zmk,behavior-tap-dance` reads a
 * fixed array out of its own devicetree node, which means changing what two
 * taps do is a firmware rebuild. On a 30% keyboard, where tap dance is how
 * the missing keys come back, that is the wrong place for the decision.
 *
 * So each slot's taps are an array setting of behavior bindings, and its
 * tapping term is an int setting. Both are ordinary custom settings, which
 * is what makes them editable from the app without a line of new protobuf:
 * the settings RPC and the app's behavior picker already exist.
 *
 * A slot with no taps configured is inert -- pressing it does nothing, the
 * same way an unbound runtime-macro slot plays nothing. That matters because
 * every slot exists from the first boot, whether or not anyone has filled
 * it in yet.
 */

#define ZMK_RUNTIME_TAP_DANCE_SUBSYSTEM_ID "keebon__runtime_tap_dance"

/* Longest key this module builds, e.g. "tap_dance15/holds". */
#define ZMK_RUNTIME_TAP_DANCE_KEY_MAX_LEN 24

/**
 * @brief How many taps slot @p slot currently has configured.
 *
 * @return 0..CONFIG_ZMK_RUNTIME_TAP_DANCE_MAX_TAPS, or 0 for an unknown slot.
 *         Zero means the slot is unconfigured and does nothing when pressed.
 */
uint32_t zmk_runtime_tap_dance_tap_count(uint32_t slot);

/**
 * @brief The binding slot @p slot runs after @p index + 1 taps.
 *
 * Resolves the stored behavior local ID back to a behavior device name, so
 * the result is ready for zmk_behavior_invoke_binding().
 *
 * @retval 0 on success.
 * @retval -EINVAL for an out-of-range slot or index.
 * @retval -ENOENT when the stored binding names a behavior this firmware
 *         does not have -- which happens if a keymap was configured against
 *         a build that had it and then flashed with one that does not.
 */
int zmk_runtime_tap_dance_binding(uint32_t slot, uint32_t index,
                                  struct zmk_behavior_binding *binding);

/**
 * @brief The binding slot @p slot holds when the key is still down after
 *        @p index + 1 taps, if one is configured.
 *
 * A dance decided while the key is still pressed -- one tap then hold, two
 * taps then hold -- runs this instead of the tap binding, so "tap for
 * Escape, hold for Control" and "double-tap-hold for something else" can
 * share a key. Without one, the tap binding is held, as it always was.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an out-of-range slot or index.
 * @retval -ENOENT when no hold action is configured for that count (the
 *         element is missing or holds &none), or it names a behavior this
 *         firmware does not have.
 */
int zmk_runtime_tap_dance_hold_binding(uint32_t slot, uint32_t index,
                                       struct zmk_behavior_binding *binding);

/**
 * @brief How long slot @p slot waits for the next tap, in milliseconds.
 *
 * Falls back to CONFIG_ZMK_RUNTIME_TAP_DANCE_DEFAULT_TERM_MS when the slot
 * has no stored term, so a fresh slot behaves sensibly rather than firing
 * instantly.
 */
uint32_t zmk_runtime_tap_dance_term_ms(uint32_t slot);
