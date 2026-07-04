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
    // environments.
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

static bool pmw3610_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                       pb_callback_t *encode_response);

ZMK_RPC_CUSTOM_SUBSYSTEM(cormoran__pmw3610, &pmw3610_feature_meta, pmw3610_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(cormoran__pmw3610, cormoran_pmw3610_Response);

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

        result.devices_count++;
    }

    resp->which_response_type = cormoran_pmw3610_Response_get_info_tag;
    resp->response_type.get_info = result;
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
        cormoran_pmw3610_ErrorResponse err = cormoran_pmw3610_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "Failed to decode request");
        resp->which_response_type = cormoran_pmw3610_Response_error_tag;
        resp->response_type.error = err;
        return true;
    }

    switch (req.which_request_type) {
    case cormoran_pmw3610_Request_get_info_tag:
        handle_get_info_request(&req.request_type.get_info, resp);
        break;
    default:
        LOG_WRN("Unsupported pmw3610 request type: %d", req.which_request_type);
        cormoran_pmw3610_ErrorResponse err = cormoran_pmw3610_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "Unsupported request type");
        resp->which_response_type = cormoran_pmw3610_Response_error_tag;
        resp->response_type.error = err;
        break;
    }

    return true;
}
