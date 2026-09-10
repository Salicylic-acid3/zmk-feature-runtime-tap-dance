/*
 * Copyright (c) 2026 Salicylic_acid3
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Registers this module as a Studio custom subsystem.
 *
 * There is no RPC here and there never will be -- the taps are ordinary
 * custom settings, edited over the settings RPC. This file exists because of
 * how a setting reaches the app at all.
 *
 * Every setting carries a `custom_subsystem_id` string, and the settings
 * handler turns that string into the index the app sees by looking it up in
 * the registered Studio subsystems. A setting whose id matches no registered
 * subsystem cannot be given an index, so it is dropped from ListSettings
 * entirely -- silently, and from every listing, not just a scoped one. The
 * effect is a module whose settings exist on the keyboard and are invisible
 * to the app.
 *
 * So the registration below is not about serving requests. It is the entry in
 * the table that lets "keebon__runtime_tap_dance" resolve to a number. The
 * handler is required by the macro and refuses everything, which is correct:
 * nothing should ever send this subsystem a request.
 */

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/studio/custom.h>

LOG_MODULE_DECLARE(zmk_runtime_tap_dance, CONFIG_ZMK_RUNTIME_TAP_DANCE_LOG_LEVEL);

/* Declared before the registration because the macro takes the handler by
 * name; defining it afterwards keeps the "why" comment next to the refusal. */
static bool runtime_tap_dance_rpc_handle_request(const zmk_custom_CallRequest *req,
                                                 pb_callback_t *res);

/*
 * Unsecured, matching the settings themselves: listing which tap dances exist
 * says nothing secret. The individual settings still carry their own
 * read/write permissions, and the taps are written only while unlocked.
 */
static struct zmk_rpc_custom_subsystem_meta runtime_tap_dance_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(),
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(keebon__runtime_tap_dance, &runtime_tap_dance_meta,
                         runtime_tap_dance_rpc_handle_request);

static bool runtime_tap_dance_rpc_handle_request(const zmk_custom_CallRequest *req,
                                                 pb_callback_t *res) {
    ARG_UNUSED(req);
    ARG_UNUSED(res);

    /* This subsystem exists to be named, not to be called. Anything arriving
     * here is a client talking to the wrong subsystem. */
    LOG_WRN("runtime tap dance has no RPC of its own; use the settings RPC");
    return false;
}
