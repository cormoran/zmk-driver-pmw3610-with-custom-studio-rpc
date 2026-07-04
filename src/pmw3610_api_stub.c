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
