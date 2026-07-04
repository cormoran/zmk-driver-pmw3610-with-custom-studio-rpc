/*
 * Copyright (c) 2022 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT cormoran_pmw3610

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/input/input.h>
#include <zephyr/pm/device.h>
#include <zmk/keymap.h>
#include <zmk/activity.h>
#include <zmk/events/activity_state_changed.h>
#include "pmw3610.h"
#include <cormoran/pmw3610/pmw3610_api.h>
#include <cormoran/pmw3610/pmw3610_settings_apply.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(pmw3610, CONFIG_PMW3610_LOG_LEVEL);

//////// Frame capture defaults (see pmw3610_capture_frame) //////////
// The public datasheet does not document the pixel-grab procedure; these
// match the common PixArt pixel-grab convention and are tunable per-request
// (struct pmw3610_frame_capture_params) pending hardware validation.
#define PMW3610_FRAME_CAPTURE_DEFAULT_PIXEL_COUNT 484 // 22 x 22
#define PMW3610_FRAME_CAPTURE_DEFAULT_MAX_INVALID_RETRIES 300
#define PMW3610_FRAME_CAPTURE_DEFAULT_FRAME_GRAB_VALUE 0x00
#define PMW3610_PIXEL_GRAB_VALID_BIT BIT(7)
// Busy-wait between invalid-pixel retries (datasheet-adjacent convention;
// short enough to not matter for the 2s timeout budget below).
#define PMW3610_FRAME_CAPTURE_RETRY_DELAY_US 20
// Unconditional wall-clock bound on the whole read loop so a caller on the
// Studio RPC thread can never be blocked indefinitely by a misbehaving
// sensor (e.g. PG_VALID never asserting). Independent of pixel_count /
// max_invalid_retries -- whichever bound is hit first wins.
#define PMW3610_FRAME_CAPTURE_TIMEOUT_MS 2000

//////// Sensor initialization steps definition //////////
// init is done in non-blocking manner (i.e., async), a //
// delayable work is defined for this purpose           //
enum pmw3610_init_step {
    ASYNC_INIT_STEP_POWER_UP,  // reset cs line and assert power-up reset
    ASYNC_INIT_STEP_CLEAR_OB1, // clear observation1 register for self-test check
    ASYNC_INIT_STEP_CHECK_OB1, // check the value of observation1 register after self-test check
    ASYNC_INIT_STEP_CONFIGURE, // set other registes like cpi and donwshift time (run, rest1, rest2)
                               // and clear motion registers

    ASYNC_INIT_STEP_COUNT // end flag
};

/* Timings (in ms) needed in between steps to allow each step finishes succussfully. */
// - Since MCU is not involved in the sensor init process, i is allowed to do other tasks.
//   Thus, k_sleep or delayed schedule can be used.
static const int32_t async_init_delay[ASYNC_INIT_STEP_COUNT] = {
    [ASYNC_INIT_STEP_POWER_UP] = 10 + CONFIG_PMW3610_INIT_POWER_UP_EXTRA_DELAY_MS, // >10ms needed
    [ASYNC_INIT_STEP_CLEAR_OB1] =
        200,                          // 150 us required, test shows too short,
                                      // also power-up reset is added in this step, thus using 50 ms
    [ASYNC_INIT_STEP_CHECK_OB1] = 50, // 10 ms required in spec,
                                      // test shows too short,
                                      // especially when integrated with display,
                                      // > 50ms is needed
    [ASYNC_INIT_STEP_CONFIGURE] = 0,
};

static int pmw3610_async_init_power_up(const struct device *dev);
static int pmw3610_async_init_clear_ob1(const struct device *dev);
static int pmw3610_async_init_check_ob1(const struct device *dev);
static int pmw3610_async_init_configure(const struct device *dev);

static int (*const async_init_fn[ASYNC_INIT_STEP_COUNT])(const struct device *dev) = {
    [ASYNC_INIT_STEP_POWER_UP] = pmw3610_async_init_power_up,
    [ASYNC_INIT_STEP_CLEAR_OB1] = pmw3610_async_init_clear_ob1,
    [ASYNC_INIT_STEP_CHECK_OB1] = pmw3610_async_init_check_ob1,
    [ASYNC_INIT_STEP_CONFIGURE] = pmw3610_async_init_configure,
};

//////// Function definitions //////////

static int pmw3610_read(const struct device *dev, uint8_t addr, uint8_t *value, uint8_t len) {
    const struct pixart_config *cfg = dev->config;
    const struct spi_buf tx_buf = {.buf = &addr, .len = sizeof(addr)};
    const struct spi_buf_set tx = {.buffers = &tx_buf, .count = 1};
    struct spi_buf rx_buf[] = {
        {
            .buf = NULL,
            .len = sizeof(addr),
        },
        {
            .buf = value,
            .len = len,
        },
    };
    const struct spi_buf_set rx = {.buffers = rx_buf, .count = ARRAY_SIZE(rx_buf)};
    return spi_transceive_dt(&cfg->spi, &tx, &rx);
}

static int pmw3610_read_reg(const struct device *dev, uint8_t addr, uint8_t *value) {
    return pmw3610_read(dev, addr, value, 1);
}

static int pmw3610_write_reg(const struct device *dev, uint8_t addr, uint8_t value) {
    const struct pixart_config *cfg = dev->config;
    uint8_t write_buf[] = {addr | SPI_WRITE_BIT, value};
    const struct spi_buf tx_buf = {
        .buf = write_buf,
        .len = sizeof(write_buf),
    };
    const struct spi_buf_set tx = {
        .buffers = &tx_buf,
        .count = 1,
    };
    return spi_write_dt(&cfg->spi, &tx);
}

static int pmw3610_write(const struct device *dev, uint8_t reg, uint8_t val) {
    pmw3610_write_reg(dev, PMW3610_REG_SPI_CLK_ON_REQ, PMW3610_SPI_CLOCK_CMD_ENABLE);
    k_sleep(K_USEC(T_CLOCK_ON_DELAY_US));

    int err = pmw3610_write_reg(dev, reg, val);
    if (unlikely(err != 0)) {
        return err;
    }

    pmw3610_write_reg(dev, PMW3610_REG_SPI_CLK_ON_REQ, PMW3610_SPI_CLOCK_CMD_DISABLE);
    return 0;
}

