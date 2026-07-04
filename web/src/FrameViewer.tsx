import { useContext, useEffect, useRef, useState } from "react";
import {
  ZMKCustomSubsystem,
  ZMKAppContext,
} from "@cormoran/zmk-studio-react-hook";
import { Request, Response } from "./proto/cormoran/pmw3610/pmw3610";
import {
  assembleFrame,
  chunkOffsets,
  frameToRgba,
  type FrameChunk,
} from "./frame";

export const PMW3610_SUBSYSTEM_IDENTIFIER = "cormoran__pmw3610";

const SIDE_OPTIONS = [19, 20, 21, 22];
const DEFAULT_SIDE = 22;
const PIXEL_SCALE = 12; // px per sensor pixel on <canvas>
const STREAM_LOOP_DELAY_MS = 10; // small yield between captures while streaming

interface AdvancedOptions {
  maxInvalidRetries: string;
}

const DEFAULT_ADVANCED: AdvancedOptions = {
  maxInvalidRetries: "",
};

export function FrameViewer() {
  const zmkApp = useContext(ZMKAppContext);
  const subsystem = zmkApp?.findSubsystem(PMW3610_SUBSYSTEM_IDENTIFIER);

  const [side, setSide] = useState(DEFAULT_SIDE);
  const [deviceIndex, setDeviceIndex] = useState(0);
  const [advanced, setAdvanced] = useState<AdvancedOptions>(DEFAULT_ADVANCED);
  const [showAdvanced, setShowAdvanced] = useState(false);
  const [isCapturing, setIsCapturing] = useState(false);
  const [isStreaming, setIsStreaming] = useState(false);
  const [invalidCount, setInvalidCount] = useState<number | null>(null);
  const [pixelCount, setPixelCount] = useState<number | null>(null);
  const [complete, setComplete] = useState<boolean | null>(null);
  const [durationMs, setDurationMs] = useState<number | null>(null);
  const [fps, setFps] = useState<number | null>(null);
  const [error, setError] = useState<string | null>(null);

  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const streamingRef = useRef(false);
  const frameCountRef = useRef(0);
  const fpsWindowStartRef = useRef(0);

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

  const captureOnce = async (): Promise<void> => {
    const pixelCountRequested = side * side;
    const captureResp = await callRequest(
      Request.create({
        captureFrame: {
          deviceIndex,
          pixelCount: pixelCountRequested,
          maxInvalidRetries: advanced.maxInvalidRetries
            ? Number.parseInt(advanced.maxInvalidRetries, 10)
            : 0,
        },
      })
    );
    if (captureResp.error) {
      throw new Error(captureResp.error.message);
    }
    const captureFrame = captureResp.captureFrame;
    if (!captureFrame) {
      throw new Error("CaptureFrame response missing capture_frame field");
    }

    const chunkSize = captureFrame.chunkSize || 128;
    const totalLength = captureFrame.pixelCount;
    const offsets = chunkOffsets(totalLength, chunkSize);
    const chunks: FrameChunk[] = [];
    for (const offset of offsets) {
      const chunkResp = await callRequest(
        Request.create({
          getFrameChunk: { frameId: captureFrame.frameId, offset },
        })
      );
      if (chunkResp.error) {
        throw new Error(chunkResp.error.message);
      }
      const chunk = chunkResp.getFrameChunk;
      if (!chunk) {
        throw new Error("GetFrameChunk response missing get_frame_chunk field");
      }
      chunks.push({ offset: chunk.offset, data: chunk.data });
    }

    const assembled = assembleFrame(chunks, totalLength);
    renderFrame(assembled.bytes, side);
    setInvalidCount(assembled.invalidCount);
    setPixelCount(totalLength);
    setComplete(captureFrame.complete);
    setDurationMs(captureFrame.durationMs);
  };

  const renderFrame = (bytes: Uint8Array, sideLength: number) => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;

    canvas.width = sideLength * PIXEL_SCALE;
    canvas.height = sideLength * PIXEL_SCALE;

    const rgba = frameToRgba(bytes);
    // Draw at 1x onto an offscreen buffer, then scale up with
    // nearest-neighbor via imageSmoothingEnabled = false.
    const off = document.createElement("canvas");
    off.width = sideLength;
    off.height = sideLength;
    const offCtx = off.getContext("2d");
    if (!offCtx) return;
    // ImageData's constructor wants a Uint8ClampedArray<ArrayBuffer>
    // specifically; frameToRgba()'s return type is the more general
    // Uint8ClampedArray<ArrayBufferLike> so it stays independent of DOM
    // lib types for testability. Copying into a plain ArrayBuffer-backed
    // array satisfies the stricter DOM type without changing frame.ts.
    const imageData = new ImageData(
      new Uint8ClampedArray(rgba),
      sideLength,
      Math.ceil(bytes.length / sideLength)
    );
    offCtx.putImageData(imageData, 0, 0);

    ctx.imageSmoothingEnabled = false;
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    ctx.drawImage(
      off,
      0,
      0,
      sideLength,
      sideLength,
      0,
      0,
      canvas.width,
      canvas.height
    );
  };

  const handleCaptureOnce = async () => {
    setIsCapturing(true);
    setError(null);
    try {
      await captureOnce();
    } catch (e) {
      setError(e instanceof Error ? e.message : "Unknown error");
    } finally {
      setIsCapturing(false);
    }
  };

  const stopStreaming = () => {
    streamingRef.current = false;
    setIsStreaming(false);
  };

  const startStreaming = () => {
    if (streamingRef.current) return;
    streamingRef.current = true;
    setIsStreaming(true);
    setError(null);
    frameCountRef.current = 0;
    fpsWindowStartRef.current = performance.now();

    const loop = async () => {
      while (streamingRef.current) {
        try {
          await captureOnce();
          frameCountRef.current++;
          const now = performance.now();
          const elapsed = now - fpsWindowStartRef.current;
          if (elapsed >= 1000) {
            setFps((frameCountRef.current * 1000) / elapsed);
            frameCountRef.current = 0;
            fpsWindowStartRef.current = now;
          }
        } catch (e) {
          setError(e instanceof Error ? e.message : "Unknown error");
          streamingRef.current = false;
          setIsStreaming(false);
          break;
        }
        // Yield briefly so the UI thread / other RPC users aren't starved;
        // "streaming" here is a sequential capture loop, not a fixed-rate
        // timer.
        await new Promise((resolve) =>
          setTimeout(resolve, STREAM_LOOP_DELAY_MS)
        );
      }
    };
    void loop();
  };

  useEffect(() => {
    return () => {
      streamingRef.current = false;
    };
  }, []);

  if (!zmkApp) return null;

  if (!subsystem) {
    return (
      <section className="card">
        <h2>Frame Viewer</h2>
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

  return (
    <section className="card">
      <h2>Frame Viewer</h2>
      <p>
        Captures a still image from the sensor using the official PMW3610
        Pixel_Grab procedure (full array is 22 x 22). Capturing briefly
        interrupts mouse motion while the sensor resets afterwards.
      </p>

      <div className="form-grid">
        <label htmlFor="side-select">Size</label>
        <select
          id="side-select"
          value={side}
          onChange={(e) => setSide(Number(e.target.value))}
          disabled={isStreaming}
        >
          {SIDE_OPTIONS.map((n) => (
            <option key={n} value={n}>
              {n} x {n} ({n * n} px)
            </option>
          ))}
        </select>

        <label htmlFor="frame-device-index">Device index</label>
        <input
          id="frame-device-index"
          type="number"
          min={0}
          value={deviceIndex}
          onChange={(e) => setDeviceIndex(Number(e.target.value))}
          disabled={isStreaming}
        />
      </div>

      <div className="toolbar">
        <button
          className="btn btn-primary"
          disabled={isCapturing || isStreaming}
          onClick={() => void handleCaptureOnce()}
        >
          {isCapturing ? "Capturing..." : "Capture Once"}
        </button>
        {isStreaming ? (
          <button className="btn btn-danger" onClick={stopStreaming}>
            Stop Streaming
          </button>
        ) : (
          <button
            className="btn"
            disabled={isCapturing}
            onClick={startStreaming}
          >
            Start Streaming
          </button>
        )}
      </div>

      <details
        className="advanced-panel"
        open={showAdvanced}
        onToggle={(e) => setShowAdvanced((e.target as HTMLDetailsElement).open)}
      >
        <summary>Advanced (capture tuning)</summary>
        <div className="form-grid">
          <label htmlFor="max-invalid-retries">
            Per-pixel ready-wait retries, 10ms each (0 = default)
          </label>
          <input
            id="max-invalid-retries"
            type="number"
            min={0}
            max={100}
            value={advanced.maxInvalidRetries}
            onChange={(e) =>
              setAdvanced({ ...advanced, maxInvalidRetries: e.target.value })
            }
            placeholder="3"
          />
        </div>
      </details>

      {error && (
        <div className="error-message">
          <p>{error}</p>
        </div>
      )}

      <div className="frame-canvas-wrap">
        <canvas ref={canvasRef} className="frame-canvas" />
      </div>

      <dl className="setting-summary">
        <div>
          <dt>Pixels captured</dt>
          <dd>{pixelCount ?? "-"}</dd>
        </div>
        <div>
          <dt>Complete</dt>
          <dd>
            {complete === null ? "-" : complete ? "yes" : "no"}
            {complete === false && (
              <span className="warning-inline"> (sensor: not all 484)</span>
            )}
          </dd>
        </div>
        <div>
          <dt>Capture time</dt>
          <dd>{durationMs !== null ? `${durationMs} ms` : "-"}</dd>
        </div>
        <div>
          <dt>Invalid bytes</dt>
          <dd>
            {invalidCount ?? "-"}
            {invalidCount !== null && invalidCount > 0 && (
              <span className="warning-inline"> (PG_Valid clear)</span>
            )}
          </dd>
        </div>
        <div>
          <dt>FPS (streaming)</dt>
          <dd>{fps !== null ? fps.toFixed(1) : "-"}</dd>
        </div>
      </dl>
    </section>
  );
}
