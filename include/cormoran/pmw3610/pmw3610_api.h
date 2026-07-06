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
#include <cormoran/pmw3610/pmw3610_settings_id.h>

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

/** @brief Get this device's stable per-device settings id.
 *
 * Resolved once at pmw3610_init() from the devicetree `settings-id` property
 * (used verbatim, truncated to PMW3610_SETTINGS_ID_MAX_LEN) or, when absent,
 * a 4-hex-digit hash of the devicetree node's full path (stable across
 * devicetree reordering of sibling nodes). Used to build this device's
 * per-device custom setting keys ("<param>@<id>") and reported in GetInfo.
 *
 * @param dev PMW3610 device.
 * @param buf Output buffer, NUL-terminated on success.
 * @param buf_len Capacity of buf, in bytes (should be at least
 *   PMW3610_SETTINGS_ID_BUF_SIZE to avoid truncation).
 * @return 0 on success, -EINVAL for a NULL dev/buf or zero buf_len.
 */
int pmw3610_get_device_id(const struct device *dev, char *buf, size_t buf_len);

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

/** @brief Byte format of the pixel data filled by a frame capture.
 *
 * Mirrors (but is intentionally independent of, per this header's no-nanopb
 * policy) the proto `PixelFormat` enum -- keep the two in sync (0 = PG7,
 * 1 = RAW8).
 */
enum pmw3610_pixel_format {
    /** Per-pixel Pixel_Grab byte: bit7 = PG_VALID, bits[6:0] = pixel value.
     * Produced by the 3-wire fallback path (disable-burst-read). */
    PMW3610_PIXEL_FORMAT_PG7 = 0,
    /** Full 8-bit pixel value, no per-pixel validity bit. Produced by the
     * 4-wire FRAME_GRAB burst read (register 0x36/0x12). */
    PMW3610_PIXEL_FORMAT_RAW8 = 1,
};

/** @brief Tunable knobs for pmw3610_capture_frame().
 *
 * The capture procedure follows the official PMW3610 datasheet (R2.4)
 * Pixel_Grab sequence; only the pixel count and the per-pixel wait budget
 * are tunable (the arm/advance register sequence itself is fixed by the
 * datasheet).
 */
struct pmw3610_frame_capture_params {
    /** Pixels to read. 0 means the driver default (484 = 22x22, the full
     * array per the datasheet's pixel address map). */
    uint16_t pixel_count;
    /** Max 10ms-wait retries per pixel while waiting for OBSERVATION1
     * (0x2D) bit2 (pixel-ready) before the capture aborts early, keeping
     * whatever was collected so far. 0 means the driver default (3);
     * clamped to 1..100. */
    uint16_t max_invalid_retries;
};

/** @brief Result of a pmw3610_capture_frame() call. */
struct pmw3610_frame_capture_result {
    /** Number of bytes actually collected (<= buf_len and <= requested
     * pixel_count; may be less than requested if the capture aborted
     * early). */
    uint16_t pixel_count;
    /** PRBS_TEST_CTL (0x47) bit0 as read after the pixel loop: the sensor
     * reports all 484 pixels were read successfully. */
    bool complete;
    /** Wall-clock duration of the capture procedure (arm sequence through
     * the completion check), in milliseconds. */
    uint32_t duration_ms;
    /** Byte format of the pixel data filled into buf (see
     * enum pmw3610_pixel_format). */
    enum pmw3610_pixel_format format;
};

/** @brief Capture a still frame (raw pixel array) from the sensor.
 *
 * Implements the official PMW3610 datasheet (R2.4) Pixel_Grab procedure:
 * SPI clock request on, page-1 magic enable (0xB4 = 0xD7), SPI clock
 * request off, test clock on (0x32 = 0x90), PIXEL_GRAB (0x35) = 0x01 to
 * arm, wait for PRBS_TEST_CTL (0x47) bit1, then per pixel: wait for
 * OBSERVATION1 (0x2D) bit2, read PIXEL_GRAB, write OBSERVATION1 = 0x01 to
 * advance. PRBS_TEST_CTL bit0 afterwards indicates a complete frame.
 *
 * Disables the motion IRQ and sets an internal "capture active" flag for
 * the duration of the call so pmw3610_report_data() does not interleave
 * SPI transactions with the capture loop. Frame grab disturbs normal
 * navigation (the datasheet requires a reset afterwards), so on return
 * (success or failure) this function always triggers the same
 * power-up-reset + reconfigure flow used by pmw3610_resume() (async init
 * from step 0) before releasing the capture lock, and re-enables the
 * motion IRQ once that flow schedules.
 *
 * Bounded to roughly 5 seconds of wall-clock time regardless of
 * pixel_count/max_invalid_retries, so a caller on the Studio RPC thread
 * cannot be blocked indefinitely by a misbehaving sensor.
 *
 * Implemented as pmw3610_frame_capture_begin() -> _read() -> _end(), so a
 * one-shot capture always resets afterwards (unchanged external behavior);
 * see those functions' docs for the persistent multi-frame alternative
 * used by streaming.
 *
 * @param dev PMW3610 device.
 * @param params Capture tuning knobs (NULL uses all defaults).
 * @param buf Output buffer for raw pixel bytes: format PMW3610_PIXEL_FORMAT_PG7
 *   (bit7 = PG_Valid, bits[6:0] = pixel value) on the 3-wire fallback path
 *   (disable-burst-read), or PMW3610_PIXEL_FORMAT_RAW8 (full 8-bit pixel,
 *   no validity bit) on the 4-wire burst path -- see result->format.
 * @param buf_len Capacity of buf, in bytes.
 * @param result Output capture result (collected count, completion flag,
 *   duration, format). Must not be NULL.
 * @return 0 on success (including a partial/early-aborted capture with
 *   result->pixel_count > 0), negative errno on failure (-ENODEV, -EINVAL
 *   for a zero buf_len or NULL result, -EBUSY if the device is not ready,
 *   or a SPI error).
 */