/* Caller must hold data->lock (composite multi-write sequence). */
static int pmw3610_set_cpi_locked(const struct device *dev, uint32_t cpi) {
    /* Set resolution with CPI step of 200 cpi
     * 0x1: 200 cpi (minimum cpi)
     * 0x2: 400 cpi
     * 0x3: 600 cpi
     * :
     */

    if ((cpi > PMW3610_MAX_CPI) || (cpi < PMW3610_MIN_CPI)) {
        LOG_ERR("CPI value %u out of range", cpi);
        return -EINVAL;
    }

    // Convert CPI to register value
    uint8_t value = (cpi / 200);
    LOG_INF("Setting CPI to %u (reg value 0x%x)", cpi, value);

    /* set the cpi */
    uint8_t addr[] = {0x7F, PMW3610_REG_RES_STEP, 0x7F};
    uint8_t data[] = {0xFF, value, 0x00};

    pmw3610_write_reg(dev, PMW3610_REG_SPI_CLK_ON_REQ, PMW3610_SPI_CLOCK_CMD_ENABLE);
    k_sleep(K_USEC(T_CLOCK_ON_DELAY_US));

    /* Write data */
    int err;
    for (size_t i = 0; i < sizeof(data); i++) {
        err = pmw3610_write_reg(dev, addr[i], data[i]);
        if (err) {
            LOG_ERR("Burst write failed on SPI write (data)");
            break;
        }
    }
    pmw3610_write_reg(dev, PMW3610_REG_SPI_CLK_ON_REQ, PMW3610_SPI_CLOCK_CMD_DISABLE);

    if (err) {
        LOG_ERR("Failed to set CPI");
        return err;
    }

    return 0;
}

/* Set sampling rate in each mode (in ms) */
static int pmw3610_set_sample_time(const struct device *dev, uint8_t reg_addr,
                                   uint32_t sample_time) {
    uint32_t maxtime = 2550;
    uint32_t mintime = 10;
    if ((sample_time > maxtime) || (sample_time < mintime)) {
        LOG_WRN("Sample time %u out of range [%u, %u]", sample_time, mintime, maxtime);
        return -EINVAL;
    }

    uint8_t value = sample_time / mintime;
    LOG_INF("Set sample time to %u ms (reg value: 0x%x)", sample_time, value);

    /* The sample time is (reg_value * mintime ) ms. 0x00 is rounded to 0x1 */
    int err = pmw3610_write(dev, reg_addr, value);
    if (err) {
        LOG_ERR("Failed to change sample time");
    }

    return err;
}

/* Set downshift time in ms.
 *
 * NOTE: The unit of run-mode downshift is related to pos mode rate, which is
 * hard coded to be 4 ms (configured in pmw3610_async_init_configure). The
 * unit of rest1/rest2 downshift depends on the *current* rest1/rest2 sample
 * time respectively, so the caller passes the currently configured sample
 * time in `sample_time_ms` (unused for PMW3610_REG_RUN_DOWNSHIFT). */
static int pmw3610_set_downshift_time(const struct device *dev, uint8_t reg_addr, uint32_t time,
                                      uint32_t sample_time_ms) {
    uint32_t maxtime;
    uint32_t mintime;

    switch (reg_addr) {
    case PMW3610_REG_RUN_DOWNSHIFT:
        /*
         * Run downshift time = PMW3610_REG_RUN_DOWNSHIFT
         *                      * 8 * pos-rate (fixed to 4ms)
         */
        maxtime = 8160; // 32 * 255;
        mintime = 32;   // hard-coded in pmw3610_async_init_configure
        break;

    case PMW3610_REG_REST1_DOWNSHIFT:
        /*
         * Rest1 downshift time = PMW3610_REG_RUN_DOWNSHIFT
         *                        * 16 * Rest1_sample_period (default 40 ms)
         */
        maxtime = 255 * 16 * sample_time_ms;
        mintime = 16 * sample_time_ms;
        break;

    case PMW3610_REG_REST2_DOWNSHIFT:
        /*
         * Rest2 downshift time = PMW3610_REG_REST2_DOWNSHIFT
         *                        * 128 * Rest2 rate (default 100 ms)
         */
        maxtime = 255 * 128 * sample_time_ms;
        mintime = 128 * sample_time_ms;
        break;

    default:
        LOG_ERR("Not supported");
        return -ENOTSUP;
    }

    if ((time > maxtime) || (time < mintime)) {
        LOG_WRN("Downshift time %u out of range (%u - %u)", time, mintime, maxtime);
        return -EINVAL;
    }

    __ASSERT_NO_MSG((mintime > 0) && (maxtime / mintime <= UINT8_MAX));

    /* Convert time to register value */
    uint8_t value = time / mintime;

    LOG_INF("Set downshift time to %u ms (reg value 0x%x)", time, value);

    int err = pmw3610_write(dev, reg_addr, value);
    if (err) {
        LOG_ERR("Failed to change downshift time");
    }

    return err;
}

/* Set the performance (force-awake) register.
 *
 * `force_awake_runtime` reflects the current runtime force-awake flag (from
 * pixart_data.runtime.force_awake, which may differ from the DT
 * force-awake property once changed via settings/RPC). When disabled, the
 * performance register is left untouched (matches previous behavior where
 * config->force_awake gated the whole function). */
static int pmw3610_set_performance(const struct device *dev, bool force_awake_runtime,
                                   bool enabled) {
    int err = 0;

    if (force_awake_runtime) {
        uint8_t value;
        err = pmw3610_read_reg(dev, PMW3610_REG_PERFORMANCE, &value);
        if (err) {
            LOG_ERR("Can't read ref-performance %d", err);
            return err;
        }
        LOG_INF("Get performance register (reg value 0x%x)", value);

        uint8_t perf = value & 0x0F; // reset bit[3..0] to 0x0 (normal operation)
        if (enabled) {
            perf |= 0xF0; // set bit[3..0] to 0xF (force awake)
        }
        if (perf != value) {
            err = pmw3610_write(dev, PMW3610_REG_PERFORMANCE, perf);
            if (err) {
                LOG_ERR("Can't write performance register %d", err);
                return err;
            }
            LOG_INF("Set performance register (reg value 0x%x)", perf);
        }
    }

    return err;
}

