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
 * as long as the keyboard is on -- the level per half, and the USB flag.
 * CONFIG_ZMK_CUSTOM_SETTINGS_TEMP_SLOTS defaults to 2, so a board enabling
 * this module should raise it (the shipped boards use 4).
 *
 * One exception to "never touches flash": when USB power arrives, the last
 * reading taken on battery is written once in PERSIST mode, so that a
 * keyboard rebooted on USB still shows the cell's last known level rather
 * than 0. One write per plug-in.
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
#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>
#endif

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

/*
 * True while this half is powered over USB. Then "central" is not a live
 * reading: the sensor measures the USB rail, which says nothing about the
 * cell, so the value shown is the last one measured on battery (see
 * battery_report_listener), and the app can say so.
 */
ZMK_CUSTOM_SETTING_DEFINE(battery_report_central_on_usb, SUBSYS, "central_on_usb",
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL, ZMK_CUSTOM_SETTING_VALUE_BOOL(false),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_SECURE, ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

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

#if IS_ENABLED(CONFIG_ZMK_USB)
static bool on_usb;
static bool have_battery_reading;
static uint8_t last_on_battery;

static void publish_on_usb(bool powered) {
    const struct zmk_custom_setting_value value = {
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL,
        .bool_value = powered,
    };
    int ret = zmk_custom_setting_write_by_key(SUBSYS, "central_on_usb", &value,
                                              ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY);
    if (ret < 0) {
        LOG_WRN("could not publish USB state (%d)", ret);
    }
}

/*
 * USB just arrived: the last reading taken on battery is written once, to
 * flash, so that it is still the number shown after a reboot on USB (a
 * firmware update, say) -- one write per plug-in, not one per minute. From
 * here until USB goes away the sensor's readings are of the USB rail and are
 * not published.
 */
static void remember_last_on_battery(void) {
    if (!have_battery_reading) {
        return;
    }
    const struct zmk_custom_setting_value value = {
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
        .int32_value = (int32_t)last_on_battery,
    };
    int ret = zmk_custom_setting_write_by_key(SUBSYS, "central", &value,
                                              ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);
    if (ret < 0) {
        LOG_WRN("could not keep the last battery reading (%d)", ret);
    }
}
#endif

static int battery_report_listener(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *local = as_zmk_battery_state_changed(eh);
    if (local != NULL) {
#if IS_ENABLED(CONFIG_ZMK_USB)
        if (zmk_usb_is_powered()) {
            /* The USB rail, not the cell. Keep showing the last real one. */
            return ZMK_EV_EVENT_BUBBLE;
        }
        have_battery_reading = true;
        last_on_battery = local->state_of_charge;
#endif
        publish("central", local->state_of_charge);
        return ZMK_EV_EVENT_BUBBLE;
    }

#if IS_ENABLED(CONFIG_ZMK_USB)
    if (as_zmk_custom_settings_initialized(eh) != NULL) {
        on_usb = zmk_usb_is_powered();
        publish_on_usb(on_usb);
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_usb_conn_state_changed *usb = as_zmk_usb_conn_state_changed(eh);
    if (usb != NULL) {
        const bool powered = zmk_usb_is_powered();
        if (powered != on_usb) {
            on_usb = powered;
            publish_on_usb(powered);
            if (powered) {
                remember_last_on_battery();
            }
        }
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

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
#if IS_ENABLED(CONFIG_ZMK_USB)
ZMK_SUBSCRIPTION(zmk_battery_report, zmk_usb_conn_state_changed);

/* The USB state at boot, once the settings registry is ready to take a
 * write: a keyboard that is plugged in when it starts would otherwise look
 * like it was on battery until USB changed state. */
ZMK_SUBSCRIPTION(zmk_battery_report, zmk_custom_settings_initialized);
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
ZMK_SUBSCRIPTION(zmk_battery_report, zmk_peripheral_battery_state_changed);
#endif
