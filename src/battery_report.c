/*
 * Copyright (c) 2026 Salicylic_acid3
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Each half's battery percentage, where the app can read it.
 *
 * ZMK already knows both numbers: the local one from its own sensor, and the
 * peripheral's from ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING. It reports
 * them over the BLE Battery Service, which means a host connected over
 * Bluetooth can see them and a host connected over USB cannot -- and USB is
 * where someone sits when the keyboard is misbehaving badly enough that they
 * are asking about batteries in the first place.
 *
 * So the levels are published as custom settings instead, which reach the app
 * over whichever transport it is already using.
 *
 * They are written in TEMPORARY mode, and that choice is the whole design:
 *
 * - it never touches flash, so a value that changes every minute cannot wear
 *   the settings partition;
 * - it does not mark the setting dirty, so the app never shows a battery
 *   reading as an unsaved change or offers to save one;
 * - the temporary pool reuses a slot per setting, so repeated writes to the
 *   same key hold one slot rather than exhausting the pool.
 *
 * That last point has a cost: these settings hold a temporary slot each, for
 * as long as the keyboard is on. CONFIG_ZMK_CUSTOM_SETTINGS_TEMP_SLOTS
 * defaults to 2, which is exactly what a split keyboard uses here, so a board
 * enabling this module should raise it.
 */

#include <errno.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <cormoran/zmk/custom_settings.h>
#include <keebon/zmk/battery_report.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>

LOG_MODULE_REGISTER(zmk_battery_report, CONFIG_ZMK_BATTERY_REPORT_LOG_LEVEL);

#define SUBSYS ZMK_BATTERY_REPORT_SUBSYSTEM_ID

/* Percent, so the range is the unit. A client cannot usefully write these --
 * the next reading overwrites whatever it wrote -- but the constraint at
 * least says what the number means. */
#define BATTERY_CONSTRAINT ZMK_CUSTOM_SETTING_RANGE_INT32(0, 100)

/*
 * The half this firmware is running on. Named "central" even on a one-piece
 * keyboard, where it is simply the only battery there is: the alternative was
 * a name that changed meaning with the build, which is worse to read in an
 * app that talks to both kinds.
 */
ZMK_CUSTOM_SETTING_DEFINE(battery_report_central, SUBSYS, "central",
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, ZMK_CUSTOM_SETTING_VALUE_INT32(0),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_SECURE, BATTERY_CONSTRAINT);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)

#define PERIPHERAL_COUNT CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS

#define PERIPHERAL_SETTING_DEFINE(n, _)                                                            \
    ZMK_CUSTOM_SETTING_DEFINE(battery_report_peripheral_##n, SUBSYS, "peripheral" #n,               \
                              ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,                                 \
                              ZMK_CUSTOM_SETTING_VALUE_INT32(0),                                   \
                              ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,                       \
                              ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,                              \
                              ZMK_CUSTOM_SETTING_PERMISSION_SECURE, BATTERY_CONSTRAINT);

LISTIFY(PERIPHERAL_COUNT, PERIPHERAL_SETTING_DEFINE, (), _)

#endif /* CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING */

static void publish(const char *key, uint8_t state_of_charge) {
    const struct zmk_custom_setting_value value = {
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
        .int32_value = (int32_t)state_of_charge,
    };

    int ret = zmk_custom_setting_write_by_key(SUBSYS, key, &value,
                                              ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY);
    if (ret < 0) {
        /* -EBUSY means the temporary pool is full; see the file comment. It
         * is worth a warning rather than a silent miss, because the symptom
         * is a battery reading that never moves. */
        LOG_WRN("could not publish %s battery (%d)", key, ret);
    }
}

static int battery_report_listener(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *local = as_zmk_battery_state_changed(eh);
    if (local != NULL) {
        publish("central", local->state_of_charge);
        return ZMK_EV_EVENT_BUBBLE;
    }

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    const struct zmk_peripheral_battery_state_changed *peripheral =
        as_zmk_peripheral_battery_state_changed(eh);
    if (peripheral != NULL) {
        if (peripheral->source >= PERIPHERAL_COUNT) {
            LOG_WRN("battery from peripheral %u, but only %d are configured", peripheral->source,
                    PERIPHERAL_COUNT);
            return ZMK_EV_EVENT_BUBBLE;
        }

        char key[ZMK_BATTERY_REPORT_KEY_MAX_LEN];
        int written = snprintf(key, sizeof(key), "peripheral%u", peripheral->source);
        if (written < 0 || (size_t)written >= sizeof(key)) {
            return ZMK_EV_EVENT_BUBBLE;
        }

        publish(key, peripheral->state_of_charge);
    }
#endif

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zmk_battery_report, battery_report_listener);
ZMK_SUBSCRIPTION(zmk_battery_report, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
ZMK_SUBSCRIPTION(zmk_battery_report, zmk_peripheral_battery_state_changed);
#endif
