import { useContext, useState } from "react";
import "./App.css";
import { connect as serial_connect } from "@zmkfirmware/zmk-studio-ts-client/transport/serial";
import {
  ZMKConnection,
  ZMKCustomSubsystem,
  ZMKAppContext,
} from "@cormoran/zmk-studio-react-hook";
import { Request, Response } from "./proto/cormoran/pmw3610/pmw3610";

export const SUBSYSTEM_IDENTIFIER = "cormoran__pmw3610";

function App() {
  return (
    <div className="app">
      <header className="app-header">
        <h1>🖱️ PMW3610 Trackball</h1>
        <p>Custom Studio RPC Demo</p>
      </header>

      <ZMKConnection
        renderDisconnected={({ connect, isLoading, error }) => (
          <section className="card">
            <h2>Device Connection</h2>
            {isLoading && <p>⏳ Connecting...</p>}
            {error && (
              <div className="error-message">
                <p>🚨 {error}</p>
              </div>
            )}
            {!isLoading && (
              <button
                className="btn btn-primary"
                onClick={() => connect(serial_connect)}
              >
                🔌 Connect Serial
              </button>
            )}
          </section>
        )}
        renderConnected={({ disconnect, deviceName }) => (
          <>
            <section className="card">
              <h2>Device Connection</h2>
              <div className="device-info">
                <h3>✅ Connected to: {deviceName}</h3>
              </div>
              <button className="btn btn-secondary" onClick={disconnect}>
                Disconnect
              </button>
            </section>

            <RPCTestSection />
          </>
        )}
      />

      <footer className="app-footer">
        <p>
          <strong>PMW3610 Module</strong> - Sensor diagnostics over custom
          Studio RPC
        </p>
      </footer>
    </div>
  );
}

export function RPCTestSection() {
  const zmkApp = useContext(ZMKAppContext);
  const [response, setResponse] = useState<string | null>(null);
  const [isLoading, setIsLoading] = useState(false);

  if (!zmkApp) return null;

  const subsystem = zmkApp.findSubsystem(SUBSYSTEM_IDENTIFIER);

  const sendGetInfoRequest = async () => {
    if (!zmkApp.state.connection || !subsystem) return;

    setIsLoading(true);
    setResponse(null);

    try {
      const service = new ZMKCustomSubsystem(
        zmkApp.state.connection,
        subsystem.index
      );

      const request = Request.create({
        getInfo: {},
      });

      const payload = Request.encode(request).finish();
      const responsePayload = await service.callRPC(payload);

      if (responsePayload) {
        const resp = Response.decode(responsePayload);
        console.log("Decoded response:", resp);

        if (resp.getInfo) {
          setResponse(JSON.stringify(resp.getInfo, null, 2));
        } else if (resp.error) {
          setResponse(`Error: ${resp.error.message}`);
        }
      }
    } catch (error) {
      console.error("RPC call failed:", error);
      setResponse(
        `Failed: ${error instanceof Error ? error.message : "Unknown error"}`
      );
    } finally {
      setIsLoading(false);
    }
  };

  if (!subsystem) {
    return (
      <section className="card">
        <div className="warning-message">
          <p>
            ⚠️ Subsystem "{SUBSYSTEM_IDENTIFIER}" not found. Make sure your
            firmware includes the PMW3610 module.
          </p>
        </div>
      </section>
    );
  }

  return (
    <section className="card">
      <h2>Sensor Info</h2>
      <p>Query PMW3610 device info from the firmware:</p>

      <button
        className="btn btn-primary"
        disabled={isLoading}
        onClick={sendGetInfoRequest}
      >
        {isLoading ? "⏳ Loading..." : "📤 Get Info"}
      </button>

      {response && (
        <div className="response-box">
          <h3>Response from Firmware:</h3>
          <pre>{response}</pre>
        </div>
      )}
    </section>
  );
}

export default App;
