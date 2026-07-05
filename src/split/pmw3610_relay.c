/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file pmw3610_relay.c
 *
 * @brief Split relay bridge for the `cormoran.pmw3610` Studio RPC subsystem
 * (CONFIG_ZMK_PMW3610_SPLIT_RPC_RELAY, see DESIGN.md Phase F) -- enabled on
 * BOTH halves of a split keyboard:
 *
 *  - Peripheral role: receives a relayed `RelayRequest` (ZMK split relay
 *    event, identifier "pmq"), executes it against this half's own local
 *    PMW3610 devices via pmw3610_request_exec_handle(), and relays a
 *    `RelayResponse` back (identifier "pmp").
 *  - Central role: pmw3610_relay_dispatch_request() (called from
 *    src/studio/pmw3610_handler.c for any request whose `source` is
 *    nonzero) relays the request out and immediately returns a
 *    DeferredResponse; when the matching `RelayResponse` relays back in, it
 *    is re-raised as a `PeripheralResponse` Studio notification (the same
 *    "custom notification" mechanism used by SetFrameStream's
 *    FrameStreamChunk).
 *
 * Pattern (event struct shape, relay macros, subsystem-index lookup,
 * static-buffer notification encoding) copied from
 * zmk-feature-custom-settings' own custom_settings_handler.c, the reference
 * implementation of this exact bridge for its own RPC surface.
 *
 * Caveat: CONFIG_ZMK_SPLIT_RELAY_EVENT broadcasts a central-to-peripheral
 * relay event to every connected peripheral, not to one addressed
 * peripheral -- so with more than one peripheral, every peripheral executes
 * every relayed request (each correctly tagged with its own source on the
 * way back, via ZMK_RELAY_EVENT_HANDLE's `source_field_name` rewrite). This
 * is fine for the common single-peripheral split; see
 * CONFIG_ZMK_PMW3610_SPLIT_RPC_RELAY's Kconfig help.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zmk/event_manager.h>

#include <cormoran/pmw3610/pmw3610.pb.h>
#include <cormoran/pmw3610/pmw3610_relay.h>
#include <cormoran/pmw3610/pmw3610_request_exec.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/studio/custom.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Matches the identifier stringified by ZMK_RPC_CUSTOM_SUBSYSTEM(cormoran__pmw3610, ...)
 * in pmw3610_handler.c -- kept as a separate copy here (not shared via a
 * header) since it is only ever needed as a literal string for the
 * subsystem-index lookup below. */
#define PMW3610_SUBSYSTEM_IDENTIFIER_STRING "cormoran__pmw3610"

/* nanopb generates a static worst-case encoded size for every message here
 * since every bytes/string/repeated field in pmw3610.proto has an explicit
 * max_size/max_count (see pmw3610.options) -- used to size the relay event
 * payload buffers below exactly, instead of guessing a constant. */
#define PMW3610_RELAY_REQUEST_PAYLOAD_MAX_SIZE cormoran_pmw3610_RelayRequest_size
#define PMW3610_RELAY_RESPONSE_PAYLOAD_MAX_SIZE cormoran_pmw3610_RelayResponse_size
#define PMW3610_RELAY_NOTIFICATION_PAYLOAD_MAX_SIZE cormoran_pmw3610_Notification_size

struct zmk_pmw3610_relay_request {
    uint8_t source;
    uint16_t size;
    uint8_t payload[PMW3610_RELAY_REQUEST_PAYLOAD_MAX_SIZE];
};

struct zmk_pmw3610_relay_response {
    uint8_t source;
    uint16_t size;
    uint8_t payload[PMW3610_RELAY_RESPONSE_PAYLOAD_MAX_SIZE];
};

/* Peripheral -> central only (one-directional, no response expected) --
 * carries an already fully-formed Notification (currently only
 * FrameStreamChunk) for pmw3610_relay_notify(). */
struct zmk_pmw3610_relay_notification {
    uint8_t source;
    uint16_t size;
    uint8_t payload[PMW3610_RELAY_NOTIFICATION_PAYLOAD_MAX_SIZE];
};

BUILD_ASSERT(sizeof(struct zmk_pmw3610_relay_request) <= CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN,
             "CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN is too small for the pmw3610 relay request "
             "payload -- raise it (see DESIGN.md Phase F / Kconfig help for "
             "ZMK_PMW3610_SPLIT_RPC_RELAY)");
BUILD_ASSERT(sizeof(struct zmk_pmw3610_relay_response) <= CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN,
             "CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN is too small for the pmw3610 relay response "
             "payload -- raise it (see DESIGN.md Phase F / Kconfig help for "
             "ZMK_PMW3610_SPLIT_RPC_RELAY)");
BUILD_ASSERT(sizeof(struct zmk_pmw3610_relay_notification) <= CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN,
             "CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN is too small for the pmw3610 relay "
             "notification payload -- raise it (see DESIGN.md Phase F / Kconfig help for "
             "ZMK_PMW3610_SPLIT_RPC_RELAY)");
/* Hard transport ceiling, independent of CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN:
 * struct relay_event_header (zmk/split/transport/types.h) encodes a relayed
 * event's data size in a single `uint8_t event_data_size` wire field, so no
 * relayed event can ever exceed 255 bytes regardless of how high
 * CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN is set. If either of pmw3610's own
 * relay structs exceeds that, raising the Kconfig value would not help --
 * pmw3610.options' GetInfoResponse.devices max_count must come down instead. */
BUILD_ASSERT(sizeof(struct zmk_pmw3610_relay_request) <= 255,
             "the pmw3610 relay request payload exceeds the split relay transport's 255-byte "
             "hard ceiling (relay_event_header.event_data_size is a uint8_t) -- reduce "
             "pmw3610.options' GetInfoResponse.devices max_count");
BUILD_ASSERT(sizeof(struct zmk_pmw3610_relay_response) <= 255,
             "the pmw3610 relay response payload exceeds the split relay transport's 255-byte "
             "hard ceiling (relay_event_header.event_data_size is a uint8_t) -- reduce "
             "pmw3610.options' GetInfoResponse.devices max_count");
BUILD_ASSERT(sizeof(struct zmk_pmw3610_relay_notification) <= 255,
             "the pmw3610 relay notification payload exceeds the split relay transport's "
             "255-byte hard ceiling (relay_event_header.event_data_size is a uint8_t)");

ZMK_EVENT_DECLARE(zmk_pmw3610_relay_request);
ZMK_EVENT_DECLARE(zmk_pmw3610_relay_response);
ZMK_EVENT_DECLARE(zmk_pmw3610_relay_notification);
ZMK_EVENT_IMPL(zmk_pmw3610_relay_request);
ZMK_EVENT_IMPL(zmk_pmw3610_relay_response);
ZMK_EVENT_IMPL(zmk_pmw3610_relay_notification);

ZMK_RELAY_EVENT_HANDLE(zmk_pmw3610_relay_request, pmq, source);
ZMK_RELAY_EVENT_HANDLE(zmk_pmw3610_relay_response, pmp, source);
ZMK_RELAY_EVENT_HANDLE(zmk_pmw3610_relay_notification, pmn, source);
ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(zmk_pmw3610_relay_request, pmq, source);
ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL(zmk_pmw3610_relay_response, pmp, source);
ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL(zmk_pmw3610_relay_notification, pmn, source);

/* --- Peripheral role: execute a relayed request, relay the response back --- */

/* Decodes `payload`, executes it via pmw3610_request_exec_handle() against
 * this half's local devices, and fills `out_resp` (always -- an
 * unsupported/undecodable request produces an ErrorResponse, not a
 * function failure, matching this module's existing "never crash on a bad
 * request" style). Exposed as its own step (rather than inlined into the
 * event listener below) so the split-relay self-test can exercise it
 * directly without needing a real relay event. */
static int pmw3610_relay_exec_request(const uint8_t *payload, size_t size,
                                      cormoran_pmw3610_RelayResponse *out_resp) {
    cormoran_pmw3610_RelayRequest relay_req = cormoran_pmw3610_RelayRequest_init_zero;
    pb_istream_t istream = pb_istream_from_buffer(payload, size);
    if (!pb_decode(&istream, cormoran_pmw3610_RelayRequest_fields, &relay_req)) {
        LOG_WRN("Failed to decode pmw3610 relay request: %s", PB_GET_ERROR(&istream));
        return -EINVAL;
    }

    *out_resp = (cormoran_pmw3610_RelayResponse)cormoran_pmw3610_RelayResponse_init_zero;
    out_resp->request_id = relay_req.request_id;
    out_resp->has_response = true;

    if (!relay_req.has_request ||
        !pmw3610_request_exec_handle(&relay_req.request, &out_resp->response)) {
        cormoran_pmw3610_ErrorResponse err = cormoran_pmw3610_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message),
                 "unsupported or missing relayed pmw3610 request");
        out_resp->response.which_response_type = cormoran_pmw3610_Response_error_tag;
        out_resp->response.response_type.error = err;
    }

    return 0;
}