static int pmw3610_set_interrupt(const struct device *dev, const bool en) {
    const struct pixart_config *config = dev->config;
    int ret = gpio_pin_interrupt_configure_dt(&config->irq_gpio,
                                              en ? GPIO_INT_LEVEL_ACTIVE : GPIO_INT_DISABLE);
    if (ret < 0) {
        LOG_ERR("can't set interrupt");
    }
    return ret;
}

static int pmw3610_async_init_power_up(const struct device *dev) {
    int ret = pmw3610_write_reg(dev, PMW3610_REG_POWER_UP_RESET, PMW3610_POWERUP_CMD_RESET);
    if (ret < 0) {
        return ret;
    }
    return 0;
}

static int pmw3610_async_init_clear_ob1(const struct device *dev) {
    return pmw3610_write(dev, PMW3610_REG_OBSERVATION, 0x00);
}

static int pmw3610_async_init_check_ob1(const struct device *dev) {
    uint8_t value;
    int err = pmw3610_read_reg(dev, PMW3610_REG_OBSERVATION, &value);
    if (err) {
        LOG_ERR("Can't do self-test");
        return err;
    }

    if ((value & 0x0F) != 0x0F) {
        LOG_ERR("Failed self-test (0x%x)", value);
        return -EINVAL;
    }

    uint8_t product_id = 0x01;
    err = pmw3610_read_reg(dev, PMW3610_REG_PRODUCT_ID, &product_id);
    if (err) {
        LOG_ERR("Cannot obtain product id");
        return err;
    }

    if (product_id != PMW3610_PRODUCT_ID) {
        LOG_ERR("Incorrect product id 0x%x (expecting 0x%x)!", product_id, PMW3610_PRODUCT_ID);
        return -EIO;
    }

    return 0;
}

static int pmw3610_async_init_configure(const struct device *dev) {
    int err = 0;
    struct pixart_data *data = dev->data;

    // clear motion registers first (required in datasheet)
    for (uint8_t reg = 0x02; (reg <= 0x05) && !err; reg++) {
        uint8_t buf[1];
        err = pmw3610_read_reg(dev, reg, buf);
    }

    // Overlay persisted/effective custom-setting values onto the runtime
    // config before it is applied to hardware below. This runs strictly
    // after main()'s settings_load() has returned (see
    // pmw3610_settings_apply.h for why there is no boot-ordering race),
    // and before `data->ready` is set, so the immediate-push branches in
    // the pmw3610_set_* setters are no-ops here -- only the in-memory
    // struct is updated, then applied once below.
    if (IS_ENABLED(CONFIG_ZMK_PMW3610_CUSTOM_SETTINGS)) {
        pmw3610_settings_apply_to_device(dev);
    }

    k_mutex_lock(&data->lock, K_FOREVER);
    struct pixart_runtime_config rt = data->runtime;
    k_mutex_unlock(&data->lock);

    if (!err) {
        err = pmw3610_set_performance(dev, rt.force_awake, true);
    }

    if (!err) {
        k_mutex_lock(&data->lock, K_FOREVER);
        err = pmw3610_set_cpi_locked(dev, rt.cpi);
        k_mutex_unlock(&data->lock);
    }

    if (!err) {
        err = pmw3610_set_downshift_time(dev, PMW3610_REG_RUN_DOWNSHIFT, rt.run_downshift_ms, 0);
    }

    if (!err) {
        err = pmw3610_set_downshift_time(dev, PMW3610_REG_REST1_DOWNSHIFT, rt.rest1_downshift_ms,
                                         rt.rest1_sample_ms);
    }

    if (!err) {
        err = pmw3610_set_downshift_time(dev, PMW3610_REG_REST2_DOWNSHIFT, rt.rest2_downshift_ms,
                                         rt.rest2_sample_ms);
    }

    if (!err) {
        err = pmw3610_set_sample_time(dev, PMW3610_REG_REST1_RATE, rt.rest1_sample_ms);
    }

    if (!err) {
        err = pmw3610_set_sample_time(dev, PMW3610_REG_REST2_RATE, rt.rest2_sample_ms);
    }

    if (!err) {
        err = pmw3610_set_sample_time(dev, PMW3610_REG_REST3_RATE, rt.rest3_sample_ms);
    }

    if (err) {
        LOG_ERR("Config the sensor failed");
        return err;
    }

    return 0;
}

static void pmw3610_async_init(struct k_work *work) {
    struct k_work_delayable *work2 = (struct k_work_delayable *)work;
    struct pixart_data *data = CONTAINER_OF(work2, struct pixart_data, init_work);
    const struct device *dev = data->dev;
    int current_step = data->async_init_step;
    LOG_INF("PMW3610 async init step %d", current_step);

    if (current_step >= ASYNC_INIT_STEP_COUNT) {
        LOG_ERR("Invalid async init step %d", current_step);
        return;
    }

    data->err = async_init_fn[current_step](dev);
    if (data->err) {
        if (data->async_init_retry_count < 10) {
            data->async_init_retry_count++;
            data->async_init_step = 0;
            LOG_WRN("PMW3610 async init step %d failed, retrying from "
                    "beginning(%d/10)",
                    current_step, data->async_init_retry_count);
            k_work_schedule(&data->init_work, K_MSEC(async_init_delay[current_step]));
            return;
        }
        LOG_ERR("PMW3610 initialization failed in step %d", current_step);
    } else {
        int next_step = ++data->async_init_step;

        if (next_step == ASYNC_INIT_STEP_COUNT) {
            data->ready = true; // sensor is ready to work
            LOG_INF("PMW3610 initialized");
            pmw3610_set_interrupt(dev, true);
        } else if (next_step < ASYNC_INIT_STEP_COUNT) {
            k_work_schedule(&data->init_work, K_MSEC(async_init_delay[next_step]));
        } // else : can happen by suspend?
    }
}

