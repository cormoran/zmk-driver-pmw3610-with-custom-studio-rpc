#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zmk/event_manager.h>

#include <cormoran/pmw3610/pmw3610.pb.h>
#include <cormoran/pmw3610/pmw3610_api.h>
#include <cormoran/pmw3610/pmw3610_request_exec.h>

/* Frame streaming needs to notify the Studio client of each captured chunk.
 * On the central (or a non-split build), that means raising a Studio custom
 * notification directly; a split peripheral has no Studio of its own, so it
 * instead relays the (already fully-formed) Notification to the central via
 * pmw3610_relay_notify() (src/split/pmw3610_relay.c), which re-raises it
 * there. CONFIG_ZMK_SPLIT_ROLE_CENTRAL does not exist on a non-split build,
 * so IS_ENABLED() alone would treat it as "peripheral" -- gate on
 * CONFIG_ZMK_SPLIT too. */
#define PMW3610_IS_SPLIT_PERIPHERAL                                                                \
    (IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

#if !PMW3610_IS_SPLIT_PERIPHERAL
#include <zmk/studio/custom.h>
#include <zmk/studio/core.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_PMW3610_SPLIT_RPC_RELAY)
#include <cormoran/pmw3610/pmw3610_relay.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static void set_error(cormoran_pmw3610_Response *resp, const char *fmt, ...) {
    cormoran_pmw3610_ErrorResponse err = cormoran_pmw3610_ErrorResponse_init_zero;

    va_list args;
    va_start(args, fmt);
    vsnprintf(err.message, sizeof(err.message), fmt, args);
    va_end(args);

    resp->which_response_type = cormoran_pmw3610_Response_error_tag;
    resp->response_type.error = err;
}

/* Resolve device_index into a device pointer, or set an ErrorResponse and
 * return NULL on an out-of-range index. */
static const struct device *resolve_device(uint32_t device_index, cormoran_pmw3610_Response *resp) {
    size_t device_count = pmw3610_device_count();
    if (device_index >= device_count) {
        set_error(resp, "device_index %u out of range (have %u device(s))", device_index,
                  (unsigned int)device_count);
        return NULL;
    }
    return pmw3610_get_device(device_index);
}

static void fill_runtime_config(cormoran_pmw3610_RuntimeConfig *out,
                                const struct pmw3610_runtime_config *rt) {
    out->cpi = rt->cpi;
    out->swap_xy = rt->swap_xy;
    out->invert_x = rt->invert_x;
    out->invert_y = rt->invert_y;
    out->force_awake = rt->force_awake;
    out->smart_algorithm = rt->smart_algorithm;
    out->run_downshift_ms = rt->run_downshift_ms;
    out->rest1_downshift_ms = rt->rest1_downshift_ms;
    out->rest2_downshift_ms = rt->rest2_downshift_ms;
    out->rest1_sample_ms = rt->rest1_sample_ms;
    out->rest2_sample_ms = rt->rest2_sample_ms;
    out->rest3_sample_ms = rt->rest3_sample_ms;
    out->report_interval_min_ms = rt->report_interval_min_ms;
}

static void handle_get_info_request(const cormoran_pmw3610_GetInfoRequest *req,
                                    cormoran_pmw3610_Response *resp) {
    ARG_UNUSED(req);

    cormoran_pmw3610_GetInfoResponse result = cormoran_pmw3610_GetInfoResponse_init_zero;

    size_t device_count = pmw3610_device_count();
    for (size_t i = 0; i < device_count && result.devices_count < ARRAY_SIZE(result.devices); i++) {
        const struct device *dev = pmw3610_get_device(i);

        cormoran_pmw3610_DeviceInfo *info = &result.devices[result.devices_count];
        info->ready = pmw3610_is_ready(dev);
        info->init_error = pmw3610_get_init_error(dev);
        info->device_index = (uint32_t)i;
        if (pmw3610_get_device_id(dev, info->settings_id, sizeof(info->settings_id)) != 0) {
            info->settings_id[0] = '\0';
        }

        uint8_t product_id = 0;
        if (pmw3610_read_register(dev, 0x00, &product_id) == 0) {
            info->product_id = product_id;
        }
        uint8_t revision_id = 0;
        if (pmw3610_read_register(dev, 0x01, &revision_id) == 0) {
            info->revision_id = revision_id;
        }

        struct pmw3610_runtime_config rt;
        if (pmw3610_get_runtime_config(dev, &rt) == 0) {
            // nanopb requires has_<field> = true to encode a sub-message.
            info->has_runtime_config = true;
            fill_runtime_config(&info->runtime_config, &rt);
        }

        result.devices_count++;
    }

    resp->which_response_type = cormoran_pmw3610_Response_get_info_tag;
    resp->response_type.get_info = result;
}

