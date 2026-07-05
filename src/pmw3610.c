/*
 * Copyright (c) 2022 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT cormoran_pmw3610

#include <string.h>
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
// Official Pixel_Grab procedure from the PMW3610 datasheet (R2.4,
// PIXEL_GRAB register 0x35 usage section + Figure 17 "Pixel Address Map
// for 22x22").
#define PMW3610_FRAME_CAPTURE_DEFAULT_PIXEL_COUNT 484 // 22 x 22 (datasheet)
// Per-pixel "pixel ready" (OBSERVATION1 bit2) wait budget: number of 10ms
// sleep+recheck retries before aborting the capture. The datasheet says
// "wait for 10ms and verify again" without an explicit bound; default 3
// retries, clamped to 1..100 when set per-request.
#define PMW3610_FRAME_CAPTURE_DEFAULT_WAIT_RETRIES 3
#define PMW3610_FRAME_CAPTURE_MAX_WAIT_RETRIES 100
#define PMW3610_FRAME_CAPTURE_WAIT_MS 10
// Actual polling period while waiting (finer than the 10ms datasheet wait
// unit -- see pmw3610_capture_wait_bit()).
#define PMW3610_FRAME_CAPTURE_POLL_MS 1
// Wait budget (in PMW3610_FRAME_CAPTURE_WAIT_MS units) for the first
// ready-bit waits after arming (step 8's 0x47 bit1 and the first pixel's
// 0x2D bit2): the ready bits update once per sensor frame, and a sensor
// woken from Rest3 (500ms/frame) may need most of a full rest-frame
// interval before the force-run PERFORMANCE write takes effect. Measured
// during Phase D hardware validation: the first capture after boot timed
// out with only the per-pixel budget. 70 units = 700ms covers one full
// Rest3 frame plus margin; once running, pixels advance at run-mode frame
// rate and the (much smaller) per-pixel budget applies.
#define PMW3610_FRAME_CAPTURE_STARTUP_WAIT_RETRIES 70
// Datasheet procedure register values (steps 1-7 of the Pixel_Grab usage
// section). 0xB4/0xD7 is an undocumented "page 1 magic enable"; 0x32=0x90
// turns the test clock on (the same 0x32 register the smart-algorithm
// logic writes 0x00/0x80 to -- the post-capture full re-init resets it).
#define PMW3610_REG_PIXEL_GRAB_MAGIC 0xB4
#define PMW3610_PIXEL_GRAB_MAGIC_VALUE 0xD7
#define PMW3610_REG_TEST_CLK 0x32
#define PMW3610_TEST_CLK_ON 0x90
#define PMW3610_PIXEL_GRAB_ARM 0x01
#define PMW3610_PRBS_READY_BIT BIT(1)    // 0x47 bit1: pixel grab armed/running
#define PMW3610_PRBS_COMPLETE_BIT BIT(0) // 0x47 bit0: all 484 pixels read
#define PMW3610_OBSERVATION1_PIXEL_RDY_BIT BIT(2)
#define PMW3610_PIXEL_GRAB_VALID_BIT BIT(7)
// Unconditional wall-clock bound on the whole procedure so a caller on the
// Studio RPC thread can never be blocked indefinitely by a misbehaving
// sensor. Independent of pixel_count / wait retries -- whichever bound is
// hit first wins. Typical complete captures are far faster (measured
// during Phase D hardware validation).
#define PMW3610_FRAME_CAPTURE_TIMEOUT_MS 5000

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
    /* Re-init starts with a power-up reset that clears the sensor's 0x32
     * register; reset the host-side smart-algorithm flag to match (see the
     * same reset in pmw3610_capture_frame_recover_locked()). */
    data->sw_smart_flag = false;
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

    pmw3610_settings_id_resolve(config->settings_id, config->dt_node_path, data->device_id);

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
        .settings_id = DT_PROP_OR(DT_DRV_INST(n), settings_id, NULL),                              \
        .dt_node_path = DT_NODE_PATH(DT_DRV_INST(n)),                                              \
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

