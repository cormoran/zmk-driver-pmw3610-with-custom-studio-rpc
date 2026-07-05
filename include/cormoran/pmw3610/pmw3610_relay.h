#pragma once

/**
 * @file pmw3610_relay.h
 *
 * @brief Split relay bridge entry points for the `cormoran.pmw3610` Studio
 * RPC subsystem, when CONFIG_ZMK_PMW3610_SPLIT_RPC_RELAY is enabled -- see
 * DESIGN.md Phase F and src/split/pmw3610_relay.c.
 *
 * Two entry points, one per role:
 *  - pmw3610_relay_dispatch_request() (central only): relays a request to a
 *    split peripheral's own PMW3610 devices. Relaying is inherently
 *    asynchronous (the split link round-trip does not fit the Studio RPC
 *    call/response model), so this always returns immediately with a
 *    DeferredResponse; the real Response for the assigned request_id
 *    arrives later as a PeripheralResponse Studio notification.
 *  - pmw3610_relay_notify() (peripheral only): relays an already fully-formed
 *    Notification (currently only FrameStreamChunk) to the central, which
 *    re-raises it as the actual Studio notification (with `source` filled
 *    in) once it arrives.
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

/** @brief Relay `notification` to the central (split peripheral role only).
 *
 * The central re-raises it as the actual Studio custom notification once it
 * arrives, filling in `source` (see FrameStreamChunk.source). Silently
 * drops the notification (logs a warning) if encoding fails; frame
 * streaming already tolerates a dropped notification (the client-side
 * assembler detects an incomplete/stale frame).
 *
 * @param notification A fully-formed Notification (e.g. frame_stream_chunk
 *   populated) to relay as-is.
 */
void pmw3610_relay_notify(const cormoran_pmw3610_Notification *notification);

#ifdef __cplusplus
}
#endif