static int pmw3610_report_data(const struct device *dev) {
    struct pixart_data *data = dev->data;
    const struct pixart_config *config = dev->config;
    uint8_t buf[PMW3610_BURST_SIZE];

    if (unlikely(!data->ready)) {
        LOG_WRN("Device is not initialized yet");
        return -EBUSY;
    }

    k_mutex_lock(&data->lock, K_FOREVER);
    if (unlikely(data->capture_active)) {
        /* A frame capture is in progress on another thread (e.g. Studio
         * RPC). It already disables the motion IRQ, but the trigger work
         * item may have been queued just before that happened -- skip
         * touching the sensor here rather than interleaving SPI
         * transactions with the capture loop. */
        k_mutex_unlock(&data->lock);
        return 0;
    }
    struct pixart_runtime_config rt = data->runtime;
    k_mutex_unlock(&data->lock);

    uint32_t report_interval_min_ms = rt.report_interval_min_ms;
    int64_t now = k_uptime_get();

    if (!config->disable_burst_read) {
        // Burst mode requires cs pin to reset
        int err = pmw3610_read(dev, PMW3610_REG_MOTION_BURST, buf, sizeof(buf));
        if (err) {
            return err;
        }
    } else {
        for (size_t i = 0; i < PMW3610_BURST_SIZE; i++) {
            int err = pmw3610_read(dev, PMW3610_REG_MOTION + i, &buf[i], 1);
            if (err) {
                return err;
            }
        }
    }
    // LOG_HEXDUMP_DBG(buf, sizeof(buf), "buf");

// 12-bit two's complement value to int16_t
// adapted from https://stackoverflow.com/questions/70802306/convert-a-12-bit-signed-number-in-c
#define TOINT16(val, bits) (((struct { int16_t value : bits; }){val}).value)

    int16_t x = TOINT16((buf[PMW3610_X_L_POS] + ((buf[PMW3610_XY_H_POS] & 0xF0) << 4)), 12);
    int16_t y = TOINT16((buf[PMW3610_Y_L_POS] + ((buf[PMW3610_XY_H_POS] & 0x0F) << 8)), 12);
    LOG_DBG("x/y: %d/%d", x, y);

    if (rt.swap_xy) {
        int16_t a = x;
        x = y;
        y = a;
    }
    if (rt.invert_x) {
        x = -x;
    }
    if (rt.invert_y) {
        y = -y;
    }

    if (rt.smart_algorithm) {
        int16_t shutter =
            ((int16_t)(buf[PMW3610_SHUTTER_H_POS] & 0x01) << 8) + buf[PMW3610_SHUTTER_L_POS];
        if (data->sw_smart_flag && shutter < 45) {
            pmw3610_write(dev, 0x32, 0x00);
            data->sw_smart_flag = false;
        }
        if (!data->sw_smart_flag && shutter > 45) {
            pmw3610_write(dev, 0x32, 0x80);
            data->sw_smart_flag = true;
        }
    }

    if (report_interval_min_ms > 0) {
        // purge accumulated delta, if last sampled had not been reported on last report tick
        if (now - data->last_smp_time >= report_interval_min_ms) {
            data->dx = 0;
            data->dy = 0;
        }
        data->last_smp_time = now;
    }

    // accumulate delta until report in next iteration
    data->dx += x;
    data->dy += y;

    if (report_interval_min_ms > 0) {
        // strict to report inerval
        if (now - data->last_rpt_time < report_interval_min_ms) {
            return 0;
        }
    }

    // fetch report value
    int16_t rx = (int16_t)CLAMP(data->dx, INT16_MIN, INT16_MAX);
    int16_t ry = (int16_t)CLAMP(data->dy, INT16_MIN, INT16_MAX);
    bool have_x = rx != 0;
    bool have_y = ry != 0;

    if (have_x || have_y) {
        if (report_interval_min_ms > 0) {
            data->last_rpt_time = now;
        }
        data->dx = 0;
        data->dy = 0;
        if (have_x) {
            input_report(dev, config->evt_type, config->x_input_code, rx, !have_y, K_NO_WAIT);
        }
        if (have_y) {
            input_report(dev, config->evt_type, config->y_input_code, ry, true, K_NO_WAIT);
        }
    }

    return 0;
}

static void pmw3610_gpio_callback(const struct device *gpiob, struct gpio_callback *cb,
                                  uint32_t pins) {
    struct pixart_data *data = CONTAINER_OF(cb, struct pixart_data, irq_gpio_cb);
    const struct device *dev = data->dev;
    pmw3610_set_interrupt(dev, false);
    k_work_submit(&data->trigger_work);
}

static void pmw3610_work_callback(struct k_work *work) {
    struct pixart_data *data = CONTAINER_OF(work, struct pixart_data, trigger_work);
    const struct device *dev = data->dev;
    pmw3610_report_data(dev);
    pmw3610_set_interrupt(dev, true);
}

static int pmw3610_enable_irq(const struct device *dev) {
    int err;
    struct pixart_data *data = dev->data;
    const struct pixart_config *config = dev->config;

    err = gpio_pin_configure_dt(&config->irq_gpio, GPIO_INPUT);
    if (err) {
        LOG_ERR("Cannot configure IRQ GPIO");
        return err;
    }
    err = gpio_add_callback(config->irq_gpio.port, &data->irq_gpio_cb);
    if (err) {
        LOG_ERR("Cannot add IRQ GPIO callback");
    }

    return err;
}

static int pmw3610_disable_irq(const struct device *dev) {
    const struct pixart_config *config = dev->config;
    struct pixart_data *data = dev->data;
    int ret = pmw3610_set_interrupt(dev, false);
    if (ret < 0) {
        return ret;
    }
    ret = gpio_pin_configure_dt(&config->irq_gpio, GPIO_DISCONNECTED);
    if (ret < 0) {
        return ret;
    }
    ret = gpio_remove_callback(config->irq_gpio.port, &data->irq_gpio_cb);
    return ret;
}

static int pmw3610_resume(const struct device *dev) {
    struct pixart_data *data = dev->data;
    const struct pixart_config *config = dev->config;
    int err = pmw3610_enable_irq(dev);
    if (err) {
        return err;
    }
    if (data->ready) {
        return pmw3610_set_interrupt(dev, true);
    }
    data->async_init_step = 0;
    data->async_init_retry_count = 0;
    k_work_schedule(&data->init_work, K_MSEC(async_init_delay[data->async_init_step]));
    return 0;
}

