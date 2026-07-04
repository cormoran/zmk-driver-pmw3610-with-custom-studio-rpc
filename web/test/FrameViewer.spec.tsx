import { render, screen } from "@testing-library/react";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { FrameViewer, PMW3610_SUBSYSTEM_IDENTIFIER } from "../src/FrameViewer";

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
});
