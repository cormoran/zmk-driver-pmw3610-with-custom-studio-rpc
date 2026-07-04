/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file pmw3610_settings.c
 *
 * @brief Registers PMW3610 sensor parameters as custom settings (subsystem
 * "cormoran__pmw3610"), and keeps every PMW3610 device's runtime config in
 * sync with the effective setting values:
 *
 *  - At boot: pmw3610_settings_apply_to_device() is called by the driver
 *    itself from pmw3610_async_init_configure() (see
 *    include/cormoran/pmw3610/pmw3610_settings_apply.h for why that call
 *    site -- rather than a SYS_INIT hook here -- avoids a boot-ordering
 *    race against zmk-feature-custom-settings' own settings_load()).
 *  - At runtime: a zmk_custom_setting_changed listener re-applies the
 *    changed key's current effective value to all PMW3610 devices,
 *    covering VALUE_UPDATED / SAVED / DISCARDED / RESET alike (all of them
 *    just mean "re-read the effective value and push it").
 *
 * The generic get/set/save/discard/reset/export RPC surface for these
 * settings is provided by zmk-feature-custom-settings' own Studio RPC
 * subsystem -- this file only *defines* the settings and reacts to changes;
 * it does not implement any RPC itself.
 */

#include <string.h>

#include <cormoran/zmk/custom_settings.h>
#include <cormoran/pmw3610/pmw3610_api.h>
#include <cormoran/pmw3610/pmw3610_settings_apply.h>

#include <zmk/event_manager.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define PMW3610_SETTINGS_SUBSYSTEM_ID "cormoran__pmw3610"

/* The CONFIG_PMW3610_* Kconfig symbols below only exist `if PMW3610` (see
 * Kconfig) -- this settings module itself is usable with zero PMW3610
 * devices present (e.g. native_sim, CONFIG_ZMK_PMW3610_CUSTOM_SETTINGS=y
 * without CONFIG_PMW3610), so fall back to the same hardcoded values used
 * as PMW3610's own Kconfig defaults when the driver core isn't compiled
 * in. When CONFIG_PMW3610 *is* enabled these fallbacks are unused (the
 * `#ifndef` loses to the real Kconfig-generated `#define`). */
#ifndef CONFIG_PMW3610_REST1_SAMPLE_TIME_MS
#define CONFIG_PMW3610_REST1_SAMPLE_TIME_MS 40
#endif
#ifndef CONFIG_PMW3610_REST2_SAMPLE_TIME_MS
#define CONFIG_PMW3610_REST2_SAMPLE_TIME_MS 100
#endif
#ifndef CONFIG_PMW3610_REST3_SAMPLE_TIME_MS
#define CONFIG_PMW3610_REST3_SAMPLE_TIME_MS 500
#endif
#ifndef CONFIG_PMW3610_RUN_DOWNSHIFT_TIME_MS
#define CONFIG_PMW3610_RUN_DOWNSHIFT_TIME_MS 128
#endif
#ifndef CONFIG_PMW3610_REST1_DOWNSHIFT_TIME_MS
#define CONFIG_PMW3610_REST1_DOWNSHIFT_TIME_MS 5000
#endif
#ifndef CONFIG_PMW3610_REST2_DOWNSHIFT_TIME_MS
#define CONFIG_PMW3610_REST2_DOWNSHIFT_TIME_MS 17000
#endif
#ifndef CONFIG_PMW3610_REPORT_INTERVAL_MIN
#define CONFIG_PMW3610_REPORT_INTERVAL_MIN 0
#endif
#ifndef CONFIG_PMW3610_SWAP_XY
#define CONFIG_PMW3610_SWAP_XY 0
#endif
#ifndef CONFIG_PMW3610_INVERT_X
#define CONFIG_PMW3610_INVERT_X 0
#endif
#ifndef CONFIG_PMW3610_INVERT_Y
#define CONFIG_PMW3610_INVERT_Y 0
#endif
#ifndef CONFIG_PMW3610_SMART_ALGORITHM
#define CONFIG_PMW3610_SMART_ALGORITHM 1
#endif

/* Default sample times (ms), matching Kconfig defaults. Used both as the
 * default value for the *_sample_ms settings and to compute the documented
 * (default-sample-time-based) range constraint for the corresponding
 * downshift setting -- the sensor's actual valid downshift range depends on
 * the *current* sample time (see pmw3610_set_downshift_time() in
 * pmw3610.c), so a downshift write that is in-range here may still be
 * rejected by the driver if the sample time was since changed. */
#define PMW3610_DEFAULT_REST1_SAMPLE_MS CONFIG_PMW3610_REST1_SAMPLE_TIME_MS
#define PMW3610_DEFAULT_REST2_SAMPLE_MS CONFIG_PMW3610_REST2_SAMPLE_TIME_MS

/* Downshift range bounds, using the *default* rest1/rest2 sample time (see
 * the doc comment on PMW3610_DEFAULT_REST1_SAMPLE_MS above): the sensor's
 * actual valid downshift range depends on the *current* sample time. */
#define PMW3610_REST1_DOWNSHIFT_MIN (16 * PMW3610_DEFAULT_REST1_SAMPLE_MS)
#define PMW3610_REST1_DOWNSHIFT_MAX (255 * 16 * PMW3610_DEFAULT_REST1_SAMPLE_MS)
#define PMW3610_REST2_DOWNSHIFT_MIN (128 * PMW3610_DEFAULT_REST2_SAMPLE_MS)
#define PMW3610_REST2_DOWNSHIFT_MAX (255 * 128 * PMW3610_DEFAULT_REST2_SAMPLE_MS)

/* NOTE: constraints are built with plain designated-initializer syntax
 * (no ZMK_CUSTOM_SETTING_RANGE_INT32()/_NO_CONSTRAINT compound-literal
 * macros) and defined directly with STRUCT_SECTION_ITERABLE (bypassing the
 * ZMK_CUSTOM_SETTING_DEFINE() convenience macro), matching the pattern used
 * by zmk-feature-custom-settings' own
 * src/test/zmk_config_sample_settings.c. This is *not* just a style choice:
 * ZMK_CUSTOM_SETTING_DEFINE()'s constraint argument is placed inside another
 * static array initializer (`_name##_constraints[] = {__VA_ARGS__}`), and
 * ZMK_CUSTOM_SETTING_RANGE_INT32() itself expands to a *nested* compound
 * literal (a compound-literal .range field whose .min/.max members are
 * themselves compound literals via ZMK_CUSTOM_SETTING_VALUE_INT32()).
 * Nested compound literals are not permitted inside a static/file-scope
 * initializer per C11 6.6p9, and arm-zephyr-eabi-gcc (unlike some other
 * compilers on some other constructs) enforces this strictly, failing with
 * "initializer element is not constant". Plain nested brace-initializers
 * (no cast, as used below) don't have this problem. */

static const struct zmk_custom_setting_constraint pmw3610_no_constraint[] = {
    {.type = ZMK_CUSTOM_SETTING_CONSTRAINT_NONE},
};

static const struct zmk_custom_setting_constraint pmw3610_cpi_constraint[] = {
    {.type = ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE,
     .range = {.min = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 200},
               .max = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 3200}}},
};