static void handle_read_diagnostics_request(const cormoran_pmw3610_ReadDiagnosticsRequest *req,
                                            cormoran_pmw3610_Response *resp) {
    const struct device *dev = resolve_device(req->device_index, resp);
    if (!dev) {
        return;
    }

    struct pmw3610_diagnostics diag;
    int err = pmw3610_read_diagnostics(dev, &diag);
    if (err) {
        set_error(resp, "ReadDiagnostics failed: errno %d", err);
        return;
    }

    cormoran_pmw3610_ReadDiagnosticsResponse result =
        cormoran_pmw3610_ReadDiagnosticsResponse_init_zero;
    result.squal = diag.squal;
    result.shutter = diag.shutter;
    result.pix_max = diag.pix_max;
    result.pix_avg = diag.pix_avg;
    result.pix_min = diag.pix_min;

    resp->which_response_type = cormoran_pmw3610_Response_read_diagnostics_tag;
    resp->response_type.read_diagnostics = result;
}

static void handle_read_register_request(const cormoran_pmw3610_ReadRegisterRequest *req,
                                         cormoran_pmw3610_Response *resp) {
    const struct device *dev = resolve_device(req->device_index, resp);
    if (!dev) {
        return;
    }

    if (req->address > 0xFF) {
        set_error(resp, "address 0x%x out of range (must be a byte)", (unsigned int)req->address);
        return;
    }

    uint8_t value = 0;
    int err = pmw3610_read_register(dev, (uint8_t)req->address, &value);
    if (err) {
        set_error(resp, "ReadRegister(0x%02x) failed: errno %d", (unsigned int)req->address, err);
        return;
    }

    cormoran_pmw3610_ReadRegisterResponse result = cormoran_pmw3610_ReadRegisterResponse_init_zero;
    result.value = value;

    resp->which_response_type = cormoran_pmw3610_Response_read_register_tag;
    resp->response_type.read_register = result;
}

static void handle_write_register_request(const cormoran_pmw3610_WriteRegisterRequest *req,
                                          cormoran_pmw3610_Response *resp) {
    const struct device *dev = resolve_device(req->device_index, resp);
    if (!dev) {
        return;
    }

    if (req->address > 0xFF) {
        set_error(resp, "address 0x%x out of range (must be a byte)", (unsigned int)req->address);
        return;
    }
    if (req->value > 0xFF) {
        set_error(resp, "value 0x%x out of range (must be a byte)", (unsigned int)req->value);
        return;
    }

    int err = pmw3610_write_register(dev, (uint8_t)req->address, (uint8_t)req->value);
    if (err) {
        set_error(resp, "WriteRegister(0x%02x, 0x%02x) failed: errno %d",
                  (unsigned int)req->address, (unsigned int)req->value, err);
        return;
    }

    cormoran_pmw3610_WriteRegisterResponse result =
        cormoran_pmw3610_WriteRegisterResponse_init_zero;

    resp->which_response_type = cormoran_pmw3610_Response_write_register_tag;
    resp->response_type.write_register = result;
}

/* --- Frame capture/streaming (CaptureFrame/GetFrameChunk/SetFrameStream) --- */
/*
 * Moved here (from src/studio/pmw3610_handler.c) so a split peripheral --
 * which has no Studio of its own, so cannot host pmw3610_handler.c's
 * ZMK_RPC_CUSTOM_SUBSYSTEM registration -- can still execute these against
 * its own local devices when relayed from the central (see DESIGN.md Phase
 * F, "F-d"). Static frame buffer/frame_id/streaming state is process-wide
 * (one instance per firmware image), same as before the move.
 */

/* Static because the response buffer (ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER,
 * central only) is encoded after the handler returns, and GetFrameChunk
 * copies a slice of this buffer into the (also static) response on each
 * call -- there is no per-request lifetime here, only "current frame"
 * state. */
