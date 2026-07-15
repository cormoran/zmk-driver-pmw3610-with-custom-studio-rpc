#pragma once

/**
 * @file pmw3610_settings_apply.h
 *
 * @brief Internal glue between the PMW3610 driver and the optional custom
 * settings integration (src/settings/pmw3610_settings.c,
 * CONFIG_ZMK_PMW3610_CUSTOM_SETTINGS).
 *
 * This header (and pmw3610_settings_apply_to_device()) only needs to exist
 * so the driver's async init configure step can pull in the
 * effective (persisted-or-default) setting values *before* they are pushed
 * to the sensor over SPI, closing a boot-ordering race that would otherwise
 * exist between:
 *   - POST_KERNEL: pmw3610_init() seeds runtime config from Kconfig/DT and
 *     schedules the async init work (fires ~260ms later on the system
 *     workqueue).
 *   - APPLICATION: custom_settings_init() (from zmk-feature-custom-settings)
 *     resets every setting's value to its compiled-in default.
 *   - main(): settings_load() loads persisted values from flash, which is
 *     the point at which "effective value" becomes meaningful.
 *
 * The async init work is timer-delayed by ~260ms, so in the common case it
 * runs after main()'s settings_load() has returned and this apply sees the
 * persisted values. That timing is NOT guaranteed, though: settings_load()
 * runs on the main thread while async init runs on the (higher-priority,
 * preempting) system workqueue, and a large NVS -- e.g. BLE bonds on a real
 * keyboard -- can make settings_load() outlast the ~260ms delay. When it
 * does, this call reads compiled-in defaults, and since the plain
 * zmk-feature-custom-settings load path raises no change event, nothing would
 * ever correct it. The deterministic fix lives in
 * src/settings/pmw3610_settings.c: a zmk_custom_settings_initialized listener
 * re-applies every device once that event fires (raised once per boot,
 * strictly after settings_load() has populated every effective value). This
 * async-init call remains as the fast path for the common
 * (settings-already-loaded) case; the event listener is the backstop that
 * closes the race.
 *
 * When CONFIG_ZMK_PMW3610_CUSTOM_SETTINGS is disabled, this call is
 * compiled out entirely (see the IS_ENABLED guard at the call site in
 * pmw3610.c) so the driver has no link-time dependency on the settings
 * module.
 */

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Overlay all "cormoran__pmw3610" custom setting effective values
 * onto the given device's runtime config, then push them to the sensor if
 * it is already marked ready (it is not, when called from
 * pmw3610_async_init_configure(), so this only updates the in-memory
 * runtime struct that the caller is about to apply to hardware itself).
 *
 * Safe to call even with zero settings registered/matching (no-op).
 */
void pmw3610_settings_apply_to_device(const struct device *dev);

#ifdef __cplusplus
}
#endif