static const struct zmk_custom_setting_constraint pmw3610_run_downshift_constraint[] = {
    {.type = ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE,
     .range = {.min = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 32},
               .max = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 8160}}},
};

static const struct zmk_custom_setting_constraint pmw3610_rest1_downshift_constraint[] = {
    {.type = ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE,
     .range = {.min = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                       .int32_value = PMW3610_REST1_DOWNSHIFT_MIN},
               .max = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                       .int32_value = PMW3610_REST1_DOWNSHIFT_MAX}}},
};

static const struct zmk_custom_setting_constraint pmw3610_rest2_downshift_constraint[] = {
    {.type = ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE,
     .range = {.min = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                       .int32_value = PMW3610_REST2_DOWNSHIFT_MIN},
               .max = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                       .int32_value = PMW3610_REST2_DOWNSHIFT_MAX}}},
};

static const struct zmk_custom_setting_constraint pmw3610_sample_ms_constraint[] = {
    {.type = ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE,
     .range = {.min = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 10},
               .max = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 2550}}},
};

static const struct zmk_custom_setting_constraint pmw3610_report_interval_constraint[] = {
    {.type = ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE,
     .range = {.min = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 0},
               .max = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 1000}}},
};

#define PMW3610_SETTING_INT32(_name, _key, _default, _constraint)                                  \
    STRUCT_SECTION_ITERABLE(zmk_custom_setting, _name) = {                                         \
        .custom_subsystem_id = PMW3610_SETTINGS_SUBSYSTEM_ID,                                      \
        .key = _key,                                                                               \
        .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,                                              \
        .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,                                         \
        .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,                          \
        .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,                                 \
        .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,                                \
        .constraints = (_constraint),                                                              \
        .constraints_count = ARRAY_SIZE(_constraint),                                              \
        .default_value = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = (_default)}, \
    }