#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

static int on_pmw3610_relay_request(const zmk_event_t *eh) {
    const struct zmk_pmw3610_relay_request *ev = as_zmk_pmw3610_relay_request(eh);
    if (!ev) {
        return 0;
    }

    cormoran_pmw3610_RelayResponse relay_resp;
    if (pmw3610_relay_exec_request(ev->payload, ev->size, &relay_resp) < 0) {
        return 0;
    }

    struct zmk_pmw3610_relay_response resp_event = {.source = ZMK_RELAY_EVENT_SOURCE_SELF};
    pb_ostream_t ostream = pb_ostream_from_buffer(resp_event.payload, sizeof(resp_event.payload));
    if (!pb_encode(&ostream, cormoran_pmw3610_RelayResponse_fields, &relay_resp)) {
        LOG_WRN("Failed to encode pmw3610 relay response: %s", PB_GET_ERROR(&ostream));
        return 0;
    }
    resp_event.size = (uint16_t)ostream.bytes_written;

    raise_zmk_pmw3610_relay_response(resp_event);
    return 0;
}

ZMK_LISTENER(pmw3610_relay_request_exec, on_pmw3610_relay_request);
ZMK_SUBSCRIPTION(pmw3610_relay_request_exec, zmk_pmw3610_relay_request);

