#pragma once

/**
 * @file pmw3610_relay.h
 *
 * @brief Central-side entry point for relaying a `cormoran.pmw3610` Studio
 * RPC request (GetInfo/ReadDiagnostics/ReadRegister/WriteRegister only) to a
 * split peripheral's own PMW3610 devices, when
 * CONFIG_ZMK_PMW3610_SPLIT_RPC_RELAY is enabled -- see DESIGN.md Phase F and
 * src/split/pmw3610_relay.c.
 *
 * Relaying is inherently asynchronous (the split link round-trip does not
 * fit the Studio RPC call/response model), so this always returns
 * immediately with a DeferredResponse; the real Response for the assigned
 * request_id arrives later as a PeripheralResponse Studio notification.
 */

#include <cormoran/pmw3610/pmw3610.pb.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Relay `req` to the split peripheral(s) and fill `resp` with a
 * DeferredResponse.
 *
 * @param source The request's `source` field (nonzero; a zero source is
 *   handled locally by the caller and never reaches this function).
 * @param req The decoded request to relay (one of get_info/
 *   read_diagnostics/read_register/write_register).
 * @param resp Always filled with a DeferredResponse (or an ErrorResponse if
 *   encoding/relaying failed outright).
 */
void pmw3610_relay_dispatch_request(uint32_t source, const cormoran_pmw3610_Request *req,
                                    cormoran_pmw3610_Response *resp);

#ifdef __cplusplus
}
#endif
