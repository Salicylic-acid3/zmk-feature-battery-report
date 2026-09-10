# zmk-feature-battery-report

Each half's battery percentage, where Keeb-On! Studio can read it.

## Why this exists

ZMK already knows both numbers. It reports them over the BLE Battery Service,
which means they are visible to a host connected over Bluetooth and invisible
to one connected over USB.

That is backwards for the case that matters. Someone asking "how is the
battery?" is usually asking because the keyboard is misbehaving — dropping
keys, a trackpad that only wakes when a cable is plugged in — and by then they
are on USB, where ZMK's own reporting says nothing.

This module publishes the same numbers as custom settings, so they arrive over
whichever transport the app is already using.

## What is published

Under the `keebon__battery` subsystem:

| Key            | Meaning                                              |
|----------------|------------------------------------------------------|
| `central`      | this half's battery, 0–100                            |
| `peripheral0`… | each split peripheral's battery, 0–100                 |

`central` is named that even on a one-piece keyboard, where it is simply the
only battery there is — better than a name whose meaning changes with the
build, in an app that talks to both kinds.

The peripheral keys exist only when
`CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING` is on. Without it the
central never learns the other half's level and there is nothing to publish.

## TEMPORARY mode, and what it costs

The writes use `ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY`, which is the whole
design:

- it never reaches flash, so a value that changes every minute cannot wear the
  settings partition;
- it does not mark the setting dirty, so the app never shows a battery reading
  as an unsaved change or offers to save one;
- the temporary pool reuses a slot per setting, so repeated writes to the same
  key hold one slot rather than exhausting the pool.

The cost is that slot. Each published key holds one for as long as the
keyboard is on, and `CONFIG_ZMK_CUSTOM_SETTINGS_TEMP_SLOTS` defaults to **2** —
exactly what a split keyboard uses here, leaving nothing for anything else.
**A board enabling this module should raise it**, e.g.

```conf
CONFIG_ZMK_CUSTOM_SETTINGS_TEMP_SLOTS=4
```

A full pool is not silent: the write fails with `-EBUSY` and the module logs a
warning, because the symptom otherwise is a battery reading that never moves.

## Why there is a Studio subsystem with no RPC

`src/studio/battery_report_subsystem.c` registers this module as a Studio
custom subsystem and handles nothing.

A setting carries a `custom_subsystem_id` string, and the settings RPC turns it
into the index the app sees by looking it up in the registered Studio
subsystems. A setting whose id matches no registered subsystem cannot be given
an index, so it is dropped from `ListSettings` — silently, and from every
listing. Without that registration the levels exist on the keyboard and the app
simply cannot see them.

## Setup

`config/west.yml`:

```yaml
    - name: zmk-feature-battery-report
      remote: salicylic-acid3
      revision: master
```

Board `.conf`, on the half that talks to Studio:

```conf
CONFIG_ZMK_BATTERY_REPORT=y
CONFIG_ZMK_CUSTOM_SETTINGS_TEMP_SLOTS=4

# Split only: without this the central never learns the other half's level.
CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING=y
```

The peripheral half needs nothing: it already reports its level over the
Battery Service, which is what the central fetches.

## How often it updates

As often as ZMK samples the battery — `CONFIG_ZMK_BATTERY_REPORT_INTERVAL`,
60 seconds by default. The first reading therefore arrives up to a minute
after boot, and a level shown as 0 immediately after connecting means "not
sampled yet", not "flat".

## License

MIT, matching the ZMK modules it is built on.