void pmw3610_relay_notify(const cormoran_pmw3610_Notification *notification) {
    struct zmk_pmw3610_relay_notification event = {.source = ZMK_RELAY_EVENT_SOURCE_SELF};
    pb_ostream_t ostream = pb_ostream_from_buffer(event.payload, sizeof(event.payload));
    if (!pb_encode(&ostream, cormoran_pmw3610_Notification_fields, notification)) {
        LOG_WRN("Failed to encode pmw3610 relay notification: %s", PB_GET_ERROR(&ostream));
        return;
    }
    event.size = (uint16_t)ostream.bytes_written;

    raise_zmk_pmw3610_relay_notification(event);
}

#endif // !CONFIG_ZMK_SPLIT_ROLE_CENTRAL

/* --- Central role: dispatch a request out, turn a relayed response into a notification --- */

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

/* Resolve this subsystem's runtime index, needed to raise a custom Studio
 * notification tagged with the right subsystem_index. Pattern copied from
 * pmw3610_handler.c's custom_subsystem_index()/_for_identifier() (itself
 * copied from custom_settings_handler.c) -- kept as its own copy here since
 * this file may be linked without pmw3610_handler.c's internals being
 * exposed. */
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

static uint32_t next_relay_request_id = 1;

void pmw3610_relay_dispatch_request(uint32_t source, const cormoran_pmw3610_Request *req,
                                    cormoran_pmw3610_Response *resp) {
    ARG_UNUSED(source); /* Transport broadcasts to every peripheral -- see file doc comment. */

    uint32_t request_id = next_relay_request_id++;

    cormoran_pmw3610_RelayRequest relay_req = cormoran_pmw3610_RelayRequest_init_zero;
    relay_req.request_id = request_id;
    relay_req.has_request = true;
    relay_req.request = *req;

    struct zmk_pmw3610_relay_request event = {.source = ZMK_RELAY_EVENT_SOURCE_SELF};
    pb_ostream_t ostream = pb_ostream_from_buffer(event.payload, sizeof(event.payload));
    if (!pb_encode(&ostream, cormoran_pmw3610_RelayRequest_fields, &relay_req)) {
        LOG_WRN("Failed to encode pmw3610 relay request: %s", PB_GET_ERROR(&ostream));
        cormoran_pmw3610_ErrorResponse err = cormoran_pmw3610_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "failed to encode relay request");
        resp->which_response_type = cormoran_pmw3610_Response_error_tag;
        resp->response_type.error = err;
        return;
    }
    event.size = (uint16_t)ostream.bytes_written;

    raise_zmk_pmw3610_relay_request(event);

    cormoran_pmw3610_DeferredResponse deferred = cormoran_pmw3610_DeferredResponse_init_zero;
    deferred.request_id = request_id;
    resp->which_response_type = cormoran_pmw3610_Response_deferred_tag;
    resp->response_type.deferred = deferred;
}

