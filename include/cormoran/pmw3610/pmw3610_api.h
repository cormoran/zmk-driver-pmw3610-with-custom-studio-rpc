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

/** @brief Write a single register over SPI (SPI clock on/off wrapped).
 *
 * Intended as a debug/tuning facility (e.g. exposed by the Studio RPC
 * WriteRegister handler). Does not validate the address/value against known
 * safe ranges -- callers may brick sensor behavior until the next power-up
 * reset / reconfigure.
 *
 * @param dev PMW3610 device.
 * @param addr Register address.
 * @param value Value to write.
 * @return 0 on success, negative errno on failure.
 */
int pmw3610_write_register(const struct device *dev, uint8_t addr, uint8_t value);

/** @brief Sensor diagnostics snapshot (see PMW3610 datasheet registers). */
struct pmw3610_diagnostics {
    uint8_t squal;    /**< Surface quality, reg 0x06. */
    uint16_t shutter; /**< Shutter value, reg 0x07 (hi) / 0x08 (lo). */
    uint8_t pix_max;  /**< Maximum pixel value, reg 0x09. */
    uint8_t pix_avg;  /**< Average pixel value, reg 0x0A. */
    uint8_t pix_min;  /**< Minimum pixel value, reg 0x0B. */
};

/** @brief Read SQUAL/shutter/pixel diagnostics registers.
 *
 * @param dev PMW3610 device.
 * @param out Output diagnostics.
 * @return 0 on success, negative errno on failure (e.g. -ENODEV, -EBUSY if
 * not ready).
 */
int pmw3610_read_diagnostics(const struct device *dev, struct pmw3610_diagnostics *out);

/** @brief Snapshot of the current runtime configuration (see
 * struct pixart_runtime_config in pixart.h for field semantics). Duplicated
 * here so callers (e.g. the Studio RPC handler) don't need to include the
 * internal pixart.h.
 */
struct pmw3610_runtime_config {
    uint32_t cpi;
    bool swap_xy;
    bool invert_x;
    bool invert_y;
    bool force_awake;
    bool smart_algorithm;
    uint32_t run_downshift_ms;
    uint32_t rest1_downshift_ms;
    uint32_t rest2_downshift_ms;
    uint32_t rest1_sample_ms;
    uint32_t rest2_sample_ms;
    uint32_t rest3_sample_ms;
    uint32_t report_interval_min_ms;
};

/** @brief Read the current runtime configuration snapshot.
 *
 * @param dev PMW3610 device.
 * @param out Output config.
 * @return 0 on success, negative errno on failure (e.g. -ENODEV).
 */
int pmw3610_get_runtime_config(const struct device *dev, struct pmw3610_runtime_config *out);

/*
 * Runtime setters.
 *
 * Each setter: validates the new value, stores it in the per-device runtime
 * config (always, even if the device is not yet ready -- this lets the
 * custom settings module seed values before async init completes, since
 * pmw3610_async_init_configure() reads the runtime config), and, if the
 * device has finished async init, immediately pushes the new value to the
 * sensor over SPI.
 *
 * Return 0 on success, -EINVAL for out-of-range values, -ENODEV for an
 * unknown device, or a negative errno from the SPI write if the immediate
 * push fails (the runtime value is still updated in that case).
 */
int pmw3610_set_cpi_runtime(const struct device *dev, uint32_t cpi);
int pmw3610_set_run_downshift_ms(const struct device *dev, uint32_t value);
int pmw3610_set_rest1_downshift_ms(const struct device *dev, uint32_t value);
int pmw3610_set_rest2_downshift_ms(const struct device *dev, uint32_t value);
int pmw3610_set_rest1_sample_ms(const struct device *dev, uint32_t value);
int pmw3610_set_rest2_sample_ms(const struct device *dev, uint32_t value);
int pmw3610_set_rest3_sample_ms(const struct device *dev, uint32_t value);
int pmw3610_set_axis_flags(const struct device *dev, bool swap_xy, bool invert_x, bool invert_y);
int pmw3610_set_force_awake(const struct device *dev, bool enabled);
int pmw3610_set_smart_algorithm(const struct device *dev, bool enabled);
int pmw3610_set_report_interval_min(const struct device *dev, uint32_t value_ms);

/** @brief Tunable knobs for pmw3610_capture_frame().
 *
 * The public 8-page PMW3610 datasheet documents the PIXEL_GRAB (0x35) /
 * FRAME_GRAB (0x36) registers but not the exact procedure required to read
 * a still frame. This struct exposes the procedure's variable points so it
 * can be tuned (e.g. via the Studio RPC CaptureFrame request) without
 * reflashing, pending hardware validation.
 */
struct pmw3610_frame_capture_params {
    /** Pixels to read. 0 means the driver default (484 = 22x22). */
    uint16_t pixel_count;
    /** Consecutive invalid-read (PG_VALID bit7 clear) retries allowed per
     * pixel before the capture aborts early, keeping whatever was
     * collected so far. 0 means the driver default (300). */
    uint16_t max_invalid_retries;
    /** Write FRAME_GRAB (0x36) = frame_grab_value before reading pixels. */
    bool write_frame_grab;
    /** Value written to FRAME_GRAB when write_frame_grab is set. */
    uint8_t frame_grab_value;
    /** Write PIXEL_GRAB (0x35) = 0x00 before reading (resets the sensor's
     * internal pixel pointer). Default true. */
    bool write_pixel_grab_reset;
};

/** @brief Capture a still frame (raw pixel array) from the sensor.
 *
 * Disables the motion IRQ and sets an internal "capture active" flag for
 * the duration of the call so pmw3610_report_data() does not interleave
 * SPI transactions with the capture loop. Frame grab disturbs normal
 * navigation, so on return (success or failure) this function always
 * triggers the same power-up-reset + reconfigure flow used by
 * pmw3610_resume() (async init from step 0) before releasing the capture
 * lock, and re-enables the motion IRQ once that flow schedules.
 *
 * Bounded to roughly 2 seconds of wall-clock time regardless of
 * pixel_count/max_invalid_retries, so a caller on the Studio RPC thread
 * cannot be blocked indefinitely by a misbehaving sensor.
 *
 * @param dev PMW3610 device.
 * @param params Capture tuning knobs (NULL uses all defaults).
 * @param buf Output buffer for raw pixel bytes (bit7 = PG_VALID, bits[6:0]
 *   = pixel value; the caller is responsible for masking/interpreting).
 * @param buf_len Capacity of buf, in bytes.
 * @param out_count Number of bytes actually collected (<= buf_len and <=
 *   requested pixel_count; may be less than requested if the capture
 *   aborted early).
 * @return 0 on success (including a partial/early-aborted capture with
 *   out_count > 0), negative errno on failure (-ENODEV, -EINVAL for a zero
 *   buf_len, -EBUSY if the device is not ready, or a SPI error).
 */
int pmw3610_capture_frame(const struct device *dev,
                          const struct pmw3610_frame_capture_params *params, uint8_t *buf,
                          uint16_t buf_len, uint16_t *out_count);

#ifdef __cplusplus
}
#endif
