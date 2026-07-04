/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file pmw3610_settings.c
 *
 * @brief Registers PMW3610 sensor parameters as custom settings (subsystem
 * "cormoran__pmw3610"), one independent set of keys per devicetree instance
 * ("<param>@<settings-id>", see pmw3610_settings_id.h), and keeps each
 * PMW3610 device's runtime config in sync with its own effective setting
 * values:
 *
 *  - At boot: pmw3610_settings_apply_to_device() is called by the driver
 *    itself from pmw3610_async_init_configure() (see
 *    include/cormoran/pmw3610/pmw3610_settings_apply.h for why that call
 *    site -- rather than a SYS_INIT hook here -- avoids a boot-ordering
 *    race against zmk-feature-custom-settings' own settings_load()).
 *  - At runtime: a zmk_custom_setting_changed listener re-applies every
 *    device's current effective values (each device only ever reads its
 *    own per-device keys, so this is "wasteful but correct", not
 *    "wrong") -- covers VALUE_UPDATED / SAVED / DISCARDED / RESET alike
 *    (all of them just mean "re-read the effective value and push it").
 *
 * The generic get/set/save/discard/reset/export RPC surface for these
 * settings is provided by zmk-feature-custom-settings' own Studio RPC
 * subsystem -- this file only *defines* the settings and reacts to changes;
 * it does not implement any RPC itself.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <cormoran/zmk/custom_settings.h>
#include <cormoran/pmw3610/pmw3610_api.h>
#include <cormoran/pmw3610/pmw3610_settings_apply.h>
#include <cormoran/pmw3610/pmw3610_settings_id.h>

#include <zmk/event_manager.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define DT_DRV_COMPAT cormoran_pmw3610

#define PMW3610_SETTINGS_SUBSYSTEM_ID "cormoran__pmw3610"

/* Longest field name is "report_interval_min_ms" (23 chars) + '@' (1) + up
 * to PMW3610_SETTINGS_ID_MAX_LEN (8) id chars + NUL (1) = 33; sized with
 * headroom. Comfortably under CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN
 * (default 48). */
#define PMW3610_SETTINGS_KEY_BUF_SIZE 40

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
        .key = (_key),                                                                             \
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
        .key = (_key),                                                                             \
        .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,                                              \
        .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL,                                          \
        .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,                          \
        .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,                                 \
        .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,                                \
        .constraints = pmw3610_no_constraint,                                                      \
        .constraints_count = ARRAY_SIZE(pmw3610_no_constraint),                                    \
        .default_value = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL, .bool_value = (_default)},   \
    }

/* --- Per-instance key storage + settings entries ----------------------- */
/*
 * One settings entry set per devicetree instance ("<param>@<id>"), instead
 * of one global set shared by every PMW3610 device. Each entry's `.key`
 * points at a per-instance static buffer (content filled at boot, before
 * settings_load(), by PMW3610_INIT_INST_KEYS below) rather than a string
 * literal -- `struct zmk_custom_setting.key` is `const char *`, so a mutable
 * buffer works exactly like a literal from every other call site's point of
 * view.
 *
 * `n` below is a compile-time devicetree instance index (from
 * DT_INST_FOREACH_STATUS_OKAY), not a runtime value -- token-pasted into
 * unique per-instance symbol names.
 */

#define PMW3610_INST_ID_VAR(n) pmw3610_settings_id_##n
#define PMW3610_INST_KEY_VAR(n, field) pmw3610_settings_key_##n##_##field

