#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/sys/util.h>
#include <zmk/studio/custom.h>
#include <cormoran/pmw3610/pmw3610.pb.h>
#include <cormoran/pmw3610/pmw3610_api.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_rpc_custom_subsystem_meta pmw3610_feature_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(
        "http://cormoran.github.io/zmk-driver-pmw3610-with-custom-studio-rpc/"),
    // Unsecured is suggested by default to avoid unlocking in un-reliable
    // environments. NOTE: WriteRegister is a raw sensor register write --
    // a debug/tuning facility with no validation beyond the SPI transfer
    // itself. Treat it the same as flashing untrusted firmware: only use it
    // over a transport/host you trust. See README.md.
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

static bool pmw3610_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                       pb_callback_t *encode_response);

ZMK_RPC_CUSTOM_SUBSYSTEM(cormoran__pmw3610, &pmw3610_feature_meta, pmw3610_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(cormoran__pmw3610, cormoran_pmw3610_Response);

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

/* Static frame buffer + frame state for CaptureFrame/GetFrameChunk. Static
 * because the response buffer (ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER) is
 * encoded after the handler returns, and GetFrameChunk copies a slice of
 * this buffer into the (also static) response on each call -- there is no
 * per-request lifetime here, only "current frame" state. */
static uint8_t frame_buf[CONFIG_ZMK_PMW3610_STUDIO_RPC_FRAME_BUF_SIZE];
static uint32_t frame_id;
static uint16_t frame_len;
/* GetFrameChunk's response.data is bounded to 128 bytes (pmw3610.options);
 * with CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE=256 this leaves headroom for the
 * rest of the Response/CallResponse proto framing overhead. If this bound
 * or the TX buffer size ever changes, re-check that a full chunk still
 * fits (see README.md). */
#define PMW3610_FRAME_CHUNK_SIZE 128

/* A 128-byte data chunk plus the surrounding Response/CallResponse proto
 * framing (oneof tags, frame_id/offset fields, length-delimited bytes
 * field header, outer RPC envelope) must fit in one CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE
 * ring buffer -- this module's build test config
 * (tests/zmk-config/config/tester_xiao.conf) sets it to 256, well above
 * PMW3610_FRAME_CHUNK_SIZE + a generous framing allowance. If either this
 * chunk size or that Kconfig value changes, re-verify a chunk response
 * still fits (see README.md's "Frame viewer" section). */
BUILD_ASSERT(PMW3610_FRAME_CHUNK_SIZE + 64 <= CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE,
             "PMW3610_FRAME_CHUNK_SIZE leaves too little headroom in "
             "CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE for GetFrameChunkResponse framing overhead");

static void handle_capture_frame_request(const cormoran_pmw3610_CaptureFrameRequest *req,
                                         cormoran_pmw3610_Response *resp) {
    const struct device *dev = resolve_device(req->device_index, resp);
    if (!dev) {
        return;
    }

    struct pmw3610_frame_capture_params params = {
        .pixel_count =
            (uint16_t)MIN(req->pixel_count, (uint32_t)CONFIG_ZMK_PMW3610_STUDIO_RPC_FRAME_BUF_SIZE),
        .max_invalid_retries = (uint16_t)MIN(req->max_invalid_retries, (uint32_t)UINT16_MAX),
        .write_frame_grab = req->write_frame_grab,
        .frame_grab_value = (uint8_t)MIN(req->frame_grab_value, (uint32_t)0xFF),
        .write_pixel_grab_reset = !req->skip_pixel_grab_reset,
    };

    uint16_t out_count = 0;
    int err = pmw3610_capture_frame(dev, &params, frame_buf, sizeof(frame_buf), &out_count);
    if (err) {
        set_error(resp, "CaptureFrame failed: errno %d", err);
        return;
    }

    frame_id++;
    frame_len = out_count;

    cormoran_pmw3610_CaptureFrameResponse result = cormoran_pmw3610_CaptureFrameResponse_init_zero;
    result.frame_id = frame_id;
    result.pixel_count = out_count;
    result.chunk_size = PMW3610_FRAME_CHUNK_SIZE;

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

static bool pmw3610_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                       pb_callback_t *encode_response) {
    cormoran_pmw3610_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(cormoran__pmw3610, encode_response);

    cormoran_pmw3610_Request req = cormoran_pmw3610_Request_init_zero;

    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, cormoran_pmw3610_Request_fields, &req)) {
        LOG_WRN("Failed to decode pmw3610 request: %s", PB_GET_ERROR(&req_stream));
        set_error(resp, "Failed to decode request");
        return true;
    }

    switch (req.which_request_type) {
    case cormoran_pmw3610_Request_get_info_tag:
        handle_get_info_request(&req.request_type.get_info, resp);
        break;
    case cormoran_pmw3610_Request_read_diagnostics_tag:
        handle_read_diagnostics_request(&req.request_type.read_diagnostics, resp);
        break;
    case cormoran_pmw3610_Request_read_register_tag:
        handle_read_register_request(&req.request_type.read_register, resp);
        break;
    case cormoran_pmw3610_Request_write_register_tag:
        handle_write_register_request(&req.request_type.write_register, resp);
        break;
    case cormoran_pmw3610_Request_capture_frame_tag:
        handle_capture_frame_request(&req.request_type.capture_frame, resp);
        break;
    case cormoran_pmw3610_Request_get_frame_chunk_tag:
        handle_get_frame_chunk_request(&req.request_type.get_frame_chunk, resp);
        break;
    default:
        LOG_WRN("Unsupported pmw3610 request type: %d", req.which_request_type);
        set_error(resp, "Unsupported request type");
        break;
    }

    return true;
}