static int pmw3610_init(const struct device *dev) {
    struct pixart_data *data = dev->data;
    const struct pixart_config *config = dev->config;
    int err;

    if (!spi_is_ready_dt(&config->spi)) {
        LOG_ERR("%s is not ready", config->spi.bus->name);
        return -ENODEV;
    }

    // init device pointer
    data->dev = dev;

    // init smart algorithm flag;
    data->sw_smart_flag = false;

    k_mutex_init(&data->lock);

    // Seed runtime config from Kconfig/DT defaults. When
    // CONFIG_ZMK_PMW3610_CUSTOM_SETTINGS is enabled, the settings module
    // overwrites these fields (before async init completes, or immediately
    // via the setters once it does) with the persisted/effective setting
    // values.
    data->runtime = (struct pixart_runtime_config){
        .cpi = config->cpi,
        .swap_xy = IS_ENABLED(CONFIG_PMW3610_SWAP_XY),
        .invert_x = IS_ENABLED(CONFIG_PMW3610_INVERT_X),
        .invert_y = IS_ENABLED(CONFIG_PMW3610_INVERT_Y),
        .force_awake = config->force_awake,
        .smart_algorithm = IS_ENABLED(CONFIG_PMW3610_SMART_ALGORITHM),
        .run_downshift_ms = CONFIG_PMW3610_RUN_DOWNSHIFT_TIME_MS,
        .rest1_downshift_ms = CONFIG_PMW3610_REST1_DOWNSHIFT_TIME_MS,
        .rest2_downshift_ms = CONFIG_PMW3610_REST2_DOWNSHIFT_TIME_MS,
        .rest1_sample_ms = CONFIG_PMW3610_REST1_SAMPLE_TIME_MS,
        .rest2_sample_ms = CONFIG_PMW3610_REST2_SAMPLE_TIME_MS,
        .rest3_sample_ms = CONFIG_PMW3610_REST3_SAMPLE_TIME_MS,
        .report_interval_min_ms = CONFIG_PMW3610_REPORT_INTERVAL_MIN,
    };
    data->dx = 0;
    data->dy = 0;
    data->last_smp_time = 0;
    data->last_rpt_time = 0;

    // init trigger handler work
    k_work_init(&data->trigger_work, pmw3610_work_callback);

    // Setup delayable and non-blocking init jobs, including following steps:
    // 1. power reset
    // 2. upload initial settings
    // 3. other configs like cpi, downshift time, sample time etc.
    // The sensor is ready to work (i.e., data->ready=true after the above steps
    // are finished)
    k_work_init_delayable(&data->init_work, pmw3610_async_init);

    if (!device_is_ready(config->irq_gpio.port)) {
        LOG_ERR("IRQ GPIO device not ready");
        return -ENODEV;
    }
    gpio_init_callback(&data->irq_gpio_cb, pmw3610_gpio_callback, BIT(config->irq_gpio.pin));
    err = pmw3610_resume(dev);
    return err;
}

static int pmw3610_attr_set(const struct device *dev, enum sensor_channel chan,
                            enum sensor_attribute attr, const struct sensor_value *val) {
    struct pixart_data *data = dev->data;
    int err;

    if (unlikely(chan != SENSOR_CHAN_ALL)) {
        return -ENOTSUP;
    }

    if (unlikely(!data->ready)) {
        LOG_DBG("Device is not initialized yet");
        return -EBUSY;
    }

    switch ((uint32_t)attr) {
    case PMW3610_ATTR_CPI:
        err = pmw3610_set_cpi_runtime(dev, PMW3610_SVALUE_TO_CPI(*val));
        break;

    case PMW3610_ATTR_RUN_DOWNSHIFT_TIME:
        err = pmw3610_set_run_downshift_ms(dev, PMW3610_SVALUE_TO_TIME(*val));
        break;

    case PMW3610_ATTR_REST1_DOWNSHIFT_TIME:
        err = pmw3610_set_rest1_downshift_ms(dev, PMW3610_SVALUE_TO_TIME(*val));
        break;

    case PMW3610_ATTR_REST2_DOWNSHIFT_TIME:
        err = pmw3610_set_rest2_downshift_ms(dev, PMW3610_SVALUE_TO_TIME(*val));
        break;

    case PMW3610_ATTR_REST1_SAMPLE_TIME:
        err = pmw3610_set_rest1_sample_ms(dev, PMW3610_SVALUE_TO_TIME(*val));
        break;

    case PMW3610_ATTR_REST2_SAMPLE_TIME:
        err = pmw3610_set_rest2_sample_ms(dev, PMW3610_SVALUE_TO_TIME(*val));
        break;

    case PMW3610_ATTR_REST3_SAMPLE_TIME:
        err = pmw3610_set_rest3_sample_ms(dev, PMW3610_SVALUE_TO_TIME(*val));
        break;

    default:
        LOG_ERR("Unknown attribute");
        err = -ENOTSUP;
    }

    return err;
}

static const struct sensor_driver_api pmw3610_driver_api = {
    .attr_set = pmw3610_attr_set,
};

#if IS_ENABLED(CONFIG_PM_DEVICE)

static int pmw3610_suspend(const struct device *dev) {
    const struct pixart_config *config = dev->config;
    struct pixart_data *data = dev->data;
    LOG_DBG("Suspend PMW3610");
    if (!data->ready) {
        k_work_cancel_delayable(&data->init_work);
    }
    data->ready = false;
    data->async_init_step = ASYNC_INIT_STEP_COUNT; // to finish on-going async init step
    data->async_init_retry_count = 0;

    int ret = pmw3610_disable_irq(dev);
    if (ret < 0) {
        return ret;
    }
    // Below code doesn't work. PMW3610 doesn't wake up from shutdown mode (if
    // NCS is always active?)
    // ret = pmw3610_write_reg(dev, PMW3610_REG_SHUTDOWN,
    //                         PMW3610_SHUTDOWN_CMD_SHUTDOWN);
    // if (ret < 0) {
    //     return ret;
    // }
    return 0;
}

static int pmw3610_pm_action(const struct device *dev, enum pm_device_action action) {
    LOG_DBG("pmw3610_pm_action %d", action);
    switch (action) {
    case PM_DEVICE_ACTION_SUSPEND:
        return pmw3610_suspend(dev);
    case PM_DEVICE_ACTION_RESUME:
        return pmw3610_resume(dev);
    default:
        return -ENOTSUP;
    }
    return 0;
}