#define PMW3610_DECLARE_INST_STORAGE(n)                                                            \
    static char PMW3610_INST_ID_VAR(n)[PMW3610_SETTINGS_ID_BUF_SIZE];                              \
    static char PMW3610_INST_KEY_VAR(n, cpi)[PMW3610_SETTINGS_KEY_BUF_SIZE];                       \
    static char PMW3610_INST_KEY_VAR(n, swap_xy)[PMW3610_SETTINGS_KEY_BUF_SIZE];                   \
    static char PMW3610_INST_KEY_VAR(n, invert_x)[PMW3610_SETTINGS_KEY_BUF_SIZE];                  \
    static char PMW3610_INST_KEY_VAR(n, invert_y)[PMW3610_SETTINGS_KEY_BUF_SIZE];                  \
    static char PMW3610_INST_KEY_VAR(n, force_awake)[PMW3610_SETTINGS_KEY_BUF_SIZE];               \
    static char PMW3610_INST_KEY_VAR(n, smart_algorithm)[PMW3610_SETTINGS_KEY_BUF_SIZE];           \
    static char PMW3610_INST_KEY_VAR(n, run_downshift_ms)[PMW3610_SETTINGS_KEY_BUF_SIZE];          \
    static char PMW3610_INST_KEY_VAR(n, rest1_downshift_ms)[PMW3610_SETTINGS_KEY_BUF_SIZE];        \
    static char PMW3610_INST_KEY_VAR(n, rest2_downshift_ms)[PMW3610_SETTINGS_KEY_BUF_SIZE];        \
    static char PMW3610_INST_KEY_VAR(n, rest1_sample_ms)[PMW3610_SETTINGS_KEY_BUF_SIZE];           \
    static char PMW3610_INST_KEY_VAR(n, rest2_sample_ms)[PMW3610_SETTINGS_KEY_BUF_SIZE];           \
    static char PMW3610_INST_KEY_VAR(n, rest3_sample_ms)[PMW3610_SETTINGS_KEY_BUF_SIZE];           \
    static char PMW3610_INST_KEY_VAR(n, report_interval_min_ms)[PMW3610_SETTINGS_KEY_BUF_SIZE];

DT_INST_FOREACH_STATUS_OKAY(PMW3610_DECLARE_INST_STORAGE)

/* DT default cpi is 600 (dts/bindings/cormoran,pmw3610.yml); force_awake now
 * defaults to this instance's own DT `force-awake` property (previously a
 * single compile-time `false` shared by every device, since a single global
 * setting couldn't represent a per-device DT property -- per-instance
 * settings remove that limitation). */
#define PMW3610_DEFINE_INST_SETTINGS(n)                                                            \
    PMW3610_SETTING_INT32(pmw3610_setting_##n##_cpi, PMW3610_INST_KEY_VAR(n, cpi),                 \
                          DT_INST_PROP_OR(n, cpi, 600), pmw3610_cpi_constraint);                   \
    PMW3610_SETTING_BOOL(pmw3610_setting_##n##_swap_xy, PMW3610_INST_KEY_VAR(n, swap_xy),          \
                         IS_ENABLED(CONFIG_PMW3610_SWAP_XY));                                      \
    PMW3610_SETTING_BOOL(pmw3610_setting_##n##_invert_x, PMW3610_INST_KEY_VAR(n, invert_x),        \
                         IS_ENABLED(CONFIG_PMW3610_INVERT_X));                                     \
    PMW3610_SETTING_BOOL(pmw3610_setting_##n##_invert_y, PMW3610_INST_KEY_VAR(n, invert_y),        \
                         IS_ENABLED(CONFIG_PMW3610_INVERT_Y));                                     \
    PMW3610_SETTING_BOOL(pmw3610_setting_##n##_force_awake, PMW3610_INST_KEY_VAR(n, force_awake),  \
                         DT_INST_PROP(n, force_awake));                                            \
    PMW3610_SETTING_BOOL(pmw3610_setting_##n##_smart_algorithm,                                    \
                         PMW3610_INST_KEY_VAR(n, smart_algorithm),                                 \
                         IS_ENABLED(CONFIG_PMW3610_SMART_ALGORITHM));                              \
    PMW3610_SETTING_INT32(pmw3610_setting_##n##_run_downshift_ms,                                  \
                          PMW3610_INST_KEY_VAR(n, run_downshift_ms),                               \
                          CONFIG_PMW3610_RUN_DOWNSHIFT_TIME_MS, pmw3610_run_downshift_constraint); \
    PMW3610_SETTING_INT32(                                                                         \
        pmw3610_setting_##n##_rest1_downshift_ms, PMW3610_INST_KEY_VAR(n, rest1_downshift_ms),     \
        CONFIG_PMW3610_REST1_DOWNSHIFT_TIME_MS, pmw3610_rest1_downshift_constraint);               \
    PMW3610_SETTING_INT32(                                                                         \
        pmw3610_setting_##n##_rest2_downshift_ms, PMW3610_INST_KEY_VAR(n, rest2_downshift_ms),     \
        CONFIG_PMW3610_REST2_DOWNSHIFT_TIME_MS, pmw3610_rest2_downshift_constraint);               \
    PMW3610_SETTING_INT32(pmw3610_setting_##n##_rest1_sample_ms,                                   \
                          PMW3610_INST_KEY_VAR(n, rest1_sample_ms),                                \
                          PMW3610_DEFAULT_REST1_SAMPLE_MS, pmw3610_sample_ms_constraint);          \
    PMW3610_SETTING_INT32(pmw3610_setting_##n##_rest2_sample_ms,                                   \
                          PMW3610_INST_KEY_VAR(n, rest2_sample_ms),                                \
                          PMW3610_DEFAULT_REST2_SAMPLE_MS, pmw3610_sample_ms_constraint);          \
    PMW3610_SETTING_INT32(pmw3610_setting_##n##_rest3_sample_ms,                                   \
                          PMW3610_INST_KEY_VAR(n, rest3_sample_ms),                                \
                          CONFIG_PMW3610_REST3_SAMPLE_TIME_MS, pmw3610_sample_ms_constraint);      \
    PMW3610_SETTING_INT32(pmw3610_setting_##n##_report_interval_min_ms,                            \
                          PMW3610_INST_KEY_VAR(n, report_interval_min_ms),                         \
                          CONFIG_PMW3610_REPORT_INTERVAL_MIN, pmw3610_report_interval_constraint);

