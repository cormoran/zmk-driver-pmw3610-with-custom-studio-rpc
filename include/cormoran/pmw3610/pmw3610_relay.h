#pragma once

/**
 * @file pmw3610_relay.h
 *
 * @brief Split relay bridge entry points for the `cormoran.pmw3610` Studio
 * RPC subsystem, when CONFIG_ZMK_PMW3610_SPLIT_RPC_RELAY is enabled -- see
 * DESIGN.md Phase F and src/split/pmw3610_relay.c.
 *
 * Three entry points:
 *  - pmw3610_relay_dispatch_request() (central only): relays a request to a
 *    split peripheral's own PMW3610 devices. Relaying is inherently
 *    asynchronous (the split link round-trip does not fit the Studio RPC
 *    call/response model), so this always returns immediately with a
 *    DeferredResponse; the real Response for the assigned request_id
 *    arrives later as a PeripheralResponse Studio notification.
 *  - pmw3610_relay_broadcast_request() (central only): fire-and-forget
 *    variant used for GetInfo's `source = PMW3610_SOURCE_ALL` sentinel --
 *    every connected peripheral answers independently as its own
 *    PeripheralResponse notification, all sharing the returned request_id.
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

/** @brief Broadcast `req` to every connected peripheral without waiting for
 * (or producing) a DeferredResponse -- used for GetInfo's
 * `source = PMW3610_SOURCE_ALL` sentinel, where the caller already has an
 * immediate local answer to return synchronously and just needs any
 * peripheral answers correlated separately.
 *
 * @param req The request to broadcast (its `source` field is ignored by
 *   the peripheral executor regardless of value).
 * @return The request_id every responding peripheral's PeripheralResponse
 *   notification will carry (nonzero), or 0 if encoding failed (logged).
 */
uint32_t pmw3610_relay_broadcast_request(const cormoran_pmw3610_Request *req);

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
