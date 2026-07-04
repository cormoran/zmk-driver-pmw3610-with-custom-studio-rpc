#include <cormoran/pmw3610/pmw3610_settings_id.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void hash_path(const char *path, char out[PMW3610_SETTINGS_ID_BUF_SIZE]) {
    /* FNV-1a, 32-bit, folded to 16 bits -- this is a stable short id, not a
     * security boundary, so a small hash + tiny collision odds (mitigated by
     * a boot-time uniqueness check in the caller) is an acceptable tradeoff
     * against staying inside PMW3610_SETTINGS_ID_MAX_LEN. */
    uint32_t hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        hash ^= *p;
        hash *= 16777619u;
    }
    uint16_t folded = (uint16_t)((hash >> 16) ^ (hash & 0xFFFF));
    snprintf(out, PMW3610_SETTINGS_ID_BUF_SIZE, "%04x", folded);
}

void pmw3610_settings_id_resolve(const char *settings_id_prop, const char *dt_node_path,
                                 char out[PMW3610_SETTINGS_ID_BUF_SIZE]) {
    if (settings_id_prop && settings_id_prop[0] != '\0') {
        strncpy(out, settings_id_prop, PMW3610_SETTINGS_ID_MAX_LEN);
        out[PMW3610_SETTINGS_ID_MAX_LEN] = '\0';
        return;
    }

    hash_path(dt_node_path, out);
}
