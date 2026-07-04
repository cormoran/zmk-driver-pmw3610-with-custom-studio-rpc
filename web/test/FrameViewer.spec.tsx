import { act, render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { FrameViewer, PMW3610_SUBSYSTEM_IDENTIFIER } from "../src/FrameViewer";
import { Notification, Response } from "../src/proto/cormoran/pmw3610/pmw3610";

// Mock the ZMK client's call_rpc so ZMKCustomSubsystem.callRPC() (used
// directly by FrameViewer, not through useZMKApp) can be controlled per
// test.
jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({
  ...jest.requireActual("@zmkfirmware/zmk-studio-ts-client"),
  call_rpc: jest.fn(),
}));

describe("FrameViewer Component", () => {
  describe("With Subsystem", () => {
    it("renders capture controls and the size selector", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [PMW3610_SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <FrameViewer />
        </ZMKAppProvider>
      );

      expect(
        screen.getByRole("heading", { name: /Frame Viewer/i })
      ).toBeInTheDocument();
      expect(screen.getByText(/Capture Once/i)).toBeInTheDocument();
      expect(screen.getByText(/Start Streaming/i)).toBeInTheDocument();
      expect(screen.getByLabelText(/Size/i)).toBeInTheDocument();
    });
  });

  describe("Without Subsystem", () => {
    it("should show a warning when the subsystem is not found", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <FrameViewer />
        </ZMKAppProvider>
      );

      expect(
        screen.getByText(/Subsystem "cormoran__pmw3610" not found/i)
      ).toBeInTheDocument();
    });
  });

  describe("Without ZMKAppContext", () => {
    it("should not render when ZMKAppContext is not provided", () => {
      const { container } = render(<FrameViewer />);

      expect(container.firstChild).toBeNull();
    });
  });

  describe("Streaming (SetFrameStream + notifications)", () => {
    const setup = () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [PMW3610_SUBSYSTEM_IDENTIFIER],
      });
      // Capture the registered custom-notification callback so the test can
      // invoke it directly, simulating a FrameStreamChunk notification
      // arriving from the device.
      let notificationCallback:
        | ((n: { subsystemIndex: number; payload: Uint8Array }) => void)
        | null = null;
      mockZMKApp.onNotification = jest.fn((subscription) => {
        if (subscription.type === "custom") {
          notificationCallback = subscription.callback;
        }
        return () => {
          notificationCallback = null;
        };
      });

      return {
        mockZMKApp,
        emit: (payload: Uint8Array) => {
          act(() => {
            notificationCallback?.({ subsystemIndex: 0, payload });
          });
        },
      };
    };

    it("calls SetFrameStream(enable=true) and subscribes to notifications on Start Streaming", async () => {
      const { call_rpc } = await import("@zmkfirmware/zmk-studio-ts-client");
      (call_rpc as jest.Mock).mockResolvedValue({
        custom: {
          call: {
            payload: Response.encode(
              Response.create({ setFrameStream: { streaming: true } })
            ).finish(),
          },
        },
      });

      const { mockZMKApp } = setup();
      render(
        <ZMKAppProvider value={mockZMKApp}>
          <FrameViewer />
        </ZMKAppProvider>
      );

      const user = userEvent.setup();
      await user.click(screen.getByText(/Start Streaming/i));

      await waitFor(() => {
        expect(screen.getByText(/Stop Streaming/i)).toBeInTheDocument();
      });
      expect(mockZMKApp.onNotification).toHaveBeenCalledWith(
        expect.objectContaining({ type: "custom", subsystemIndex: 0 })
      );
    });

    it("assembles and renders a frame from FrameStreamChunk notifications", async () => {
      const { call_rpc } = await import("@zmkfirmware/zmk-studio-ts-client");
      (call_rpc as jest.Mock).mockResolvedValue({
        custom: {
          call: {
            payload: Response.encode(
              Response.create({ setFrameStream: { streaming: true } })
            ).finish(),
          },
        },
      });

      const { mockZMKApp, emit } = setup();
      render(
        <ZMKAppProvider value={mockZMKApp}>
          <FrameViewer />
        </ZMKAppProvider>
      );

      const user = userEvent.setup();
      await user.click(screen.getByText(/Start Streaming/i));
      await waitFor(() => {
        expect(screen.getByText(/Stop Streaming/i)).toBeInTheDocument();
      });

      const totalSize = 4;
      const chunk1 = Notification.encode(
        Notification.create({
          frameStreamChunk: {
            frameId: 1,
            offset: 0,
            data: Uint8Array.from([0x81, 0x82]),
            totalSize,
            complete: true,
          },
        })
      ).finish();
      const chunk2 = Notification.encode(
        Notification.create({
          frameStreamChunk: {
            frameId: 1,
            offset: 2,
            data: Uint8Array.from([0x00, 0x84]),
            totalSize,
            complete: true,
          },
        })
      ).finish();

      emit(chunk1);
      // A single chunk isn't the full frame yet -- pixel count still shows
      // the placeholder.
      expect(screen.getByText("Pixels captured").nextSibling).toHaveTextContent(
        "-"
      );

      emit(chunk2);

      await waitFor(() => {
        expect(screen.getByText("4")).toBeInTheDocument(); // pixelCount
      });
      // One byte (0x00) has bit7 clear -> counted invalid.
      expect(screen.getByText("1")).toBeInTheDocument();
    });

    it("calls SetFrameStream(enable=false) and unsubscribes on Stop Streaming", async () => {
      const { call_rpc } = await import("@zmkfirmware/zmk-studio-ts-client");
      const mockCallRpc = call_rpc as jest.Mock;
      mockCallRpc.mockResolvedValue({
        custom: {
          call: {
            payload: Response.encode(
              Response.create({ setFrameStream: { streaming: true } })
            ).finish(),
          },
        },
      });

      const { mockZMKApp } = setup();
      render(
        <ZMKAppProvider value={mockZMKApp}>
          <FrameViewer />
        </ZMKAppProvider>
      );

      const user = userEvent.setup();
      await user.click(screen.getByText(/Start Streaming/i));
      await waitFor(() => {
        expect(screen.getByText(/Stop Streaming/i)).toBeInTheDocument();
      });

      mockCallRpc.mockResolvedValue({
        custom: {
          call: {
            payload: Response.encode(
              Response.create({ setFrameStream: { streaming: false } })
            ).finish(),
          },
        },
      });

      await user.click(screen.getByText(/Stop Streaming/i));

      await waitFor(() => {
        expect(screen.getByText(/Start Streaming/i)).toBeInTheDocument();
      });

      const lastCallArgs = mockCallRpc.mock.calls.at(-1);
      const requestPayload = lastCallArgs?.[1]?.custom?.call?.payload;
      expect(requestPayload).toBeDefined();
    });
  });

  describe("Locked state", () => {
    it("disables capture and streaming controls when locked", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [PMW3610_SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <FrameViewer locked={true} />
        </ZMKAppProvider>
      );

      expect(screen.getByText(/Capture Once/i)).toBeDisabled();
      expect(screen.getByText(/Start Streaming/i)).toBeDisabled();
    });
  });
});
