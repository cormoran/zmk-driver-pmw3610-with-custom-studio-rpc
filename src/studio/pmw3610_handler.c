#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zmk/studio/custom.h>
#include <cormoran/pmw3610/pmw3610.pb.h>
#include <cormoran/pmw3610/pmw3610_request_exec.h>
#if IS_ENABLED(CONFIG_ZMK_PMW3610_SPLIT_RPC_RELAY)
#include <cormoran/pmw3610/pmw3610_relay.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_rpc_custom_subsystem_meta pmw3610_feature_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(
        "http://cormoran.github.io/zmk-driver-pmw3610-with-custom-studio-rpc/"),
    // Secured: WriteRegister is a raw sensor register write with no
    // validation beyond fitting in a byte -- effectively an arbitrary-write
    // primitive against the sensor. Security is per-subsystem, not
    // per-method (see custom_subsystem.c's call(), which checks
    // meta->security once for the whole subsystem before dispatching to the
    // handler), so this secures every method in this subsystem, including
    // GetInfo/ReadDiagnostics -- an accepted trade-off given the risk
    // WriteRegister carries. ZMK Studio must be unlocked (physical
    // `&studio_unlock` keypress in this fork -- there is no RPC/PIN unlock)
    // before any pmw3610 RPC succeeds; see README.md's "Security" section.
    .security = ZMK_STUDIO_RPC_HANDLER_SECURED,
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

/* All actual request handling (local device access for every supported
 * request kind, including frame capture/streaming) lives in
 * pmw3610_request_exec.c, shared with the split relay peripheral executor
 * (src/split/pmw3610_relay.c) -- this file only decodes the request,
 * decides local-vs-relay from its `source` field, and encodes the
 * response. */
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

    uint32_t source = pmw3610_request_get_source(&req);
    /* GetInfo{source: PMW3610_SOURCE_ALL} ("list every PMW3610 across the
     * whole keyboard") answers with local devices synchronously -- same as
     * source 0, since there is no way to enumerate connected peripherals
     * without asking them -- and additionally broadcasts to every
     * connected peripheral, each answering later as its own
     * PeripheralResponse notification (see pmw3610.proto). */
    bool is_broadcast_get_info = req.which_request_type == cormoran_pmw3610_Request_get_info_tag &&
                                 source == PMW3610_SOURCE_ALL;

    if (source == 0 || is_broadcast_get_info) {
        if (!pmw3610_request_exec_handle(&req, resp)) {
            LOG_WRN("Unsupported pmw3610 request type: %d", req.which_request_type);
            set_error(resp, "Unsupported request type");
        }
#if IS_ENABLED(CONFIG_ZMK_PMW3610_SPLIT_RPC_RELAY)
        if (is_broadcast_get_info &&
            resp->which_response_type == cormoran_pmw3610_Response_get_info_tag) {
            resp->response_type.get_info.relay_request_id = pmw3610_relay_broadcast_request(&req);
        }
#endif
    } else {
#if IS_ENABLED(CONFIG_ZMK_PMW3610_SPLIT_RPC_RELAY)
        pmw3610_relay_dispatch_request(source, &req, resp);
#else
        set_error(resp, "source %u requested but CONFIG_ZMK_PMW3610_SPLIT_RPC_RELAY is not enabled",
                  source);
#endif
    }

    return true;
}