int pmw3610_capture_frame(const struct device *dev,
                          const struct pmw3610_frame_capture_params *params, uint8_t *buf,
                          uint16_t buf_len, struct pmw3610_frame_capture_result *result);

/** @brief Begin a persistent frame-capture session (no per-frame reset).
 *
 * Takes the device's internal lock just long enough to require `data->ready`
 * (else -EBUSY) and set an internal "capture active" flag, then disables the
 * motion IRQ and -- once for the whole session -- forces the sensor into run
 * mode (PERFORMANCE |= 0xF0) and confirms the mode switch landed (clear +
 * poll OBSERVATION1), which can take up to ~700ms if the sensor was in a
 * deep rest mode. This is the slow, one-time setup that
 * pmw3610_frame_capture_read() no longer has to repeat on every frame.
 *
 * Call pmw3610_frame_capture_read() any number of times afterwards to
 * capture successive frames without disturbing navigation between them,
 * then pmw3610_frame_capture_end() exactly once to resume navigation
 * (required by the datasheet) and release the session.
 *
 * @param dev PMW3610 device.
 * @param params Capture tuning knobs, currently unused by begin() itself
 *   (reserved for future per-session tuning); pass the same params intended
 *   for the session's _read() calls, or NULL.
 * @return 0 on success, -ENODEV for a NULL dev, -EBUSY if the device is not
 *   ready (or a session is already active), or a negative SPI errno.
 */
int pmw3610_frame_capture_begin(const struct device *dev,
                                const struct pmw3610_frame_capture_params *params);

/** @brief Capture exactly one frame within an active session, with no reset.
 *
 * Must be called between a successful pmw3610_frame_capture_begin() and the
 * matching pmw3610_frame_capture_end(). Takes `data->lock` for the duration
 * of the read so a concurrent ReadRegister/WriteRegister RPC cannot
 * interleave SPI transactions with the capture.
 *
 * On the 4-wire burst path (`!config->disable_burst_read`): (re-)arms
 * FRAME_GRAB (PERFORMANCE force-run + SPI clock request + test clock 0x10 +
 * FG_EN 0x36=0x80), waits for one fresh frame to latch, then issues a single
 * burst read of up to `buf_len` bytes from MOTION_BURST (0x12).
 * result->format is set to PMW3610_PIXEL_FORMAT_RAW8.
 *
 * On the 3-wire fallback path (`config->disable_burst_read`): runs the
 * unchanged, official datasheet Pixel_Grab procedure (same as the
 * standalone pmw3610_capture_frame(), steps 1-11) end to end -- this path
 * is not optimized by the persistent session (see DESIGN.md Phase G scope
 * notes); result->format is set to PMW3610_PIXEL_FORMAT_PG7.
 *
 * @param dev PMW3610 device.
 * @param params Capture tuning knobs (NULL uses all defaults); same
 *   semantics as pmw3610_capture_frame()'s params.
 * @param buf Output buffer for raw pixel bytes.
 * @param buf_len Capacity of buf, in bytes.
 * @param result Output capture result (collected count, completion flag,
 *   duration, format). Must not be NULL.
 * @return 0 on success (including a partial capture), negative errno on
 *   failure (-ENODEV, -EINVAL, or a SPI/timeout error). Does not return
 *   -EBUSY for "no session active" -- callers are responsible for only
 *   calling this between begin()/end().
 */
int pmw3610_frame_capture_read(const struct device *dev,
                               const struct pmw3610_frame_capture_params *params, uint8_t *buf,
                               uint16_t buf_len, struct pmw3610_frame_capture_result *result);

/** @brief End a persistent frame-capture session, resuming navigation.
 *
 * Takes `data->lock`, runs the same power-up-reset + async re-init flow
 * used by the one-shot pmw3610_capture_frame() (and by pmw3610_resume()'s
 * not-ready path) to resume normal navigation, clears the internal
 * "capture active"/"capture mode active" flags, and releases the lock. The
 * motion IRQ re-enables once the async re-init flow reaches its last step,
 * same as today.
 *
 * Safe to call even if pmw3610_frame_capture_begin() was never called or
 * already failed (e.g. to unconditionally clean up) -- it is a no-op beyond
 * the reset/re-init flow, which is itself idempotent.
 *
 * @param dev PMW3610 device.
 * @return 0 on success, -ENODEV for a NULL dev.
 */
int pmw3610_frame_capture_end(const struct device *dev);

#ifdef __cplusplus
}
#endif