static K_MUTEX_DEFINE(peripheral_response_notification_lock);
static cormoran_pmw3610_Notification peripheral_response_notification;

static bool encode_pmw3610_notification_payload(pb_ostream_t *stream, const pb_field_t *field,
                                                void *const *arg) {
    const cormoran_pmw3610_Notification *notification = (const cormoran_pmw3610_Notification *)*arg;
    return zmk_rpc_custom_subsystem_encode_response_payload(
        stream, field, cormoran_pmw3610_Notification_fields, notification);
}

static int raise_pmw3610_notification(cormoran_pmw3610_Notification *notification) {
    int index = custom_subsystem_index();
    if (index < 0) {
        return index;
    }

    pb_callback_t payload = {
        .funcs.encode = encode_pmw3610_notification_payload,
        .arg = (void *)notification,
    };

    return raise_zmk_studio_custom_notification((struct zmk_studio_custom_notification){
        .subsystem_index = (uint8_t)index,
        .encode_payload = payload,
    });
}

static int on_pmw3610_relay_response(const zmk_event_t *eh) {
    const struct zmk_pmw3610_relay_response *ev = as_zmk_pmw3610_relay_response(eh);
    if (!ev) {
        return 0;
    }

    cormoran_pmw3610_RelayResponse relay_resp = cormoran_pmw3610_RelayResponse_init_zero;
    pb_istream_t istream = pb_istream_from_buffer(ev->payload, ev->size);
    if (!pb_decode(&istream, cormoran_pmw3610_RelayResponse_fields, &relay_resp)) {
        LOG_WRN("Failed to decode pmw3610 relay response: %s", PB_GET_ERROR(&istream));
        return 0;
    }

    k_mutex_lock(&peripheral_response_notification_lock, K_FOREVER);

    peripheral_response_notification =
        (cormoran_pmw3610_Notification)cormoran_pmw3610_Notification_init_zero;
    peripheral_response_notification.which_notification_type =
        cormoran_pmw3610_Notification_peripheral_response_tag;
    cormoran_pmw3610_PeripheralResponse *pr =
        &peripheral_response_notification.notification_type.peripheral_response;
    /* ev->source was rewritten by ZMK_RELAY_EVENT_HANDLE's receive-side
     * `source_field_name = ev->source + 1` to the relaying peripheral's
     * slot + 1 -- exactly the addressing convention this module documents
     * for `source` elsewhere (0 = local/central, N = peripheral slot N). */
    pr->source = ev->source;
    pr->request_id = relay_resp.request_id;
    pr->has_response = relay_resp.has_response;
    if (relay_resp.has_response) {
        pr->response = relay_resp.response;
    }

    int ret = raise_pmw3610_notification(&peripheral_response_notification);
    if (ret) {
        LOG_WRN("Failed to raise pmw3610 PeripheralResponse notification: %d", ret);
    }

    k_mutex_unlock(&peripheral_response_notification_lock);
    return 0;
}