#endif // IS_ENABLED(CONFIG_PM_DEVICE)

#define PMW3610_SPI_MODE                                                                           \
    (SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_MODE_CPOL | SPI_MODE_CPHA | SPI_TRANSFER_MSB)

#define PMW3610_DEFINE(n)                                                                          \
    static struct pixart_data data##n;                                                             \
    static const struct pixart_config config##n = {                                                \
        .spi = SPI_DT_SPEC_INST_GET(n, PMW3610_SPI_MODE, 0),                                       \
        .irq_gpio = GPIO_DT_SPEC_INST_GET(n, irq_gpios),                                           \
        .cpi = DT_PROP(DT_DRV_INST(n), cpi),                                                       \
        .evt_type = DT_PROP(DT_DRV_INST(n), evt_type),                                             \
        .x_input_code = DT_PROP(DT_DRV_INST(n), x_input_code),                                     \
        .y_input_code = DT_PROP(DT_DRV_INST(n), y_input_code),                                     \
        .force_awake = DT_PROP(DT_DRV_INST(n), force_awake),                                       \
        .disable_burst_read = DT_PROP(DT_DRV_INST(n), disable_burst_read),                         \
    };                                                                                             \
    PM_DEVICE_DT_INST_DEFINE(n, pmw3610_pm_action);                                                \
    DEVICE_DT_INST_DEFINE(n, pmw3610_init, PM_DEVICE_DT_INST_GET(n), &data##n, &config##n,         \
                          POST_KERNEL, CONFIG_INPUT_PMW3610_INIT_PRIORITY, &pmw3610_driver_api);

DT_INST_FOREACH_STATUS_OKAY(PMW3610_DEFINE)

#define GET_PMW3610_DEV(node_id) DEVICE_DT_GET(node_id),

static const struct device *pmw3610_devs[] = {
    DT_FOREACH_STATUS_OKAY(cormoran_pmw3610, GET_PMW3610_DEV)};

static int on_activity_state(const zmk_event_t *eh) {
    struct zmk_activity_state_changed *state_ev = as_zmk_activity_state_changed(eh);

    if (!state_ev) {
        LOG_WRN("NO EVENT, leaving early");
        return 0;
    }

    bool enable = state_ev->state == ZMK_ACTIVITY_ACTIVE ? 1 : 0;
    LOG_DBG("Change PMW3610 performance to %s", enable ? "active" : "inactive");
    for (size_t i = 0; i < ARRAY_SIZE(pmw3610_devs); i++) {
        const struct device *dev = pmw3610_devs[i];
        struct pixart_data *data = dev->data;
        if (!data->ready) {
            continue;
        }
        k_mutex_lock(&data->lock, K_FOREVER);
        bool force_awake = data->runtime.force_awake;
        k_mutex_unlock(&data->lock);
        pmw3610_set_performance(dev, force_awake, enable);
    }

    return 0;
}

ZMK_LISTENER(zmk_pmw3610_idle_sleeper, on_activity_state);
ZMK_SUBSCRIPTION(zmk_pmw3610_idle_sleeper, zmk_activity_state_changed);

//////// Public API (see include/cormoran/pmw3610/pmw3610_api.h) //////////

size_t pmw3610_device_count(void) { return ARRAY_SIZE(pmw3610_devs); }

const struct device *pmw3610_get_device(size_t index) {
    if (index >= ARRAY_SIZE(pmw3610_devs)) {
        return NULL;
    }
    return pmw3610_devs[index];
}

bool pmw3610_is_ready(const struct device *dev) {
    if (!dev) {
        return false;
    }
    struct pixart_data *data = dev->data;
    return data->ready;
}

int pmw3610_get_init_error(const struct device *dev) {
    if (!dev) {
        return -ENODEV;
    }
    struct pixart_data *data = dev->data;
    return data->err;
}

int pmw3610_read_register(const struct device *dev, uint8_t addr, uint8_t *value) {
    if (!dev) {
        return -ENODEV;
    }
    return pmw3610_read_reg(dev, addr, value);
}

int pmw3610_write_register(const struct device *dev, uint8_t addr, uint8_t value) {
    if (!dev) {
        return -ENODEV;
    }
    struct pixart_data *data = dev->data;
    k_mutex_lock(&data->lock, K_FOREVER);
    int err = pmw3610_write(dev, addr, value);
    k_mutex_unlock(&data->lock);
    return err;
}

int pmw3610_read_diagnostics(const struct device *dev, struct pmw3610_diagnostics *out) {
    if (!dev || !out) {
        return -ENODEV;
    }
    struct pixart_data *data = dev->data;
    if (!data->ready) {
        return -EBUSY;
    }

    uint8_t squal, shutter_hi, shutter_lo, pix_max, pix_avg, pix_min;
    int err;

    k_mutex_lock(&data->lock, K_FOREVER);
    do {
        if ((err = pmw3610_read_reg(dev, PMW3610_REG_SQUAL, &squal)) != 0) {
            break;
        }
        if ((err = pmw3610_read_reg(dev, PMW3610_REG_SHUTTER_HIGHER, &shutter_hi)) != 0) {
            break;
        }
        if ((err = pmw3610_read_reg(dev, PMW3610_REG_SHUTTER_LOWER, &shutter_lo)) != 0) {
            break;
        }
        if ((err = pmw3610_read_reg(dev, PMW3610_REG_PIX_MAX, &pix_max)) != 0) {
            break;
        }
        if ((err = pmw3610_read_reg(dev, PMW3610_REG_PIX_AVG, &pix_avg)) != 0) {
            break;
        }
        err = pmw3610_read_reg(dev, PMW3610_REG_PIX_MIN, &pix_min);
    } while (0);
    k_mutex_unlock(&data->lock);

    if (err) {
        return err;
    }

    out->squal = squal;
    out->shutter = ((uint16_t)(shutter_hi & 0x01) << 8) + shutter_lo;
    out->pix_max = pix_max;
    out->pix_avg = pix_avg;
    out->pix_min = pix_min;
    return 0;
}