#define PMW3610_SETTING_BOOL(_name, _key, _default)                                                \
    STRUCT_SECTION_ITERABLE(zmk_custom_setting, _name) = {                                         \
        .custom_subsystem_id = PMW3610_SETTINGS_SUBSYSTEM_ID,                                      \
        .key = _key,                                                                               \
        .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,                                              \
        .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL,                                          \
        .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,                          \
        .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,                                 \
        .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,                                \
        .constraints = pmw3610_no_constraint,                                                      \
        .constraints_count = ARRAY_SIZE(pmw3610_no_constraint),                                    \
        .default_value = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL, .bool_value = (_default)},   \
    }

/* DT default cpi is 600 (dts/bindings/cormoran,pmw3610.yml). Devices with a
 * different DT `cpi` still boot with their own DT value until this setting
 * is written -- pmw3610_settings_apply_to_device() only overlays a setting
 * once its `initialized` flag is set (see zmk_custom_setting.initialized in
 * custom_settings.c), which is true from early boot, so in practice all
 * devices converge on this single global cpi setting immediately. This
 * matches the single-shared-setting design in DESIGN.md (one physical
 * sensor per board is the common case). */
PMW3610_SETTING_INT32(pmw3610_setting_cpi, "cpi", 600, pmw3610_cpi_constraint);
PMW3610_SETTING_BOOL(pmw3610_setting_swap_xy, "swap_xy", IS_ENABLED(CONFIG_PMW3610_SWAP_XY));
PMW3610_SETTING_BOOL(pmw3610_setting_invert_x, "invert_x", IS_ENABLED(CONFIG_PMW3610_INVERT_X));
PMW3610_SETTING_BOOL(pmw3610_setting_invert_y, "invert_y", IS_ENABLED(CONFIG_PMW3610_INVERT_Y));
PMW3610_SETTING_BOOL(pmw3610_setting_force_awake, "force_awake", false);
PMW3610_SETTING_BOOL(pmw3610_setting_smart_algorithm, "smart_algorithm",
                     IS_ENABLED(CONFIG_PMW3610_SMART_ALGORITHM));
PMW3610_SETTING_INT32(pmw3610_setting_run_downshift_ms, "run_downshift_ms",
                      CONFIG_PMW3610_RUN_DOWNSHIFT_TIME_MS, pmw3610_run_downshift_constraint);
PMW3610_SETTING_INT32(pmw3610_setting_rest1_downshift_ms, "rest1_downshift_ms",
                      CONFIG_PMW3610_REST1_DOWNSHIFT_TIME_MS, pmw3610_rest1_downshift_constraint);
PMW3610_SETTING_INT32(pmw3610_setting_rest2_downshift_ms, "rest2_downshift_ms",
                      CONFIG_PMW3610_REST2_DOWNSHIFT_TIME_MS, pmw3610_rest2_downshift_constraint);
PMW3610_SETTING_INT32(pmw3610_setting_rest1_sample_ms, "rest1_sample_ms",
                      PMW3610_DEFAULT_REST1_SAMPLE_MS, pmw3610_sample_ms_constraint);
PMW3610_SETTING_INT32(pmw3610_setting_rest2_sample_ms, "rest2_sample_ms",
                      PMW3610_DEFAULT_REST2_SAMPLE_MS, pmw3610_sample_ms_constraint);
PMW3610_SETTING_INT32(pmw3610_setting_rest3_sample_ms, "rest3_sample_ms",
                      CONFIG_PMW3610_REST3_SAMPLE_TIME_MS, pmw3610_sample_ms_constraint);
PMW3610_SETTING_INT32(pmw3610_setting_report_interval_min_ms, "report_interval_min_ms",
                      CONFIG_PMW3610_REPORT_INTERVAL_MIN, pmw3610_report_interval_constraint);