ZMK_LISTENER(pmw3610_relay_response_notify, on_pmw3610_relay_response);
ZMK_SUBSCRIPTION(pmw3610_relay_response_notify, zmk_pmw3610_relay_response);

static K_MUTEX_DEFINE(relayed_notification_lock);
static cormoran_pmw3610_Notification relayed_notification;

/* A peripheral's own FrameStreamChunk notification (raised via
 * pmw3610_relay_notify(), the peripheral counterpart of this function)
 * relayed back in -- stamp `source` (the streaming peripheral's slot + 1,
 * same convention as PeripheralResponse.source) and re-raise it as the
 * actual Studio notification. */
static int on_pmw3610_relay_notification(const zmk_event_t *eh) {
    const struct zmk_pmw3610_relay_notification *ev = as_zmk_pmw3610_relay_notification(eh);
    if (!ev) {
        return 0;
    }

    k_mutex_lock(&relayed_notification_lock, K_FOREVER);

    relayed_notification = (cormoran_pmw3610_Notification)cormoran_pmw3610_Notification_init_zero;
    pb_istream_t istream = pb_istream_from_buffer(ev->payload, ev->size);
    if (!pb_decode(&istream, cormoran_pmw3610_Notification_fields, &relayed_notification)) {
        LOG_WRN("Failed to decode pmw3610 relay notification: %s", PB_GET_ERROR(&istream));
        k_mutex_unlock(&relayed_notification_lock);
        return 0;
    }

    if (relayed_notification.which_notification_type ==
        cormoran_pmw3610_Notification_frame_stream_chunk_tag) {
        relayed_notification.notification_type.frame_stream_chunk.source = ev->source;
    }

    int ret = raise_pmw3610_notification(&relayed_notification);
    if (ret) {
        LOG_WRN("Failed to raise relayed pmw3610 notification: %d", ret);
    }

    k_mutex_unlock(&relayed_notification_lock);
    return 0;
}

ZMK_LISTENER(pmw3610_relay_notification_notify, on_pmw3610_relay_notification);
ZMK_SUBSCRIPTION(pmw3610_relay_notification_notify, zmk_pmw3610_relay_notification);

#endif // CONFIG_ZMK_SPLIT_ROLE_CENTRAL

/* --- native_sim-only self-test (peripheral role): exercise the relay --- */
/* --- executor without a real transport (native_sim cannot simulate one) --- */

#if IS_ENABLED(CONFIG_ZMK_PMW3610_SPLIT_RPC_RELAY_TEST)

