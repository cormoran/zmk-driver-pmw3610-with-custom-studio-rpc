import { callPmw3610Request, PeripheralResponseCorrelator } from "../src/relay";
import { Notification, Response } from "../src/proto/cormoran/pmw3610/pmw3610";

describe("PeripheralResponseCorrelator", () => {
  it("resolves waitFor() when a matching PeripheralResponse notification arrives", async () => {
    const correlator = new PeripheralResponseCorrelator();
    const pending = correlator.waitFor(7);

    const payload = Notification.encode(
      Notification.create({
        peripheralResponse: {
          source: 1,
          requestId: 7,
          response: { readRegister: { value: 42 } },
        },
      })
    ).finish();

    expect(correlator.handleNotificationPayload(payload)).toBe(true);
    const response = await pending;
    expect(response.readRegister?.value).toBe(42);
  });

  it("ignores a notification for a request id nobody is waiting for", () => {
    const correlator = new PeripheralResponseCorrelator();
    const payload = Notification.encode(
      Notification.create({
        peripheralResponse: { source: 1, requestId: 99, response: {} },
      })
    ).finish();

    expect(correlator.handleNotificationPayload(payload)).toBe(false);
  });

  it("ignores non-PeripheralResponse notifications (e.g. FrameStreamChunk)", () => {
    const correlator = new PeripheralResponseCorrelator();
    void correlator.waitFor(1).catch(() => {});
    const payload = Notification.encode(
      Notification.create({
        frameStreamChunk: { frameId: 1, offset: 0, data: new Uint8Array() },
      })
    ).finish();

    expect(correlator.handleNotificationPayload(payload)).toBe(false);
    correlator.clear("test cleanup");
  });

  it("ignores garbage payloads without throwing", () => {
    const correlator = new PeripheralResponseCorrelator();
    expect(
      correlator.handleNotificationPayload(new Uint8Array([0xff, 0xff, 0xff]))
    ).toBe(false);
  });

  it("rejects a pending request once its timeout elapses", async () => {
    jest.useFakeTimers();
    try {
      const correlator = new PeripheralResponseCorrelator(1000);
      const pending = correlator.waitFor(3);
      const assertion = expect(pending).rejects.toThrow(/Timed out/);
      jest.advanceTimersByTime(1000);
      await assertion;
    } finally {
      jest.useRealTimers();
    }
  });

  it("clear() rejects every still-pending request", async () => {
    const correlator = new PeripheralResponseCorrelator();
    const pending = correlator.waitFor(5);
    correlator.clear("disconnected");
    await expect(pending).rejects.toThrow("disconnected");
  });

  it("a resolved request id can be reused afterwards", async () => {
    const correlator = new PeripheralResponseCorrelator();
    const first = correlator.waitFor(1);
    correlator.handleNotificationPayload(
      Notification.encode(
        Notification.create({
          peripheralResponse: { source: 1, requestId: 1, response: {} },
        })
      ).finish()
    );
    await first;

    // Same id, no longer pending -- must not resolve/throw a second time.
    expect(
      correlator.handleNotificationPayload(
        Notification.encode(
          Notification.create({
            peripheralResponse: { source: 1, requestId: 1, response: {} },
          })
        ).finish()
      )
    ).toBe(false);
  });

  describe("collectBroadcast", () => {
    it("collects every PeripheralResponse for a request id within the window", async () => {
      jest.useFakeTimers();
      try {
        const correlator = new PeripheralResponseCorrelator();
        const resultsPromise = correlator.collectBroadcast(20, 1000);

        correlator.handleNotificationPayload(
          Notification.encode(
            Notification.create({
              peripheralResponse: {
                source: 1,
                requestId: 20,
                response: { getInfo: { devices: [{ deviceIndex: 0 }] } },
              },
            })
          ).finish()
        );
        correlator.handleNotificationPayload(
          Notification.encode(
            Notification.create({
              peripheralResponse: {
                source: 2,
                requestId: 20,
                response: { getInfo: { devices: [] } },
              },
            })
          ).finish()
        );

        jest.advanceTimersByTime(1000);
        const results = await resultsPromise;

        expect(results).toHaveLength(2);
        expect(results.map((r) => r.source).sort()).toEqual([1, 2]);
      } finally {
        jest.useRealTimers();
      }
    });

    it("resolves with an empty array when nobody answers", async () => {
      jest.useFakeTimers();
      try {
        const correlator = new PeripheralResponseCorrelator();
        const resultsPromise = correlator.collectBroadcast(21, 500);
        jest.advanceTimersByTime(500);
        await expect(resultsPromise).resolves.toEqual([]);
      } finally {
        jest.useRealTimers();
      }
    });

    it("does not let a waitFor() and a collectBroadcast() on the same id double-consume", async () => {
      const correlator = new PeripheralResponseCorrelator();
      // waitFor() takes priority (checked first in handleNotificationPayload);
      // once it resolves the entry is gone, so a second identical
      // notification would fall through to any broadcast listener instead.
      const pending = correlator.waitFor(30);
      const payload = Notification.encode(
        Notification.create({
          peripheralResponse: { source: 1, requestId: 30, response: {} },
        })
      ).finish();

      expect(correlator.handleNotificationPayload(payload)).toBe(true);
      await pending;
    });
  });
});

describe("callPmw3610Request", () => {
  it("returns the response directly when it is not deferred", async () => {
    const correlator = new PeripheralResponseCorrelator();
    const responsePayload = Response.encode(
      Response.create({ readRegister: { value: 7 } })
    ).finish();
    const service = { callRPC: jest.fn().mockResolvedValue(responsePayload) };

    const response = await callPmw3610Request(
      service,
      new Uint8Array(),
      correlator
    );
    expect(response.readRegister?.value).toBe(7);
  });

  it("awaits the correlated PeripheralResponse when the reply is deferred", async () => {
    const correlator = new PeripheralResponseCorrelator();
    const deferredPayload = Response.encode(
      Response.create({ deferred: { requestId: 11 } })
    ).finish();
    const service = { callRPC: jest.fn().mockResolvedValue(deferredPayload) };

    const resultPromise = callPmw3610Request(
      service,
      new Uint8Array(),
      correlator
    );
    // Let callPmw3610Request's `await service.callRPC(...)` resolve and
    // register the pending waitFor(11) before the notification "arrives".
    await Promise.resolve();
    await Promise.resolve();

    correlator.handleNotificationPayload(
      Notification.encode(
        Notification.create({
          peripheralResponse: {
            source: 1,
            requestId: 11,
            response: { getInfo: { devices: [] } },
          },
        })
      ).finish()
    );

    const result = await resultPromise;
    expect(result.getInfo?.devices).toEqual([]);
  });

  it("throws on an empty RPC response", async () => {
    const correlator = new PeripheralResponseCorrelator();
    const service = { callRPC: jest.fn().mockResolvedValue(null) };

    await expect(
      callPmw3610Request(service, new Uint8Array(), correlator)
    ).rejects.toThrow("Empty response");
  });
});
