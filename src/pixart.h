#pragma once

/**
 * @file pixart.h
 *
 * @brief Common header file for all optical motion sensor by PIXART
 */

#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Runtime-configurable sensor parameters.
 *
 * Seeded from Kconfig/DT defaults at driver init, optionally overridden by
 * the custom settings module (CONFIG_ZMK_PMW3610_CUSTOM_SETTINGS) before or
 * after the sensor finishes async init, and mutable afterwards via the
 * pmw3610_set_* public API (e.g. from the Studio RPC handler).
 */
struct pixart_runtime_config {
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

/* device data structure */
struct pixart_data {
    const struct device *dev;
    bool sw_smart_flag; // for pmw3610 smart algorithm

    struct gpio_callback irq_gpio_cb; // motion pin irq callback
    struct k_work trigger_work;       // realtrigger job

    struct k_work_delayable init_work; // the work structure for delayable init steps
    int async_init_step;
    int async_init_retry_count; // retry count for async init

    bool ready; // whether init is finished successfully
    bool last_read_burst;
    int err; // error code during async init

    /* Runtime-configurable parameters (see struct pixart_runtime_config). */
    struct pixart_runtime_config runtime;

    /* Serializes composite register sequences (e.g. multi-write CPI, clock
     * on/off wrapped writes, burst report reads) against concurrent access
     * from the Studio RPC thread while the trigger work runs on the system
     * workqueue. SPI bus arbitration alone is not enough since these
     * sequences are multiple SPI transactions that must not interleave. */
    struct k_mutex lock;

    /* Report accumulation state, moved here (was `static` in pmw3610.c) to
     * fix cross-talk between multiple sensor instances. */
    int64_t dx;
    int64_t dy;
    int64_t last_smp_time;
    int64_t last_rpt_time;
};

// device config data structure
struct pixart_config {
    struct spi_dt_spec spi;
    struct gpio_dt_spec irq_gpio;
    uint16_t cpi;
    uint8_t evt_type;
    uint8_t x_input_code;
    uint8_t y_input_code;
    bool force_awake;
    bool disable_burst_read;
};

#ifdef __cplusplus
}
#endif

/**
 * @}
 */