static uint8_t frame_buf[CONFIG_ZMK_PMW3610_STUDIO_RPC_FRAME_BUF_SIZE];
static uint32_t frame_id;
static uint16_t frame_len;
/* GetFrameChunk's response.data is bounded to 128 bytes (pmw3610.options);
 * with CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE=256 this leaves headroom for the
 * rest of the Response/CallResponse proto framing overhead. If this bound
 * or the TX buffer size ever changes, re-check that a full chunk still
 * fits (see README.md). */
#define PMW3610_FRAME_CHUNK_SIZE 128

#if !PMW3610_IS_SPLIT_PERIPHERAL
/* A 128-byte data chunk plus the surrounding Response/CallResponse proto
 * framing (oneof tags, frame_id/offset fields, length-delimited bytes
 * field header, outer RPC envelope) must fit in one
 * CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE ring buffer -- only meaningful when
 * this half is actually serving the Studio RPC transport (central/non-split;
 * CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE does not even exist on a split
 * peripheral, which never encodes a CallResponse itself). This module's
 * build test config (tests/zmk-config/config/tester_xiao.conf) sets it to
 * 256, well above PMW3610_FRAME_CHUNK_SIZE + a generous framing allowance.
 * If either this chunk size or that Kconfig value changes, re-verify a
 * chunk response still fits (see README.md's "Frame viewer" section). */
BUILD_ASSERT(PMW3610_FRAME_CHUNK_SIZE + 64 <= CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE,
             "PMW3610_FRAME_CHUNK_SIZE leaves too little headroom in "
             "CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE for GetFrameChunkResponse framing overhead");
#endif

/* --- Frame streaming (SetFrameStream) notification plumbing ------------ */

#if !PMW3610_IS_SPLIT_PERIPHERAL

/* Resolve this subsystem's runtime index (its position in the
 * STRUCT_SECTION_ITERABLE(zmk_rpc_custom_subsystem, ...) section), needed
 * to raise a custom Studio notification tagged with the right
 * subsystem_index. Pattern copied from custom_settings_handler.c's
 * custom_subsystem_index_for_identifier()/custom_subsystem_index(). */
#define PMW3610_SUBSYSTEM_IDENTIFIER_STRING "cormoran__pmw3610"

static int custom_subsystem_index_for_identifier(const char *identifier, uint32_t *index) {
    if (!identifier) {
        return -ENOENT;
    }

    size_t subsystem_count;
    STRUCT_SECTION_COUNT(zmk_rpc_custom_subsystem, &subsystem_count);

    for (size_t i = 0; i < subsystem_count; i++) {
        struct zmk_rpc_custom_subsystem *custom_subsys;
        STRUCT_SECTION_GET(zmk_rpc_custom_subsystem, i, &custom_subsys);
        if (strcmp(custom_subsys->identifier, identifier) == 0) {
            *index = i;
            return 0;
        }
    }

    return -ENOENT;
}

static int custom_subsystem_index(void) {
    static int cached_index = -1;
    if (cached_index >= 0) {
        return cached_index;
    }

    uint32_t index;
    int ret = custom_subsystem_index_for_identifier(PMW3610_SUBSYSTEM_IDENTIFIER_STRING, &index);
    if (ret < 0) {
        return ret;
    }

    cached_index = (int)index;
    return cached_index;
}

static bool encode_frame_stream_notification_payload(pb_ostream_t *stream, const pb_field_t *field,
                                                     void *const *arg) {
    const cormoran_pmw3610_Notification *notification = (const cormoran_pmw3610_Notification *)*arg;
    return zmk_rpc_custom_subsystem_encode_response_payload(
        stream, field, cormoran_pmw3610_Notification_fields, notification);
}

#endif // !PMW3610_IS_SPLIT_PERIPHERAL

static K_MUTEX_DEFINE(frame_stream_notification_lock);
static cormoran_pmw3610_Notification frame_stream_notification;

/* Raise/relay one FrameStreamChunk notification. Synchronous (ZMK's event
 * manager dispatches notifications/relay events synchronously), so it is
 * safe to reuse a single static buffer/mutex across calls -- matches
 * custom_settings_handler.c's raise_encoded_studio_notification(). */