int pmw3610_get_runtime_config(const struct device *dev, struct pmw3610_runtime_config *out) {
    if (!dev || !out) {
        return -ENODEV;
    }
    struct pixart_data *data = dev->data;

    k_mutex_lock(&data->lock, K_FOREVER);
    struct pixart_runtime_config rt = data->runtime;
    k_mutex_unlock(&data->lock);

    *out = (struct pmw3610_runtime_config){
        .cpi = rt.cpi,
        .swap_xy = rt.swap_xy,
        .invert_x = rt.invert_x,
        .invert_y = rt.invert_y,
        .force_awake = rt.force_awake,
        .smart_algorithm = rt.smart_algorithm,
        .run_downshift_ms = rt.run_downshift_ms,
        .rest1_downshift_ms = rt.rest1_downshift_ms,
        .rest2_downshift_ms = rt.rest2_downshift_ms,
        .rest1_sample_ms = rt.rest1_sample_ms,
        .rest2_sample_ms = rt.rest2_sample_ms,
        .rest3_sample_ms = rt.rest3_sample_ms,
        .report_interval_min_ms = rt.report_interval_min_ms,
    };
    return 0;
}

int pmw3610_set_cpi_runtime(const struct device *dev, uint32_t cpi) {
    if (!dev) {
        return -ENODEV;
    }
    if (cpi < PMW3610_MIN_CPI || cpi > PMW3610_MAX_CPI) {
        return -EINVAL;
    }
    struct pixart_data *data = dev->data;

    k_mutex_lock(&data->lock, K_FOREVER);
    data->runtime.cpi = cpi;
    int err = data->ready ? pmw3610_set_cpi_locked(dev, cpi) : 0;
    k_mutex_unlock(&data->lock);
    return err;
}

int pmw3610_set_run_downshift_ms(const struct device *dev, uint32_t value) {
    if (!dev) {
        return -ENODEV;
    }
    if (value < 32 || value > 8160) {
        return -EINVAL;
    }
    struct pixart_data *data = dev->data;

    k_mutex_lock(&data->lock, K_FOREVER);
    data->runtime.run_downshift_ms = value;
    int err =
        data->ready ? pmw3610_set_downshift_time(dev, PMW3610_REG_RUN_DOWNSHIFT, value, 0) : 0;
    k_mutex_unlock(&data->lock);
    return err;
}

int pmw3610_set_rest1_downshift_ms(const struct device *dev, uint32_t value) {
    if (!dev) {
        return -ENODEV;
    }
    struct pixart_data *data = dev->data;

    k_mutex_lock(&data->lock, K_FOREVER);
    uint32_t sample_ms = data->runtime.rest1_sample_ms;
    uint32_t mintime = 16 * sample_ms;
    uint32_t maxtime = 255 * 16 * sample_ms;
    if (value < mintime || value > maxtime) {
        k_mutex_unlock(&data->lock);
        return -EINVAL;
    }
    data->runtime.rest1_downshift_ms = value;
    int err = data->ready
                  ? pmw3610_set_downshift_time(dev, PMW3610_REG_REST1_DOWNSHIFT, value, sample_ms)
                  : 0;
    k_mutex_unlock(&data->lock);
    return err;
}

int pmw3610_set_rest2_downshift_ms(const struct device *dev, uint32_t value) {
    if (!dev) {
        return -ENODEV;
    }
    struct pixart_data *data = dev->data;

    k_mutex_lock(&data->lock, K_FOREVER);
    uint32_t sample_ms = data->runtime.rest2_sample_ms;
    uint32_t mintime = 128 * sample_ms;
    uint32_t maxtime = 255 * 128 * sample_ms;
    if (value < mintime || value > maxtime) {
        k_mutex_unlock(&data->lock);
        return -EINVAL;
    }
    data->runtime.rest2_downshift_ms = value;
    int err = data->ready
                  ? pmw3610_set_downshift_time(dev, PMW3610_REG_REST2_DOWNSHIFT, value, sample_ms)
                  : 0;
    k_mutex_unlock(&data->lock);
    return err;
}

static int pmw3610_set_sample_ms_common(const struct device *dev, uint8_t reg_addr,
                                        uint32_t *runtime_field, uint32_t value) {
    if (!dev) {
        return -ENODEV;
    }
    if (value < 10 || value > 2550) {
        return -EINVAL;
    }
    struct pixart_data *data = dev->data;

    k_mutex_lock(&data->lock, K_FOREVER);
    *runtime_field = value;
    int err = data->ready ? pmw3610_set_sample_time(dev, reg_addr, value) : 0;
    k_mutex_unlock(&data->lock);
    return err;
}

int pmw3610_set_rest1_sample_ms(const struct device *dev, uint32_t value) {
    if (!dev) {
        return -ENODEV;
    }
    struct pixart_data *data = dev->data;
    return pmw3610_set_sample_ms_common(dev, PMW3610_REG_REST1_RATE, &data->runtime.rest1_sample_ms,
                                        value);
}

int pmw3610_set_rest2_sample_ms(const struct device *dev, uint32_t value) {
    if (!dev) {
        return -ENODEV;
    }
    struct pixart_data *data = dev->data;
    return pmw3610_set_sample_ms_common(dev, PMW3610_REG_REST2_RATE, &data->runtime.rest2_sample_ms,
                                        value);
}

int pmw3610_set_rest3_sample_ms(const struct device *dev, uint32_t value) {
    if (!dev) {
        return -ENODEV;
    }
    struct pixart_data *data = dev->data;
    return pmw3610_set_sample_ms_common(dev, PMW3610_REG_REST3_RATE, &data->runtime.rest3_sample_ms,
                                        value);
}

int pmw3610_set_axis_flags(const struct device *dev, bool swap_xy, bool invert_x, bool invert_y) {
    if (!dev) {
        return -ENODEV;
    }
    struct pixart_data *data = dev->data;

    k_mutex_lock(&data->lock, K_FOREVER);
    data->runtime.swap_xy = swap_xy;
    data->runtime.invert_x = invert_x;
    data->runtime.invert_y = invert_y;
    k_mutex_unlock(&data->lock);
    return 0;
}

