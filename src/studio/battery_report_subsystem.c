/*
 * Copyright (c) 2026 Salicylic_acid3
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Registers this module as a Studio custom subsystem.
 *
 * There is no RPC here. The battery levels are ordinary custom settings and
 * travel over the settings RPC; this file exists because of how a setting
 * reaches the app at all.
 *
 * Every setting carries a `custom_subsystem_id` string, and the settings
 * handler turns that string into the index the app sees by looking it up in
 * the registered Studio subsystems. A setting whose id matches no registered
 * subsystem cannot be given an index, so it is dropped from ListSettings
 * entirely -- silently, and from every listing. Without the registration
 * below, the levels would exist on the keyboard and be invisible to the app.
 *
 * The same trap cost a day on the tap dance module. It is written out again
 * here rather than cross-referenced, because the next person to add a
 * settings-only module will be reading this file, not that one.
 */

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/studio/custom.h>

LOG_MODULE_DECLARE(zmk_battery_report, CONFIG_ZMK_BATTERY_REPORT_LOG_LEVEL);

/* Declared before the registration because the macro takes the handler by
 * name; defining it afterwards keeps the "why" next to the refusal. */
static bool battery_report_rpc_handle_request(const zmk_custom_CallRequest *req,
                                              pb_callback_t *res);

/*
 * Unsecured, matching the settings themselves. A battery percentage is not a
 * secret, and being able to read it while locked is the point: the app should
 * be able to show why a keyboard is behaving badly without being unlocked
 * first.
 */
static struct zmk_rpc_custom_subsystem_meta battery_report_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(),
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(keebon__battery, &battery_report_meta, battery_report_rpc_handle_request);

static bool battery_report_rpc_handle_request(const zmk_custom_CallRequest *req,
                                              pb_callback_t *res) {
    ARG_UNUSED(req);
    ARG_UNUSED(res);

    /* This subsystem exists to be named, not to be called. */
    LOG_WRN("battery report has no RPC of its own; use the settings RPC");
    return false;
}