/* NOTE: force_awake's setting default above is `false`, independent of any
 * DT `force-awake` property, because a custom setting default must be a
 * compile-time constant shared by all devices, while `force-awake` is a
 * per-device DT property. A device that sets `force-awake;` in DT still
 * boots with force-awake enabled (seeded in pmw3610_init()) until this
 * setting's effective value (false, unless changed) is applied in
 * pmw3610_async_init_configure() -- i.e. enabling `force-awake` in DT alone
 * is overridden to `false` once CONFIG_ZMK_PMW3610_CUSTOM_SETTINGS is
 * enabled. This is called out in README.md. */

static int read_int32(const char *key, int32_t fallback) {
    struct zmk_custom_setting_value value;
    if (zmk_custom_setting_read_by_key(PMW3610_SETTINGS_SUBSYSTEM_ID, key, &value) != 0 ||
        value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32) {
        return fallback;
    }
    return value.int32_value;
}

static bool read_bool(const char *key, bool fallback) {
    struct zmk_custom_setting_value value;
    if (zmk_custom_setting_read_by_key(PMW3610_SETTINGS_SUBSYSTEM_ID, key, &value) != 0 ||
        value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL) {
        return fallback;
    }
    return value.bool_value;
}

void pmw3610_settings_apply_to_device(const struct device *dev) {
    if (!dev) {
        return;
    }

    pmw3610_set_cpi_runtime(dev, (uint32_t)read_int32("cpi", 600));
    pmw3610_set_axis_flags(dev, read_bool("swap_xy", false), read_bool("invert_x", false),
                           read_bool("invert_y", false));
    pmw3610_set_force_awake(dev, read_bool("force_awake", false));
    pmw3610_set_smart_algorithm(dev, read_bool("smart_algorithm", true));
    pmw3610_set_run_downshift_ms(
        dev, (uint32_t)read_int32("run_downshift_ms", CONFIG_PMW3610_RUN_DOWNSHIFT_TIME_MS));
    /* Apply sample times before downshift times: downshift range validation
     * in the driver depends on the *current* runtime sample time. */
    pmw3610_set_rest1_sample_ms(
        dev, (uint32_t)read_int32("rest1_sample_ms", PMW3610_DEFAULT_REST1_SAMPLE_MS));
    pmw3610_set_rest2_sample_ms(
        dev, (uint32_t)read_int32("rest2_sample_ms", PMW3610_DEFAULT_REST2_SAMPLE_MS));
    pmw3610_set_rest3_sample_ms(
        dev, (uint32_t)read_int32("rest3_sample_ms", CONFIG_PMW3610_REST3_SAMPLE_TIME_MS));
    pmw3610_set_rest1_downshift_ms(
        dev, (uint32_t)read_int32("rest1_downshift_ms", CONFIG_PMW3610_REST1_DOWNSHIFT_TIME_MS));
    pmw3610_set_rest2_downshift_ms(
        dev, (uint32_t)read_int32("rest2_downshift_ms", CONFIG_PMW3610_REST2_DOWNSHIFT_TIME_MS));
    pmw3610_set_report_interval_min(
        dev, (uint32_t)read_int32("report_interval_min_ms", CONFIG_PMW3610_REPORT_INTERVAL_MIN));
}

static void pmw3610_settings_apply_to_all_devices(void) {
    size_t count = pmw3610_device_count();
    for (size_t i = 0; i < count; i++) {
        pmw3610_settings_apply_to_device(pmw3610_get_device(i));
    }
}

static int pmw3610_settings_changed_listener(const zmk_event_t *eh) {
    const struct zmk_custom_setting_changed *ev = as_zmk_custom_setting_changed(eh);
    if (!ev || !ev->setting) {
        return 0;
    }

    if (strncmp(ev->setting->custom_subsystem_id, PMW3610_SETTINGS_SUBSYSTEM_ID,
                CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN) != 0) {
        return 0;
    }

    /* All changed kinds (VALUE_UPDATED / SAVED / DISCARDED / RESET) mean
     * the same thing to us: re-read the effective value and push it to
     * every device. Re-applying every key on any single key's change is
     * wasteful but simple and correct, and settings changes are rare
     * (interactive Studio RPC use, not a hot path). */
    pmw3610_settings_apply_to_all_devices();
    return 0;
}

ZMK_LISTENER(pmw3610_settings, pmw3610_settings_changed_listener);
ZMK_SUBSCRIPTION(pmw3610_settings, zmk_custom_setting_changed);