int pmw3610_set_force_awake(const struct device *dev, bool enabled) {
    if (!dev) {
        return -ENODEV;
    }
    struct pixart_data *data = dev->data;

    /* Reflect current keyboard activity state: force-awake only takes
     * effect while ZMK considers the keyboard active (matches
     * on_activity_state()'s existing behavior; this is not itself an
     * activity-state change, just an immediate re-application of it). */
    bool active = zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE;

    k_mutex_lock(&data->lock, K_FOREVER);
    data->runtime.force_awake = enabled;
    int err = data->ready ? pmw3610_set_performance(dev, enabled, active) : 0;
    k_mutex_unlock(&data->lock);
    return err;
}

int pmw3610_set_smart_algorithm(const struct device *dev, bool enabled) {
    if (!dev) {
        return -ENODEV;
    }
    struct pixart_data *data = dev->data;

    k_mutex_lock(&data->lock, K_FOREVER);
    data->runtime.smart_algorithm = enabled;
    if (!enabled) {
        data->sw_smart_flag = false;
    }
    k_mutex_unlock(&data->lock);
    return 0;
}

int pmw3610_set_report_interval_min(const struct device *dev, uint32_t value_ms) {
    if (!dev) {
        return -ENODEV;
    }
    if (value_ms > 1000) {
        return -EINVAL;
    }
    struct pixart_data *data = dev->data;

    k_mutex_lock(&data->lock, K_FOREVER);
    data->runtime.report_interval_min_ms = value_ms;
    k_mutex_unlock(&data->lock);
    return 0;
}

/* Recover normal navigation after a frame capture, which disturbs it: reset
 * the async init state machine to step 0 and (re)schedule the init work,
 * exactly like pmw3610_resume() does for the "not ready yet" case. Caller
 * must hold data->lock. */
static void pmw3610_capture_frame_recover_locked(const struct device *dev) {
    struct pixart_data *data = dev->data;

    data->ready = false;
    data->async_init_step = 0;
    data->async_init_retry_count = 0;
    k_work_schedule(&data->init_work, K_MSEC(async_init_delay[data->async_init_step]));
}

int pmw3610_capture_frame(const struct device *dev,
                          const struct pmw3610_frame_capture_params *params, uint8_t *buf,
                          uint16_t buf_len, uint16_t *out_count) {
    if (!dev) {
        return -ENODEV;
    }
    if (!buf || buf_len == 0 || !out_count) {
        return -EINVAL;
    }

    struct pixart_data *data = dev->data;

    struct pmw3610_frame_capture_params p = {
        .pixel_count = PMW3610_FRAME_CAPTURE_DEFAULT_PIXEL_COUNT,
        .max_invalid_retries = PMW3610_FRAME_CAPTURE_DEFAULT_MAX_INVALID_RETRIES,
        .write_frame_grab = false,
        .frame_grab_value = PMW3610_FRAME_CAPTURE_DEFAULT_FRAME_GRAB_VALUE,
        .write_pixel_grab_reset = true,
    };
    if (params) {
        if (params->pixel_count != 0) {
            p.pixel_count = params->pixel_count;
        }
        if (params->max_invalid_retries != 0) {
            p.max_invalid_retries = params->max_invalid_retries;
        }
        p.write_frame_grab = params->write_frame_grab;
        p.frame_grab_value = params->frame_grab_value;
        p.write_pixel_grab_reset = params->write_pixel_grab_reset;
    }
    if (p.pixel_count > buf_len) {
        p.pixel_count = buf_len;
    }

    *out_count = 0;

    k_mutex_lock(&data->lock, K_FOREVER);

    if (!data->ready) {
        k_mutex_unlock(&data->lock);
        return -EBUSY;
    }

    data->capture_active = true;
    k_mutex_unlock(&data->lock);

    /* Disable the motion IRQ outside the lock (matches pmw3610_set_interrupt
     * usage elsewhere in this file, which does not require data->lock). */
    pmw3610_set_interrupt(dev, false);

    int err = 0;
    if (p.write_pixel_grab_reset) {
        err = pmw3610_write_reg(dev, PMW3610_REG_PIXEL_GRAB, 0x00);
    }
    if (!err && p.write_frame_grab) {
        err = pmw3610_write_reg(dev, PMW3610_REG_FRAME_GRAB, p.frame_grab_value);
    }

    uint16_t collected = 0;
    int64_t deadline = k_uptime_get() + PMW3610_FRAME_CAPTURE_TIMEOUT_MS;

    if (!err) {
        for (uint16_t i = 0; i < p.pixel_count; i++) {
            uint16_t invalid_retries = 0;

            for (;;) {
                if (k_uptime_get() >= deadline) {
                    LOG_WRN("Frame capture timed out after %u pixel(s) (of %u requested)",
                            collected, p.pixel_count);
                    goto capture_done;
                }

                uint8_t value;
                int read_err = pmw3610_read_reg(dev, PMW3610_REG_PIXEL_GRAB, &value);
                if (read_err) {
                    err = read_err;
                    LOG_ERR("Frame capture: PIXEL_GRAB read failed (errno %d) after %u pixel(s)",
                            read_err, collected);
                    goto capture_done;
                }

                if (value & PMW3610_PIXEL_GRAB_VALID_BIT) {
                    /* Store the raw byte (bit7 kept) -- the host masks
                     * bits[6:0] and can inspect bit7 for validity. */
                    buf[collected++] = value;
                    break;
                }

                invalid_retries++;
                if (invalid_retries >= p.max_invalid_retries) {
                    LOG_WRN("Frame capture: %u consecutive invalid reads at pixel %u, aborting "
                            "with %u pixel(s) collected",
                            invalid_retries, i, collected);
                    goto capture_done;
                }
                k_busy_wait(PMW3610_FRAME_CAPTURE_RETRY_DELAY_US);
            }
        }
    }

capture_done:
    *out_count = collected;

    k_mutex_lock(&data->lock, K_FOREVER);
    pmw3610_capture_frame_recover_locked(dev);
    data->capture_active = false;
    k_mutex_unlock(&data->lock);

    /* IRQ is re-enabled once async init reaches ASYNC_INIT_STEP_COUNT (see
     * pmw3610_async_init()), matching pmw3610_resume()'s not-ready path. */

    if (err) {
        return err;
    }
    return 0;
}