static int notify_frame_stream_chunk(uint32_t stream_frame_id, uint32_t offset, const uint8_t *data,
                                     uint32_t len, uint32_t total_size, bool complete) {
#if !PMW3610_IS_SPLIT_PERIPHERAL
    int index = custom_subsystem_index();
    if (index < 0) {
        return index;
    }
#endif

    k_mutex_lock(&frame_stream_notification_lock, K_FOREVER);

    frame_stream_notification =
        (cormoran_pmw3610_Notification)cormoran_pmw3610_Notification_init_zero;
    frame_stream_notification.which_notification_type =
        cormoran_pmw3610_Notification_frame_stream_chunk_tag;
    cormoran_pmw3610_FrameStreamChunk *chunk =
        &frame_stream_notification.notification_type.frame_stream_chunk;
    chunk->frame_id = stream_frame_id;
    chunk->offset = offset;
    chunk->data.size = MIN(len, sizeof(chunk->data.bytes));
    memcpy(chunk->data.bytes, data, chunk->data.size);
    chunk->total_size = total_size;
    chunk->complete = complete;
    /* `source` is filled in by the central when re-raising a chunk relayed
     * up from a peripheral (src/split/pmw3610_relay.c) -- 0 here always
     * means "streamed locally", correct whether this code is running on
     * the central (local stream) or a peripheral (source is meaningless
     * from the peripheral's own point of view; the central fills it in). */
    chunk->source = 0;

    int ret;
#if PMW3610_IS_SPLIT_PERIPHERAL
    pmw3610_relay_notify(&frame_stream_notification);
    ret = 0;
#else
    pb_callback_t payload = {
        .funcs.encode = encode_frame_stream_notification_payload,
        .arg = (void *)&frame_stream_notification,
    };
    ret = raise_zmk_studio_custom_notification((struct zmk_studio_custom_notification){
        .subsystem_index = (uint8_t)index,
        .encode_payload = payload,
    });
#endif

    k_mutex_unlock(&frame_stream_notification_lock);
    return ret;
}

/* Streaming state, guarded implicitly by running only on the system
 * workqueue (frame_stream_work) and being set from the RPC/relay-executor
 * thread (which ZMK serializes one-request-at-a-time) -- no separate lock
 * needed since the only cross-thread field is the bool `frame_stream_active`,
 * which is only ever read-modify-written as a whole. */
static bool frame_stream_active;
static uint32_t frame_stream_device_index;
static uint16_t frame_stream_pixel_count;
static uint16_t frame_stream_max_invalid_retries;

static void frame_stream_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(frame_stream_work, frame_stream_work_handler);

static void frame_stream_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!frame_stream_active) {
        return;
    }

    /* Defensive re-check: device_index was validated when streaming was
     * enabled and cannot become invalid at runtime (device count is fixed
     * at compile time), but re-validate anyway rather than trust stale
     * state across an unbounded number of loop iterations. */
    size_t device_count = pmw3610_device_count();
    if (frame_stream_device_index >= device_count) {
        LOG_WRN("Frame stream: device_index %u out of range (have %u device(s)), stopping",
                (unsigned int)frame_stream_device_index, (unsigned int)device_count);
        frame_stream_active = false;
        return;
    }
    const struct device *dev = pmw3610_get_device(frame_stream_device_index);

    struct pmw3610_frame_capture_params params = {
        .pixel_count = frame_stream_pixel_count,
        .max_invalid_retries = frame_stream_max_invalid_retries,
    };

    struct pmw3610_frame_capture_result capture = {0};
    int err = pmw3610_capture_frame(dev, &params, frame_buf, sizeof(frame_buf), &capture);
    if (err) {
        LOG_WRN("Frame stream: CaptureFrame failed: errno %d", err);
    } else {
        frame_id++;
        frame_len = capture.pixel_count;

        for (uint32_t offset = 0; offset < capture.pixel_count;
             offset += PMW3610_FRAME_CHUNK_SIZE) {
            uint32_t remaining = capture.pixel_count - offset;
            uint32_t chunk_len = MIN(remaining, PMW3610_FRAME_CHUNK_SIZE);
            int notify_err =
                notify_frame_stream_chunk(frame_id, offset, &frame_buf[offset], chunk_len,
                                          capture.pixel_count, capture.complete);
            if (notify_err) {
                LOG_WRN("Frame stream: failed to raise/relay notification: errno %d", notify_err);
                break;
            }
        }
    }

    /* Back-to-back reschedule with no artificial delay: pmw3610_capture_frame()
     * blocks for ~2s per 484-pixel frame (measured in Phase D), which already
     * paces the loop far below any transport throughput concern. Hardware
     * testing (Phase E validation) should confirm no transport overrun
     * before adding an inter-frame delay here. */
    if (frame_stream_active) {
        k_work_reschedule(&frame_stream_work, K_NO_WAIT);
    }
}

