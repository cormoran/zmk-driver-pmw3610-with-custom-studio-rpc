// Split relay support (DESIGN.md Phase F): a request with a nonzero
// `source` is answered asynchronously -- the RPC call returns a
// `DeferredResponse{requestId}` immediately, and the real Response arrives
// later as a `PeripheralResponse{source, requestId, response}` Studio
// notification. This module correlates the two by requestId, independent
// of any React component -- pure and unit-testable without a DOM/canvas.

import { Notification, Response } from "./proto/cormoran/pmw3610/pmw3610";

const DEFAULT_TIMEOUT_MS = 6000; // > firmware's ~5s frame-capture deadline

interface PendingRequest {
  resolve: (response: Response) => void;
  reject: (error: Error) => void;
  timeoutId: ReturnType<typeof setTimeout>;
}

/**
 * Tracks relayed requests awaiting their PeripheralResponse notification.
 * One instance is enough per subsystem connection -- requestId is assigned
 * by the firmware from a single counter, so concurrent callers (e.g.
 * multiple UI panels) sharing one correlator never collide.
 */
export class PeripheralResponseCorrelator {
  private pending = new Map<number, PendingRequest>();

  constructor(private readonly timeoutMs: number = DEFAULT_TIMEOUT_MS) {}

  /** Register interest in `requestId`'s eventual PeripheralResponse. */
  waitFor(requestId: number): Promise<Response> {
    return new Promise((resolve, reject) => {
      const timeoutId = setTimeout(() => {
        this.pending.delete(requestId);
        reject(
          new Error(
            `Timed out waiting for a PeripheralResponse to request ${requestId}`
          )
        );
      }, this.timeoutMs);
      this.pending.set(requestId, { resolve, reject, timeoutId });
    });
  }

  /**
   * Feed a raw Studio custom-notification payload. Resolves the matching
   * pending request (if any) when it decodes to a PeripheralResponse with a
   * response payload. Returns true if a pending request was resolved.
   */
  handleNotificationPayload(payload: Uint8Array): boolean {
    let notification: Notification;
    try {
      notification = Notification.decode(payload);
    } catch {
      return false;
    }
    const peripheralResponse = notification.peripheralResponse;
    if (!peripheralResponse || !peripheralResponse.response) {
      return false;
    }
    return this.resolve(
      peripheralResponse.requestId,
      peripheralResponse.response
    );
  }

  private resolve(requestId: number, response: Response): boolean {
    const pending = this.pending.get(requestId);
    if (!pending) {
      return false;
    }
    clearTimeout(pending.timeoutId);
    this.pending.delete(requestId);
    pending.resolve(response);
    return true;
  }

  /** Reject every still-pending request (e.g. on disconnect/unmount). */
  clear(reason: string): void {
    for (const pending of this.pending.values()) {
      clearTimeout(pending.timeoutId);
      pending.reject(new Error(reason));
    }
    this.pending.clear();
  }
}

/**
 * Send `request` over `service` and resolve with the real Response --
 * transparently awaiting the correlated PeripheralResponse notification if
 * the immediate reply is a DeferredResponse (source != 0).
 */
export async function callPmw3610Request(
  service: {
    callRPC(payload: Uint8Array): Promise<Uint8Array | null | undefined>;
  },
  encodedRequest: Uint8Array,
  correlator: PeripheralResponseCorrelator
): Promise<Response> {
  const responsePayload = await service.callRPC(encodedRequest);
  if (!responsePayload) {
    throw new Error("Empty response");
  }
  const response = Response.decode(responsePayload);
  if (response.deferred) {
    return correlator.waitFor(response.deferred.requestId);
  }
  return response;
}
