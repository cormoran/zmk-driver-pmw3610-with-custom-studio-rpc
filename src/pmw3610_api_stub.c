/*
 * Fallback implementation of the PMW3610 public API used when the driver
 * core (CONFIG_PMW3610) is not compiled in, e.g. native_sim which has no
 * devicetree node for the sensor (DT_HAS_CORMORAN_PMW3610_ENABLED=n).
 *
 * This keeps the custom Studio RPC handler (CONFIG_ZMK_PMW3610_STUDIO_RPC)
 * buildable and functional (reporting zero devices) without depending on
 * CONFIG_PMW3610.
 */

#include <cormoran/pmw3610/pmw3610_api.h>
#include <zephyr/sys/util.h>
#include <errno.h>

size_t pmw3610_device_count(void) { return 0; }

const struct device *pmw3610_get_device(size_t index) {
    ARG_UNUSED(index);
    return NULL;
}

bool pmw3610_is_ready(const struct device *dev) {
    ARG_UNUSED(dev);
    return false;
}

int pmw3610_get_init_error(const struct device *dev) {
    ARG_UNUSED(dev);
    return -ENODEV;
}

int pmw3610_read_register(const struct device *dev, uint8_t addr, uint8_t *value) {
    ARG_UNUSED(dev);
    ARG_UNUSED(addr);
    ARG_UNUSED(value);
    return -ENODEV;
}

int pmw3610_write_register(const struct device *dev, uint8_t addr, uint8_t value) {
    ARG_UNUSED(dev);
    ARG_UNUSED(addr);
    ARG_UNUSED(value);
    return -ENODEV;
}

int pmw3610_read_diagnostics(const struct device *dev, struct pmw3610_diagnostics *out) {
    ARG_UNUSED(dev);
    ARG_UNUSED(out);
    return -ENODEV;
}

int pmw3610_get_runtime_config(const struct device *dev, struct pmw3610_runtime_config *out) {
    ARG_UNUSED(dev);
    ARG_UNUSED(out);
    return -ENODEV;
}

int pmw3610_set_cpi_runtime(const struct device *dev, uint32_t cpi) {
    ARG_UNUSED(dev);
    ARG_UNUSED(cpi);
    return -ENODEV;
}

int pmw3610_set_run_downshift_ms(const struct device *dev, uint32_t value) {
    ARG_UNUSED(dev);
    ARG_UNUSED(value);
    return -ENODEV;
}

int pmw3610_set_rest1_downshift_ms(const struct device *dev, uint32_t value) {
    ARG_UNUSED(dev);
    ARG_UNUSED(value);
    return -ENODEV;
}

int pmw3610_set_rest2_downshift_ms(const struct device *dev, uint32_t value) {
    ARG_UNUSED(dev);
    ARG_UNUSED(value);
    return -ENODEV;
}

int pmw3610_set_rest1_sample_ms(const struct device *dev, uint32_t value) {
    ARG_UNUSED(dev);
    ARG_UNUSED(value);
    return -ENODEV;
}

int pmw3610_set_rest2_sample_ms(const struct device *dev, uint32_t value) {
    ARG_UNUSED(dev);
    ARG_UNUSED(value);
    return -ENODEV;
}

int pmw3610_set_rest3_sample_ms(const struct device *dev, uint32_t value) {
    ARG_UNUSED(dev);
    ARG_UNUSED(value);
    return -ENODEV;
}

int pmw3610_set_axis_flags(const struct device *dev, bool swap_xy, bool invert_x, bool invert_y) {
    ARG_UNUSED(dev);
    ARG_UNUSED(swap_xy);
    ARG_UNUSED(invert_x);
    ARG_UNUSED(invert_y);
    return -ENODEV;
}

int pmw3610_set_force_awake(const struct device *dev, bool enabled) {
    ARG_UNUSED(dev);
    ARG_UNUSED(enabled);
    return -ENODEV;
}

int pmw3610_set_smart_algorithm(const struct device *dev, bool enabled) {
    ARG_UNUSED(dev);
    ARG_UNUSED(enabled);
    return -ENODEV;
}

int pmw3610_set_report_interval_min(const struct device *dev, uint32_t value_ms) {
    ARG_UNUSED(dev);
    ARG_UNUSED(value_ms);
    return -ENODEV;
}

int pmw3610_capture_frame(const struct device *dev,
                          const struct pmw3610_frame_capture_params *params, uint8_t *buf,
                          uint16_t buf_len, uint16_t *out_count) {
    ARG_UNUSED(dev);
    ARG_UNUSED(params);
    ARG_UNUSED(buf);
    ARG_UNUSED(buf_len);
    if (out_count) {
        *out_count = 0;
    }
    return -ENODEV;
}
