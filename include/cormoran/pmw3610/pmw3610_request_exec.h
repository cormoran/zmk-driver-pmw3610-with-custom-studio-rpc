#pragma once

/**
 * @file pmw3610_request_exec.h
 *
 * @brief Transport-independent execution of the `cormoran.pmw3610` proto's
 * GetInfo/ReadDiagnostics/ReadRegister/WriteRegister requests against this
 * build's LOCAL PMW3610 devices (pmw3610_device_count()/pmw3610_get_device()
 * -- on a split peripheral, "local" means the peripheral's own devices, not
 * the central's).
 *
 * Shared by two callers that each decide *whether* a request should be
 * executed locally at all:
 *  - src/studio/pmw3610_handler.c (the cormoran__pmw3610 Studio RPC
 *    subsystem, central/non-split only) -- for a request whose `source`
 *    field is 0 (or the request kind has no `source` field, e.g.
 *    CaptureFrame).
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

#include <cormoran/pmw3610/pmw3610.pb.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Execute a GetInfo/ReadDiagnostics/ReadRegister/WriteRegister
 * request against local devices.
 *
 * @param req Decoded request. Only get_info/read_diagnostics/read_register/
 *   write_register request kinds are handled.
 * @param resp Output response, filled with either the successful response
 *   or an ErrorResponse. Untouched if `req` is a different request kind.
 * @return true if `req` was one of the four supported kinds (resp is always
 *   filled in that case); false otherwise (resp untouched, caller must
 *   handle the request kind itself, e.g. frame capture).
 */
bool pmw3610_request_exec_handle(const cormoran_pmw3610_Request *req,
                                 cormoran_pmw3610_Response *resp);

/** @brief Extract the `source` field from a get_info/read_diagnostics/
 * read_register/write_register request (0 for any other request kind,
 * matching those kinds' lack of a `source` field -- they always target
 * local devices).
 */
uint32_t pmw3610_request_get_source(const cormoran_pmw3610_Request *req);

#ifdef __cplusplus
}
#endif
