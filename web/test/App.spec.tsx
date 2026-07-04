import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { setupZMKMocks } from "@cormoran/zmk-studio-react-hook/testing";
import { LockState } from "@zmkfirmware/zmk-studio-ts-client/core";
import App from "../src/App";

// Mock the ZMK client
jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({
  create_rpc_connection: jest.fn(),
  call_rpc: jest.fn(),
}));

jest.mock("@zmkfirmware/zmk-studio-ts-client/transport/serial", () => ({
  connect: jest.fn(),
}));

describe("App Component", () => {
  describe("Basic Rendering", () => {
    it("should render the application header", () => {
      render(<App />);

      expect(screen.getByText(/PMW3610 Trackball/i)).toBeInTheDocument();
      expect(screen.getByText(/frame viewer/i)).toBeInTheDocument();
    });

    it("should render connection button when disconnected", () => {
      render(<App />);

      expect(screen.getByText(/Connect Serial/i)).toBeInTheDocument();
    });

    it("should render footer", () => {
      render(<App />);

      expect(screen.getByText(/PMW3610 Module/i)).toBeInTheDocument();
    });
  });

  describe("Connection Flow", () => {
    let mocks: ReturnType<typeof setupZMKMocks>;

    beforeEach(() => {
      mocks = setupZMKMocks();
    });

    it("should connect to device and render the feature cards", async () => {
      mocks.mockSuccessfulConnection({
        deviceName: "Test Keyboard",
        subsystems: ["cormoran__pmw3610", "cormoran_custom_settings"],
      });

      const { connect: serial_connect } =
        await import("@zmkfirmware/zmk-studio-ts-client/transport/serial");
      (serial_connect as jest.Mock).mockResolvedValue(mocks.mockTransport);

      render(<App />);

      expect(screen.getByText(/Connect Serial/i)).toBeInTheDocument();

      const user = userEvent.setup();
      const connectButton = screen.getByText(/Connect Serial/i);
      await user.click(connectButton);

      await waitFor(() => {
        expect(
          screen.getByText(/Connected to: Test Keyboard/i)
        ).toBeInTheDocument();
      });

      expect(screen.getByText(/Disconnect/i)).toBeInTheDocument();
      expect(
        screen.getByRole("heading", { name: /Sensor/i })
      ).toBeInTheDocument();
      expect(
        screen.getByRole("heading", { name: /Diagnostics/i })
      ).toBeInTheDocument();
      expect(
        screen.getByRole("heading", { name: /Settings/i })
      ).toBeInTheDocument();
      expect(
        screen.getByRole("heading", { name: /Frame Viewer/i })
      ).toBeInTheDocument();
    });
  });

  describe("Studio lock state", () => {
    let mocks: ReturnType<typeof setupZMKMocks>;

    beforeEach(() => {
      mocks = setupZMKMocks();
    });

    it("shows a locked banner when a core lock-state notification arrives", async () => {
      mocks.mockSuccessfulConnection({
        deviceName: "Test Keyboard",
        subsystems: ["cormoran__pmw3610", "cormoran_custom_settings"],
        notifications: [
          {
            core: {
              lockStateChanged: LockState.ZMK_STUDIO_CORE_LOCK_STATE_LOCKED,
            },
          },
        ],
      });

      const { connect: serial_connect } =
        await import("@zmkfirmware/zmk-studio-ts-client/transport/serial");
      (serial_connect as jest.Mock).mockResolvedValue(mocks.mockTransport);

      render(<App />);

      const user = userEvent.setup();
      await user.click(screen.getByText(/Connect Serial/i));

      await waitFor(() => {
        expect(screen.getByRole("alert")).toHaveTextContent(/locked/i);
      });
    });

    it("does not show a locked banner while unlocked", async () => {
      mocks.mockSuccessfulConnection({
        deviceName: "Test Keyboard",
        subsystems: ["cormoran__pmw3610", "cormoran_custom_settings"],
        notifications: [
          {
            core: {
              lockStateChanged: LockState.ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED,
            },
          },
        ],
      });

      const { connect: serial_connect } =
        await import("@zmkfirmware/zmk-studio-ts-client/transport/serial");
      (serial_connect as jest.Mock).mockResolvedValue(mocks.mockTransport);

      render(<App />);

      const user = userEvent.setup();
      await user.click(screen.getByText(/Connect Serial/i));

      await waitFor(() => {
        expect(
          screen.getByRole("heading", { name: /Frame Viewer/i })
        ).toBeInTheDocument();
      });
      expect(screen.queryByRole("alert")).not.toBeInTheDocument();
    });
  });
});
