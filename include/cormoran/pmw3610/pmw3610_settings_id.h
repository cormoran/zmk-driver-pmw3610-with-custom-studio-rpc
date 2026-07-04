#pragma once

/**
 * @file pmw3610_settings_id.h
 *
 * @brief Deterministic per-device short id, used to build per-device custom
 * setting keys ("<param>@<id>") and reported in GetInfo so the web UI can
 * group settings by device.
 *
 * Computed independently (no shared runtime state, no init-order dependency)
 * from two purely compile-time inputs: the optional `settings-id` devicetree
 * property (used verbatim, truncated) or, when absent, a 4-hex-digit FNV-1a
 * hash of the devicetree node's full path (DT_NODE_PATH(), stable across
 * devicetree reordering of sibling nodes -- unlike a DT_INST enumeration
 * index, which shifts when sensors are added/removed/reordered).
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Max chars (excluding NUL) of a resolved settings id. */
#define PMW3610_SETTINGS_ID_MAX_LEN 8
/** Buffer size required to hold a resolved settings id, NUL included. */
#define PMW3610_SETTINGS_ID_BUF_SIZE (PMW3610_SETTINGS_ID_MAX_LEN + 1)

/** @brief Resolve a device's settings id into `out`.
 *
 * @param settings_id_prop The devicetree `settings-id` property value, or
 *   NULL/empty if the property is absent.
 * @param dt_node_path The devicetree node's full path (DT_NODE_PATH()),
 *   used to derive a fallback hash when settings_id_prop is absent. Must not
 *   be NULL.
 * @param out Output buffer, at least PMW3610_SETTINGS_ID_BUF_SIZE bytes.
 */
void pmw3610_settings_id_resolve(const char *settings_id_prop, const char *dt_node_path,
                                 char out[PMW3610_SETTINGS_ID_BUF_SIZE]);

#ifdef __cplusplus
}
#endif