static int pmw3610_split_relay_test_init(void) {
    cormoran_pmw3610_RelayRequest relay_req = cormoran_pmw3610_RelayRequest_init_zero;
    relay_req.request_id = 42;
    relay_req.has_request = true;
    relay_req.request.which_request_type = cormoran_pmw3610_Request_get_info_tag;
    relay_req.request.request_type.get_info.source = 1;

    uint8_t payload[PMW3610_RELAY_REQUEST_PAYLOAD_MAX_SIZE];
    pb_ostream_t ostream = pb_ostream_from_buffer(payload, sizeof(payload));
    if (!pb_encode(&ostream, cormoran_pmw3610_RelayRequest_fields, &relay_req)) {
        LOG_ERR("Split relay test: failed to encode synthetic request: %s", PB_GET_ERROR(&ostream));
        return -EIO;
    }

    cormoran_pmw3610_RelayResponse relay_resp;
    int ret = pmw3610_relay_exec_request(payload, ostream.bytes_written, &relay_resp);
    if (ret < 0) {
        LOG_ERR("Split relay test: exec failed: %d", ret);
        return ret;
    }

    if (relay_resp.request_id != 42) {
        LOG_ERR("Split relay test: request_id mismatch: got %u", relay_resp.request_id);
        return -EINVAL;
    }
    if (relay_resp.response.which_response_type != cormoran_pmw3610_Response_get_info_tag) {
        LOG_ERR("Split relay test: expected a GetInfoResponse, got response type %d",
                relay_resp.response.which_response_type);
        return -EINVAL;
    }

    printk("PASS: pmw3610_split_relay device_count=%u\n",
           (unsigned int)relay_resp.response.response_type.get_info.devices_count);

    /* CaptureFrame is a supported (relayable) request kind (Phase F-d), but
     * with zero local devices (native_sim) it errors out on device_index
     * resolution -- still an ErrorResponse, just for a different underlying
     * reason than an unsupported request kind (checked next). */
    relay_req.request.which_request_type = cormoran_pmw3610_Request_capture_frame_tag;
    relay_req.request.request_type.capture_frame.device_index = 0;
    ostream = pb_ostream_from_buffer(payload, sizeof(payload));
    if (!pb_encode(&ostream, cormoran_pmw3610_RelayRequest_fields, &relay_req)) {
        LOG_ERR("Split relay test: failed to encode CaptureFrame request: %s",
                PB_GET_ERROR(&ostream));
        return -EIO;
    }
    ret = pmw3610_relay_exec_request(payload, ostream.bytes_written, &relay_resp);
    if (ret < 0) {
        LOG_ERR("Split relay test: CaptureFrame exec failed: %d", ret);
        return ret;
    }
    if (relay_resp.response.which_response_type != cormoran_pmw3610_Response_error_tag) {
        LOG_ERR("Split relay test: expected an ErrorResponse (no local devices) for a relayed "
                "CaptureFrame, got response type %d",
                relay_resp.response.which_response_type);
        return -EINVAL;
    }

    printk("PASS: pmw3610_split_relay_capture_frame_no_device\n");

    /* Genuinely unsupported/malformed relayed request (no request_type set
     * at all): pmw3610_request_exec_handle() returns false, which must
     * still produce an ErrorResponse, not a crash or an unfilled response. */
    cormoran_pmw3610_RelayRequest empty_relay_req = cormoran_pmw3610_RelayRequest_init_zero;
    empty_relay_req.request_id = 43;
    empty_relay_req.has_request = true;
    ostream = pb_ostream_from_buffer(payload, sizeof(payload));
    if (!pb_encode(&ostream, cormoran_pmw3610_RelayRequest_fields, &empty_relay_req)) {
        LOG_ERR("Split relay test: failed to encode empty request: %s", PB_GET_ERROR(&ostream));
        return -EIO;
    }
    ret = pmw3610_relay_exec_request(payload, ostream.bytes_written, &relay_resp);
    if (ret < 0) {
        LOG_ERR("Split relay test: empty-request exec failed: %d", ret);
        return ret;
    }
    if (relay_resp.response.which_response_type != cormoran_pmw3610_Response_error_tag) {
        LOG_ERR("Split relay test: expected an ErrorResponse for an unset request kind, got "
                "response type %d",
                relay_resp.response.which_response_type);
        return -EINVAL;
    }

    printk("PASS: pmw3610_split_relay_unsupported_kind\n");
    return 0;
}

SYS_INIT(pmw3610_split_relay_test_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif // CONFIG_ZMK_PMW3610_SPLIT_RPC_RELAY_TEST
