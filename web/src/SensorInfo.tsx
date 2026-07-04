import { useContext, useEffect, useRef, useState } from "react";
import {
  ZMKCustomSubsystem,
  ZMKAppContext,
} from "@cormoran/zmk-studio-react-hook";
import {
  DeviceInfo,
  ReadDiagnosticsResponse,
  Request,
  Response,
} from "./proto/cormoran/pmw3610/pmw3610";

export const PMW3610_SUBSYSTEM_IDENTIFIER = "cormoran__pmw3610";
export const PMW3610_PRODUCT_ID = 0x3e;
const DIAGNOSTICS_POLL_INTERVAL_MS = 1000;

export function SensorInfo() {
  const zmkApp = useContext(ZMKAppContext);
  const subsystem = zmkApp?.findSubsystem(PMW3610_SUBSYSTEM_IDENTIFIER);

  const [devices, setDevices] = useState<DeviceInfo[]>([]);
  const [selectedDevice, setSelectedDevice] = useState(0);
  const [diagnostics, setDiagnostics] =
    useState<ReadDiagnosticsResponse | null>(null);
  const [autoRefresh, setAutoRefresh] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [isLoading, setIsLoading] = useState(false);

  const callRequest = async (request: Request): Promise<Response> => {
    const connection = zmkApp?.state.connection;
    if (!connection || !subsystem) {
      throw new Error("PMW3610 subsystem is not available");
    }
    const service = new ZMKCustomSubsystem(connection, subsystem.index);
    const payload = Request.encode(request).finish();
    const responsePayload = await service.callRPC(payload);
    if (!responsePayload) {
      throw new Error("Empty response");
    }
    return Response.decode(responsePayload);
  };

  const refreshInfo = async () => {
    setIsLoading(true);
    setError(null);
    try {
      const resp = await callRequest(Request.create({ getInfo: {} }));
      if (resp.error) {
        throw new Error(resp.error.message);
      }
      setDevices(resp.getInfo?.devices ?? []);
    } catch (e) {
      setError(e instanceof Error ? e.message : "Unknown error");
    } finally {
      setIsLoading(false);
    }
  };

  const refreshDiagnostics = async () => {
    try {
      const resp = await callRequest(
        Request.create({ readDiagnostics: { deviceIndex: selectedDevice } })
      );
      if (resp.error) {
        throw new Error(resp.error.message);
      }
      if (resp.readDiagnostics) {
        setDiagnostics(resp.readDiagnostics);
      }
    } catch (e) {
      setError(e instanceof Error ? e.message : "Unknown error");
    }
  };

  useEffect(() => {
    if (subsystem) {
      // Fire-and-forget: refreshInfo manages its own loading/error state
      // asynchronously. This effect only needs to kick it off once when the
      // subsystem becomes available (react-hooks/set-state-in-effect flags
      // the eventual setState inside refreshInfo, which is intentional
      // here -- there is no synchronous render-phase setState).
      // eslint-disable-next-line react-hooks/set-state-in-effect
      void refreshInfo();
    }
    // Load once when the subsystem becomes available.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [subsystem?.index]);

  const intervalRef = useRef<ReturnType<typeof setInterval> | null>(null);
  useEffect(() => {
    if (autoRefresh && subsystem) {
      intervalRef.current = setInterval(
        () => void refreshDiagnostics(),
        DIAGNOSTICS_POLL_INTERVAL_MS
      );
      return () => {
        if (intervalRef.current) clearInterval(intervalRef.current);
      };
    }
    // Only depends on the toggle/device/subsystem, not the callback identity.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [autoRefresh, selectedDevice, subsystem?.index]);

  if (!zmkApp) return null;

  if (!subsystem) {
    return (
      <section className="card">
        <h2>Sensor</h2>
        <div className="warning-message">
          <p>
            Subsystem &quot;{PMW3610_SUBSYSTEM_IDENTIFIER}&quot; not found. Make
            sure your firmware includes the PMW3610 module with
            CONFIG_ZMK_PMW3610_STUDIO_RPC=y.
          </p>
        </div>
      </section>
    );
  }

  const device = devices[selectedDevice];

  return (
    <>
      <section className="card">
        <h2>Sensor</h2>
        <div className="toolbar">
          <button
            className="btn"
            disabled={isLoading}
            onClick={() => void refreshInfo()}
          >
            {isLoading ? "Loading..." : "Refresh"}
          </button>
          {devices.length > 1 && (
            <>
              <label htmlFor="device-select" className="inline-label">
                Device
              </label>
              <select
                id="device-select"
                value={selectedDevice}
                onChange={(e) => setSelectedDevice(Number(e.target.value))}
              >
                {devices.map((d, i) => (
                  <option key={i} value={i}>
                    {i}
                    {d.settingsId ? ` (${d.settingsId})` : ""}
                  </option>
                ))}
              </select>
            </>
          )}
        </div>

        {error && (
          <div className="error-message">
            <p>{error}</p>
          </div>
        )}

        {device ? (
          <dl className="setting-summary">
            <div>
              <dt>Settings ID</dt>
              <dd>{device.settingsId || "(none)"}</dd>
            </div>
            <div>
              <dt>Ready</dt>
              <dd>{device.ready ? "yes" : "no"}</dd>
            </div>
            <div>
              <dt>Product ID</dt>
              <dd>
                0x{device.productId.toString(16).padStart(2, "0")}
                {device.productId !== PMW3610_PRODUCT_ID && " (unexpected)"}
              </dd>
            </div>
            <div>
              <dt>Revision ID</dt>
              <dd>0x{device.revisionId.toString(16).padStart(2, "0")}</dd>
            </div>
            <div>
              <dt>Init Error</dt>
              <dd>{device.initError}</dd>
            </div>
            {device.runtimeConfig && (
              <div>
                <dt>Runtime Config</dt>
                <dd>
                  <pre>{JSON.stringify(device.runtimeConfig, null, 2)}</pre>
                </dd>
              </div>
            )}
          </dl>
        ) : (
          <p className="empty-message">No devices reported.</p>
        )}
      </section>

      <section className="card">
        <h2>Diagnostics</h2>
        <div className="toolbar">
          <button className="btn" onClick={() => void refreshDiagnostics()}>
            Read Once
          </button>
          <label className="inline-label">
            <input
              type="checkbox"
              checked={autoRefresh}
              onChange={(e) => setAutoRefresh(e.target.checked)}
            />
            Auto-refresh (1s)
          </label>
        </div>

        {diagnostics ? (
          <dl className="setting-summary">
            <div>
              <dt>SQUAL</dt>
              <dd>{diagnostics.squal}</dd>
            </div>
            <div>
              <dt>Shutter</dt>
              <dd>{diagnostics.shutter}</dd>
            </div>
            <div>
              <dt>Pixel Min</dt>
              <dd>{diagnostics.pixMin}</dd>
            </div>
            <div>
              <dt>Pixel Avg</dt>
              <dd>{diagnostics.pixAvg}</dd>
            </div>
            <div>
              <dt>Pixel Max</dt>
              <dd>{diagnostics.pixMax}</dd>
            </div>
          </dl>
        ) : (
          <p className="empty-message">No diagnostics read yet.</p>
        )}
      </section>
    </>
  );
}