DT_INST_FOREACH_STATUS_OKAY(PMW3610_DEFINE_INST_SETTINGS)

/* Resolves this instance's settings id (DT `settings-id` property, or a
 * hash of the devicetree node path) and fills its 13 key buffers, all
 * before custom_settings_init() (APPLICATION init level, which resets
 * every registered zmk_custom_setting's value from .default_value) and
 * main()'s settings_load() (which reads persisted values by key) run --
 * guaranteed by running at POST_KERNEL, an earlier init level than
 * APPLICATION, regardless of relative priority within POST_KERNEL. Purely a
 * function of compile-time-known devicetree data, so this has no ordering
 * dependency on anything else in this init level. */
#define PMW3610_INIT_INST_KEYS_FN(n)                                                               \
    static int pmw3610_settings_keys_init_##n(void) {                                              \
        pmw3610_settings_id_resolve(DT_INST_PROP_OR(n, settings_id, NULL),                         \
                                    DT_NODE_PATH(DT_DRV_INST(n)), PMW3610_INST_ID_VAR(n));         \
        snprintf(PMW3610_INST_KEY_VAR(n, cpi), PMW3610_SETTINGS_KEY_BUF_SIZE, "cpi@%s",            \
                 PMW3610_INST_ID_VAR(n));                                                          \
        snprintf(PMW3610_INST_KEY_VAR(n, swap_xy), PMW3610_SETTINGS_KEY_BUF_SIZE, "swap_xy@%s",    \
                 PMW3610_INST_ID_VAR(n));                                                          \
        snprintf(PMW3610_INST_KEY_VAR(n, invert_x), PMW3610_SETTINGS_KEY_BUF_SIZE, "invert_x@%s",  \
                 PMW3610_INST_ID_VAR(n));                                                          \
        snprintf(PMW3610_INST_KEY_VAR(n, invert_y), PMW3610_SETTINGS_KEY_BUF_SIZE, "invert_y@%s",  \
                 PMW3610_INST_ID_VAR(n));                                                          \
        snprintf(PMW3610_INST_KEY_VAR(n, force_awake), PMW3610_SETTINGS_KEY_BUF_SIZE,              \
                 "force_awake@%s", PMW3610_INST_ID_VAR(n));                                        \
        snprintf(PMW3610_INST_KEY_VAR(n, smart_algorithm), PMW3610_SETTINGS_KEY_BUF_SIZE,          \
                 "smart_algorithm@%s", PMW3610_INST_ID_VAR(n));                                    \
        snprintf(PMW3610_INST_KEY_VAR(n, run_downshift_ms), PMW3610_SETTINGS_KEY_BUF_SIZE,         \
                 "run_downshift_ms@%s", PMW3610_INST_ID_VAR(n));                                   \
        snprintf(PMW3610_INST_KEY_VAR(n, rest1_downshift_ms), PMW3610_SETTINGS_KEY_BUF_SIZE,       \
                 "rest1_downshift_ms@%s", PMW3610_INST_ID_VAR(n));                                 \
        snprintf(PMW3610_INST_KEY_VAR(n, rest2_downshift_ms), PMW3610_SETTINGS_KEY_BUF_SIZE,       \
                 "rest2_downshift_ms@%s", PMW3610_INST_ID_VAR(n));                                 \
        snprintf(PMW3610_INST_KEY_VAR(n, rest1_sample_ms), PMW3610_SETTINGS_KEY_BUF_SIZE,          \
                 "rest1_sample_ms@%s", PMW3610_INST_ID_VAR(n));                                    \
        snprintf(PMW3610_INST_KEY_VAR(n, rest2_sample_ms), PMW3610_SETTINGS_KEY_BUF_SIZE,          \
                 "rest2_sample_ms@%s", PMW3610_INST_ID_VAR(n));                                    \
        snprintf(PMW3610_INST_KEY_VAR(n, rest3_sample_ms), PMW3610_SETTINGS_KEY_BUF_SIZE,          \
                 "rest3_sample_ms@%s", PMW3610_INST_ID_VAR(n));                                    \
        snprintf(PMW3610_INST_KEY_VAR(n, report_interval_min_ms), PMW3610_SETTINGS_KEY_BUF_SIZE,   \
                 "report_interval_min_ms@%s", PMW3610_INST_ID_VAR(n));                             \
        return 0;                                                                                  \
    }                                                                                              \
    SYS_INIT(pmw3610_settings_keys_init_##n, POST_KERNEL, 0);

DT_INST_FOREACH_STATUS_OKAY(PMW3610_INIT_INST_KEYS_FN)

/* --- Apply effective settings to a device, by its own settings id ------ */

static int32_t read_int32_by_field(const char *id, const char *field, int32_t fallback) {
    char key[PMW3610_SETTINGS_KEY_BUF_SIZE];
    snprintf(key, sizeof(key), "%s@%s", field, id);

    struct zmk_custom_setting_value value;
    if (zmk_custom_setting_read_by_key(PMW3610_SETTINGS_SUBSYSTEM_ID, key, &value) != 0 ||
        value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32) {
        return fallback;
    }
    return value.int32_value;
}

static bool read_bool_by_field(const char *id, const char *field, bool fallback) {
    char key[PMW3610_SETTINGS_KEY_BUF_SIZE];
    snprintf(key, sizeof(key), "%s@%s", field, id);

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

    char id[PMW3610_SETTINGS_ID_BUF_SIZE];
    if (pmw3610_get_device_id(dev, id, sizeof(id)) != 0) {
        return;
    }

    pmw3610_set_cpi_runtime(dev, (uint32_t)read_int32_by_field(id, "cpi", 600));
    pmw3610_set_axis_flags(dev, read_bool_by_field(id, "swap_xy", false),
                           read_bool_by_field(id, "invert_x", false),
                           read_bool_by_field(id, "invert_y", false));
    pmw3610_set_force_awake(dev, read_bool_by_field(id, "force_awake", false));
    pmw3610_set_smart_algorithm(dev, read_bool_by_field(id, "smart_algorithm", true));
    pmw3610_set_run_downshift_ms(
        dev, (uint32_t)read_int32_by_field(id, "run_downshift_ms",
                                           CONFIG_PMW3610_RUN_DOWNSHIFT_TIME_MS));
    /* Apply sample times before downshift times: downshift range validation
     * in the driver depends on the *current* runtime sample time. */
    pmw3610_set_rest1_sample_ms(
        dev, (uint32_t)read_int32_by_field(id, "rest1_sample_ms", PMW3610_DEFAULT_REST1_SAMPLE_MS));
    pmw3610_set_rest2_sample_ms(
        dev, (uint32_t)read_int32_by_field(id, "rest2_sample_ms", PMW3610_DEFAULT_REST2_SAMPLE_MS));
    pmw3610_set_rest3_sample_ms(
        dev,
        (uint32_t)read_int32_by_field(id, "rest3_sample_ms", CONFIG_PMW3610_REST3_SAMPLE_TIME_MS));
    pmw3610_set_rest1_downshift_ms(
        dev, (uint32_t)read_int32_by_field(id, "rest1_downshift_ms",
                                           CONFIG_PMW3610_REST1_DOWNSHIFT_TIME_MS));
    pmw3610_set_rest2_downshift_ms(
        dev, (uint32_t)read_int32_by_field(id, "rest2_downshift_ms",
                                           CONFIG_PMW3610_REST2_DOWNSHIFT_TIME_MS));
    pmw3610_set_report_interval_min(
        dev, (uint32_t)read_int32_by_field(id, "report_interval_min_ms",
                                           CONFIG_PMW3610_REPORT_INTERVAL_MIN));
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
     * wasteful but simple and correct -- each device only ever reads keys
     * scoped to its own settings id, so re-invoking this for an unrelated
     * device is a no-op, not a correctness risk -- and settings changes
     * are rare (interactive Studio RPC use, not a hot path). */
    pmw3610_settings_apply_to_all_devices();
    return 0;
}

ZMK_LISTENER(pmw3610_settings, pmw3610_settings_changed_listener);
ZMK_SUBSCRIPTION(pmw3610_settings, zmk_custom_setting_changed);
