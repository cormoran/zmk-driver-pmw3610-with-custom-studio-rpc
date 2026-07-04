#pragma once

/**
 * @file pmw3610_api.h
 *
 * @brief Public API exposed by the PMW3610 driver for use by the custom
 * Studio RPC handler (and, in later phases, settings integration).
 *
 * This header intentionally does not depend on any nanopb/proto generated
 * types so it stays usable outside the studio RPC subsystem.
 *
 * When CONFIG_PMW3610 is disabled (e.g. native_sim with no devicetree node),
 * these functions are still declared so callers do not need to guard every
 * call site; the implementation in pmw3610.c is only compiled when
 * CONFIG_PMW3610 is enabled, and callers should be prepared for
 * pmw3610_device_count() == 0.
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Number of PMW3610 device instances found in the devicetree. */
size_t pmw3610_device_count(void);

/** @brief Get the PMW3610 device instance at the given index.
 *
 * @param index Index in range [0, pmw3610_device_count()).
 * @return Device pointer, or NULL if index is out of range.
 */
const struct device *pmw3610_get_device(size_t index);

/** @brief Whether the given PMW3610 device has finished async init. */
bool pmw3610_is_ready(const struct device *dev);

/** @brief Last error code observed during async init (0 = no error). */
int pmw3610_get_init_error(const struct device *dev);

/** @brief Read a single register over SPI.
 *
 * @param dev PMW3610 device.
 * @param addr Register address.
 * @param value Output value.
 * @return 0 on success, negative errno on failure.
 */
int pmw3610_read_register(const struct device *dev, uint8_t addr, uint8_t *value);

#ifdef __cplusplus
}
#endif
