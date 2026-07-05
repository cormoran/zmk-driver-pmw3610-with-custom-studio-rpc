#pragma once

/**
 * @file pmw3610_request_exec.h
 *
 * @brief Transport-independent execution of every `cormoran.pmw3610` proto
 * request kind (GetInfo/ReadDiagnostics/ReadRegister/WriteRegister/
 * CaptureFrame/GetFrameChunk/SetFrameStream) against this build's LOCAL
 * PMW3610 devices (pmw3610_device_count()/pmw3610_get_device() -- on a
 * split peripheral, "local" means the peripheral's own devices, not the
 * central's).
 *
 * Shared by two callers that each decide *whether* a request should be
 * executed locally at all:
 *  - src/studio/pmw3610_handler.c (the cormoran__pmw3610 Studio RPC
 *    subsystem, central/non-split only) -- for a request whose `source`
 *    field is 0.
 *  - src/split/pmw3610_relay.c (CONFIG_ZMK_PMW3610_SPLIT_RPC_RELAY, both
 *    split roles) -- for a request relayed in from the central, executed
 *    unconditionally against the peripheral's own devices (a peripheral has
 *    no further split role to relay to, and does not attempt to validate
 *    the request's `source` against its own identity -- see DESIGN.md
 *    Phase F's "broadcasts to every peripheral" caveat).
 *
 * This file intentionally has no notion of `source`/relaying itself -- that
 * routing decision belongs to the callers above.
 */

#include <stdbool.h>
#include <stdint.h>

#include <cormoran/pmw3610/pmw3610.pb.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Sentinel GetInfoRequest.source value meaning "list every PMW3610 across
 * the whole keyboard": the caller (pmw3610_handler.c) answers with local
 * devices synchronously *and* broadcasts the same request to every
 * connected peripheral -- see GetInfoRequest's doc comment in
 * pmw3610.proto and pmw3610_relay_broadcast_request(). Not meaningful to
 * pmw3610_request_exec_handle() itself, which ignores `source` entirely;
 * only the dispatch layer inspects this constant. */
#define PMW3610_SOURCE_ALL UINT32_MAX

/** @brief Execute any supported request kind against local devices.
 *
 * @param req Decoded request.
 * @param resp Output response, filled with either the successful response
 *   or an ErrorResponse. Untouched if `req` has no request_type set.
 * @return true if `req` was a supported kind (resp is always filled in
 *   that case); false otherwise (resp untouched -- in practice only a
 *   request with no request_type set at all, since every kind defined in
 *   pmw3610.proto is handled).
 */
bool pmw3610_request_exec_handle(const cormoran_pmw3610_Request *req,
                                 cormoran_pmw3610_Response *resp);

/** @brief Extract the `source` field from any request kind (every kind
 * defined in pmw3610.proto has one; returns 0 -- "local" -- for a request
 * with no request_type set at all).
 */
uint32_t pmw3610_request_get_source(const cormoran_pmw3610_Request *req);

#ifdef __cplusplus
}
#endif