static void handle_set_frame_stream_request(const cormoran_pmw3610_SetFrameStreamRequest *req,
                                            cormoran_pmw3610_Response *resp) {
    if (req->enable) {
        const struct device *dev = resolve_device(req->device_index, resp);
        if (!dev) {
            return;
        }

        frame_stream_device_index = req->device_index;
        frame_stream_pixel_count =
            (uint16_t)MIN(req->pixel_count, (uint32_t)CONFIG_ZMK_PMW3610_STUDIO_RPC_FRAME_BUF_SIZE);
        frame_stream_max_invalid_retries =
            (uint16_t)MIN(req->max_invalid_retries, (uint32_t)UINT16_MAX);

        bool already_active = frame_stream_active;
        frame_stream_active = true;
        if (!already_active) {
            k_work_reschedule(&frame_stream_work, K_NO_WAIT);
        }
    } else {
        frame_stream_active = false;
    }

    cormoran_pmw3610_SetFrameStreamResponse result =
        cormoran_pmw3610_SetFrameStreamResponse_init_zero;
    result.streaming = frame_stream_active;

    resp->which_response_type = cormoran_pmw3610_Response_set_frame_stream_tag;
    resp->response_type.set_frame_stream = result;
}

#if !PMW3610_IS_SPLIT_PERIPHERAL
/* Force-stop any active stream when Studio locks -- notifications are not
 * gated by lock state at the transport level (custom_event_mapper() has no
 * lock check, unlike the RPC call() path), so without this a stream started
 * while unlocked would keep emitting notifications after a later auto-lock.
 * Central/non-split only: Studio lock state does not exist on a split
 * peripheral. */
static int on_studio_core_lock_state_changed(const zmk_event_t *eh) {
    struct zmk_studio_core_lock_state_changed *ev = as_zmk_studio_core_lock_state_changed(eh);
    if (!ev) {
        return 0;
    }

    if (ev->state == ZMK_STUDIO_CORE_LOCK_STATE_LOCKED) {
        frame_stream_active = false;
    }

    return 0;
}

ZMK_LISTENER(zmk_pmw3610_studio_lock_listener, on_studio_core_lock_state_changed);
ZMK_SUBSCRIPTION(zmk_pmw3610_studio_lock_listener, zmk_studio_core_lock_state_changed);
#endif // !PMW3610_IS_SPLIT_PERIPHERAL

#if PMW3610_IS_SPLIT_PERIPHERAL
/* Force-stop any active stream when the split link drops -- a stream
 * relaying chunks up would otherwise keep running (and failing to relay,
 * since zmk_split_peripheral_report_event() has nowhere to send to) forever
 * once disconnected from the central. */
#include <zmk/events/split_peripheral_status_changed.h>

static int on_split_peripheral_status_changed(const zmk_event_t *eh) {
    struct zmk_split_peripheral_status_changed *ev = as_zmk_split_peripheral_status_changed(eh);
    if (!ev) {
        return 0;
    }

    if (!ev->connected) {
        frame_stream_active = false;
    }

    return 0;
}

ZMK_LISTENER(zmk_pmw3610_split_status_listener, on_split_peripheral_status_changed);
ZMK_SUBSCRIPTION(zmk_pmw3610_split_status_listener, zmk_split_peripheral_status_changed);
#endif // PMW3610_IS_SPLIT_PERIPHERAL

