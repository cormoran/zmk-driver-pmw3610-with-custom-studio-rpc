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
 * Since main() runs after all SYS_INIT levels but the async init work is
 * timer-delayed by ~260ms (well after main() has returned in practice),
 * applying settings once more from a SYS_INIT(APPLICATION) hook would
 * almost always be safe -- but "almost always" is exactly the kind of
 * boot-order assumption this project wants to avoid baking in silently.
 * Instead, the driver calls this function directly from
 * pmw3610_async_init_configure(), i.e. strictly after main()'s
 * settings_load() has already returned (both run on cooperating threads,
 * and the async init work is scheduled well after main() starts), so there
 * is no race: by the time this runs, settings_load() is guaranteed done.
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