int pmw3610_get_device_id(const struct device *dev, char *buf, size_t buf_len) {
    if (!dev || !buf || buf_len == 0) {
        return -EINVAL;
    }
    struct pixart_data *data = dev->data;
    strncpy(buf, data->device_id, buf_len - 1);
    buf[buf_len - 1] = '\0';
    return 0;
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

/* Recover normal navigation after a frame capture, which disturbs it (the
 * datasheet requires a reset to resume navigation): reset the async init
 * state machine to step 0 and (re)schedule the init work, exactly like
 * pmw3610_resume() does for the "not ready yet" case. Caller must hold
 * data->lock. */
static void pmw3610_capture_frame_recover_locked(const struct device *dev) {
    struct pixart_data *data = dev->data;

    data->ready = false;
    data->async_init_step = 0;
    data->async_init_retry_count = 0;
    /* The capture procedure wrote PMW3610_REG_TEST_CLK (0x32) = 0x90, the
     * same register the smart-algorithm logic tracks via sw_smart_flag
     * (0x00/0x80 writes in pmw3610_report_data). The power-up reset in the
     * re-init resets the sensor-side register, so the host-side flag must
     * be reset too or the smart-algorithm logic would skip its next 0x80
     * write. (pmw3610_init() seeds this flag, but re-init paths do not go
     * through pmw3610_init().) */
    data->sw_smart_flag = false;
    k_work_schedule(&data->init_work, K_MSEC(async_init_delay[data->async_init_step]));
}

/* Poll `reg` every PMW3610_FRAME_CAPTURE_POLL_MS until (value & mask) != 0,
 * up to a time budget of `max_retries` datasheet wait units
 * (PMW3610_FRAME_CAPTURE_WAIT_MS = 10ms each) and the wall-clock `deadline`
 * (whichever is hit first). The datasheet phrases the wait as "wait for
 * 10ms and verify again"; polling finer than 10ms is harmless (just more
 * SPI reads) and matters for throughput: the pixel-ready bit updates once
 * per sensor frame (< 10ms in forced run mode), so 10ms sleep quantization
 * would cost ~10ms/pixel (~4.8s per full frame, measured), while 1ms
 * polling tracks the actual frame period. Returns 0 when the bit is set,
 * -ETIMEDOUT when the budget/deadline is exhausted, or a negative SPI
 * errno. */
static int pmw3610_capture_wait_bit(const struct device *dev, uint8_t reg, uint8_t mask,
                                    uint16_t max_retries, int64_t deadline) {
    int64_t budget_end = k_uptime_get() + (int64_t)max_retries * PMW3610_FRAME_CAPTURE_WAIT_MS;
    for (;;) {
        uint8_t value;
        int err = pmw3610_read_reg(dev, reg, &value);
        if (err) {
            return err;
        }
        if (value & mask) {
            return 0;
        }
        int64_t now = k_uptime_get();
        if (now >= budget_end || now >= deadline) {
            return -ETIMEDOUT;
        }
        k_sleep(K_MSEC(PMW3610_FRAME_CAPTURE_POLL_MS));
    }
}

int pmw3610_capture_frame(const struct device *dev,
                          const struct pmw3610_frame_capture_params *params, uint8_t *buf,
                          uint16_t buf_len, struct pmw3610_frame_capture_result *result) {
    if (!dev) {
        return -ENODEV;
    }
    if (!buf || buf_len == 0 || !result) {
        return -EINVAL;
    }

    struct pixart_data *data = dev->data;

    uint16_t pixel_count = PMW3610_FRAME_CAPTURE_DEFAULT_PIXEL_COUNT;
    uint16_t wait_retries = PMW3610_FRAME_CAPTURE_DEFAULT_WAIT_RETRIES;
    if (params) {
        if (params->pixel_count != 0) {
            pixel_count = params->pixel_count;
        }
        if (params->max_invalid_retries != 0) {
            wait_retries =
                CLAMP(params->max_invalid_retries, 1, PMW3610_FRAME_CAPTURE_MAX_WAIT_RETRIES);
        }
    }
    if (pixel_count > buf_len) {
        pixel_count = buf_len;
    }

    *result = (struct pmw3610_frame_capture_result){0};

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

    int64_t start = k_uptime_get();
    int64_t deadline = start + PMW3610_FRAME_CAPTURE_TIMEOUT_MS;
    uint16_t collected = 0;

    /* Force run mode for the duration of the capture: the OBSERVATION1
     * bits used as the per-pixel ready handshake are "set every frame"
     * (datasheet), so the pixel cadence is the sensor's frame rate. A
     * stationary sensor downshifts to Rest3 (~500ms/frame -- measured
     * ~433ms/pixel during Phase D hardware validation, i.e. ~4 minutes
     * for a full 484-pixel frame). Setting PERFORMANCE bits[7:4] = 0xF
     * (force awake, same write pmw3610_set_performance() uses) keeps the
     * sensor in Run mode so pixels advance at run-mode frame rate; the
     * post-capture power-up reset restores the register. */
    uint8_t perf = 0;
    int err = pmw3610_read_reg(dev, PMW3610_REG_PERFORMANCE, &perf);
    if (!err) {
        err = pmw3610_write(dev, PMW3610_REG_PERFORMANCE, (perf & 0x0F) | 0xF0);
    }
    if (err) {
        LOG_ERR("Frame capture: force-run PERFORMANCE write failed (errno %d)", err);
        goto capture_done;
    }

    /* Confirm run mode before arming: the force-run write only manifests
     * at the next sensor frame boundary, which from Rest3 can be ~500ms
     * away. Clear OBSERVATION1 (any write clears its bits), then wait for
     * the frame bits to repopulate with the MODE bits (bits[7:6]) showing
     * Run (00). Arming while the sensor is still in a rest mode makes the
     * per-pixel ready bit follow the (much slower) rest frame cadence --
     * measured 30ms-budget misses in Rest1 (40ms/frame) and full-startup
     * timeouts from Rest2/3 during Phase D hardware validation. */
    err = pmw3610_write_reg(dev, PMW3610_REG_OBSERVATION, 0x00);
    if (err) {
        LOG_ERR("Frame capture: OBSERVATION1 clear failed (errno %d)", err);
        goto capture_done;
    }
    {
        int64_t run_budget_end =
            k_uptime_get() +
            (int64_t)PMW3610_FRAME_CAPTURE_STARTUP_WAIT_RETRIES * PMW3610_FRAME_CAPTURE_WAIT_MS;
        for (;;) {
            uint8_t obs = 0;
            err = pmw3610_read_reg(dev, PMW3610_REG_OBSERVATION, &obs);
            if (err) {
                LOG_ERR("Frame capture: OBSERVATION1 read failed (errno %d)", err);
                goto capture_done;
            }
            /* Frame bits repopulated + MODE bits (bits[7:6]) == 00 (Run). */
            if ((obs & 0x0F) != 0 && (obs & 0xC0) == 0) {
                break;
            }
            int64_t now = k_uptime_get();
            if (now >= run_budget_end || now >= deadline) {
                err = -ETIMEDOUT;
                LOG_WRN("Frame capture: sensor did not reach run mode (0x2D=0x%02x)", obs);
                goto capture_done;
            }
            k_sleep(K_MSEC(PMW3610_FRAME_CAPTURE_POLL_MS));
        }
    }

    /* Datasheet (R2.4) Pixel_Grab procedure, steps 1-7: arm the pixel
     * grabber. Raw pmw3610_write_reg() on purpose -- the sequence manages
     * the 0x41 SPI clock request itself (steps 1 and 5), so the
     * pmw3610_write() clk-on/off wrapper must NOT be used here. */
    static const uint8_t arm_seq[][2] = {
        {PMW3610_REG_SPI_CLK_ON_REQ, PMW3610_SPI_CLOCK_CMD_ENABLE},     // 1) 0x41 = 0xBA
        {PMW3610_REG_SPI_PAGE0, 0xFF},                                  // 2) 0x7F = 0xFF (page 1)
        {PMW3610_REG_PIXEL_GRAB_MAGIC, PMW3610_PIXEL_GRAB_MAGIC_VALUE}, // 3) 0xB4 = 0xD7
        {PMW3610_REG_SPI_PAGE0, 0x00},                                  // 4) 0x7F = 0x00 (page 0)
        {PMW3610_REG_SPI_CLK_ON_REQ, PMW3610_SPI_CLOCK_CMD_DISABLE},    // 5) 0x41 = 0xB5
        {PMW3610_REG_TEST_CLK, PMW3610_TEST_CLK_ON},                    // 6) 0x32 = 0x90
        {PMW3610_REG_PIXEL_GRAB, PMW3610_PIXEL_GRAB_ARM},               // 7) 0x35 = 0x01
    };

    for (size_t i = 0; i < ARRAY_SIZE(arm_seq); i++) {
        err = pmw3610_write_reg(dev, arm_seq[i][0], arm_seq[i][1]);
        if (err) {
            LOG_ERR("Frame capture: arm step %u (reg 0x%02x) failed (errno %d)", (unsigned int)i,
                    arm_seq[i][0], err);
            goto capture_done;
        }
    }

    /* Step 8: wait for PRBS_TEST_CTL (0x47) bit1 (pixel grab running).
     * Startup budget: the sensor may still be waking from a rest mode
     * (see PMW3610_FRAME_CAPTURE_STARTUP_WAIT_RETRIES). */
    err = pmw3610_capture_wait_bit(dev, PMW3610_REG_PRBS_TEST_CTL, PMW3610_PRBS_READY_BIT,
                                   PMW3610_FRAME_CAPTURE_STARTUP_WAIT_RETRIES, deadline);
    if (err) {
        LOG_WRN("Frame capture: PRBS ready (0x47 bit1) not set (errno %d)", err);
        goto capture_done;
    }

    /* Steps 9-11, repeated per pixel: wait for OBSERVATION1 (0x2D) bit2,
     * read PIXEL_GRAB (0x35), write OBSERVATION1 = 0x01 to advance. The
     * first pixel also gets the startup budget (the frame that reflects
     * run mode may not have happened yet); subsequent pixels arrive at
     * run-mode frame rate and use the per-pixel budget. */
    for (uint16_t i = 0; i < pixel_count; i++) {
        uint16_t budget =
            (i == 0) ? MAX(wait_retries, PMW3610_FRAME_CAPTURE_STARTUP_WAIT_RETRIES) : wait_retries;
        err = pmw3610_capture_wait_bit(dev, PMW3610_REG_OBSERVATION,
                                       PMW3610_OBSERVATION1_PIXEL_RDY_BIT, budget, deadline);
        if (err) {
            LOG_WRN("Frame capture: pixel %u not ready (0x2D bit2, errno %d), aborting with %u "
                    "pixel(s) collected",
                    i, err, collected);
            goto capture_done;
        }

        uint8_t value;
        err = pmw3610_read_reg(dev, PMW3610_REG_PIXEL_GRAB, &value);
        if (err) {
            LOG_ERR("Frame capture: PIXEL_GRAB read failed (errno %d) after %u pixel(s)", err,
                    collected);
            goto capture_done;
        }
        /* Store the raw byte (bit7 = PG_Valid kept) -- the host masks
         * bits[6:0] and can inspect bit7 for capture quality. */
        buf[collected++] = value;

        err = pmw3610_write_reg(dev, PMW3610_REG_OBSERVATION, PMW3610_PIXEL_GRAB_ARM);
        if (err) {
            LOG_ERR("Frame capture: OBSERVATION1 advance write failed (errno %d) after %u "
                    "pixel(s)",
                    err, collected);
            goto capture_done;
        }
    }

    /* Step 12: PRBS_TEST_CTL (0x47) bit0 indicates all 484 pixels were
     * read successfully. Informational -- a partial-frame request (< 484
     * pixels) legitimately leaves it clear. */
    {
        uint8_t value;
        int status_err = pmw3610_read_reg(dev, PMW3610_REG_PRBS_TEST_CTL, &value);
        if (status_err) {
            LOG_WRN("Frame capture: completion status read failed (errno %d)", status_err);
        } else {
            result->complete = (value & PMW3610_PRBS_COMPLETE_BIT) != 0;
        }
    }

capture_done:
    result->pixel_count = collected;
    result->duration_ms = (uint32_t)(k_uptime_get() - start);
    LOG_INF("Frame capture: %u pixel(s) in %u ms, complete=%d, err=%d", collected,
            result->duration_ms, result->complete, err);

    k_mutex_lock(&data->lock, K_FOREVER);
    pmw3610_capture_frame_recover_locked(dev);
    data->capture_active = false;
    k_mutex_unlock(&data->lock);

    /* IRQ is re-enabled once async init reaches ASYNC_INIT_STEP_COUNT (see
     * pmw3610_async_init()), matching pmw3610_resume()'s not-ready path. */

    if (err && err != -ETIMEDOUT) {
        return err;
    }
    /* -ETIMEDOUT with some pixels collected is reported as a partial
     * success (result->pixel_count < requested, complete=false); with zero
     * pixels it still returns 0 so the caller can inspect the (empty)
     * result -- mirrors the previous abort-early behavior. */
    return 0;
}