static void handle_capture_frame_request(const cormoran_pmw3610_CaptureFrameRequest *req,
                                         cormoran_pmw3610_Response *resp) {
    if (frame_stream_active) {
        set_error(resp, "frame streaming is active; call SetFrameStream(enable=false) first");
        return;
    }

    const struct device *dev = resolve_device(req->device_index, resp);
    if (!dev) {
        return;
    }

    struct pmw3610_frame_capture_params params = {
        .pixel_count =
            (uint16_t)MIN(req->pixel_count, (uint32_t)CONFIG_ZMK_PMW3610_STUDIO_RPC_FRAME_BUF_SIZE),
        /* Per-pixel 10ms-wait retry budget; the driver clamps to its own
         * 1..100 range (0 = driver default). */
        .max_invalid_retries = (uint16_t)MIN(req->max_invalid_retries, (uint32_t)UINT16_MAX),
    };

    struct pmw3610_frame_capture_result capture = {0};
    int err = pmw3610_capture_frame(dev, &params, frame_buf, sizeof(frame_buf), &capture);
    if (err) {
        set_error(resp, "CaptureFrame failed: errno %d", err);
        return;
    }

    frame_id++;
    frame_len = capture.pixel_count;

    cormoran_pmw3610_CaptureFrameResponse result = cormoran_pmw3610_CaptureFrameResponse_init_zero;
    result.frame_id = frame_id;
    result.pixel_count = capture.pixel_count;
    result.chunk_size = PMW3610_FRAME_CHUNK_SIZE;
    result.complete = capture.complete;
    result.duration_ms = capture.duration_ms;

    resp->which_response_type = cormoran_pmw3610_Response_capture_frame_tag;
    resp->response_type.capture_frame = result;
}

static void handle_get_frame_chunk_request(const cormoran_pmw3610_GetFrameChunkRequest *req,
                                           cormoran_pmw3610_Response *resp) {
    if (frame_id == 0 || req->frame_id != frame_id) {
        set_error(resp, "frame_id %u not found (current frame_id %u)", (unsigned int)req->frame_id,
                  (unsigned int)frame_id);
        return;
    }
    if (req->offset >= frame_len) {
        set_error(resp, "offset %u out of range (frame length %u)", (unsigned int)req->offset,
                  (unsigned int)frame_len);
        return;
    }

    uint32_t remaining = frame_len - req->offset;
    uint32_t chunk = MIN(remaining, PMW3610_FRAME_CHUNK_SIZE);

    cormoran_pmw3610_GetFrameChunkResponse result =
        cormoran_pmw3610_GetFrameChunkResponse_init_zero;
    result.frame_id = frame_id;
    result.offset = req->offset;
    result.data.size = chunk;
    memcpy(result.data.bytes, &frame_buf[req->offset], chunk);

    resp->which_response_type = cormoran_pmw3610_Response_get_frame_chunk_tag;
    resp->response_type.get_frame_chunk = result;
}

bool pmw3610_request_exec_handle(const cormoran_pmw3610_Request *req,
                                 cormoran_pmw3610_Response *resp) {
    switch (req->which_request_type) {
    case cormoran_pmw3610_Request_get_info_tag:
        handle_get_info_request(&req->request_type.get_info, resp);
        return true;
    case cormoran_pmw3610_Request_read_diagnostics_tag:
        handle_read_diagnostics_request(&req->request_type.read_diagnostics, resp);
        return true;
    case cormoran_pmw3610_Request_read_register_tag:
        handle_read_register_request(&req->request_type.read_register, resp);
        return true;
    case cormoran_pmw3610_Request_write_register_tag:
        handle_write_register_request(&req->request_type.write_register, resp);
        return true;
    case cormoran_pmw3610_Request_capture_frame_tag:
        handle_capture_frame_request(&req->request_type.capture_frame, resp);
        return true;
    case cormoran_pmw3610_Request_get_frame_chunk_tag:
        handle_get_frame_chunk_request(&req->request_type.get_frame_chunk, resp);
        return true;
    case cormoran_pmw3610_Request_set_frame_stream_tag:
        handle_set_frame_stream_request(&req->request_type.set_frame_stream, resp);
        return true;
    default:
        return false;
    }
}

uint32_t pmw3610_request_get_source(const cormoran_pmw3610_Request *req) {
    switch (req->which_request_type) {
    case cormoran_pmw3610_Request_get_info_tag:
        return req->request_type.get_info.source;
    case cormoran_pmw3610_Request_read_diagnostics_tag:
        return req->request_type.read_diagnostics.source;
    case cormoran_pmw3610_Request_read_register_tag:
        return req->request_type.read_register.source;
    case cormoran_pmw3610_Request_write_register_tag:
        return req->request_type.write_register.source;
    case cormoran_pmw3610_Request_capture_frame_tag:
        return req->request_type.capture_frame.source;
    case cormoran_pmw3610_Request_get_frame_chunk_tag:
        return req->request_type.get_frame_chunk.source;
    case cormoran_pmw3610_Request_set_frame_stream_tag:
        return req->request_type.set_frame_stream.source;
    default:
        return 0;
    }
}
