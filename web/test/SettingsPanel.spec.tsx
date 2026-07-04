import { render, screen } from "@testing-library/react";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import {
  SettingsPanel,
  CUSTOM_SETTINGS_SUBSYSTEM_IDENTIFIER,
  PMW3610_SETTINGS_SUBSYSTEM_ID,
} from "../src/SettingsPanel";

describe("SettingsPanel Component", () => {
  describe("Without the custom-settings subsystem", () => {
    it("shows a warning naming the custom-settings subsystem identifier", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <SettingsPanel />
        </ZMKAppProvider>
      );

      expect(
        screen.getByText(
          new RegExp(
            `Subsystem "${CUSTOM_SETTINGS_SUBSYSTEM_IDENTIFIER}" not found`,
            "i"
          )
        )
      ).toBeInTheDocument();
    });
  });

  describe("With custom-settings but without the pmw3610 settings subsystem", () => {
    it("shows a warning naming the pmw3610 settings subsystem id", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [CUSTOM_SETTINGS_SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <SettingsPanel />
        </ZMKAppProvider>
      );

      expect(
        screen.getByText(
          new RegExp(
            `Subsystem "${PMW3610_SETTINGS_SUBSYSTEM_ID}" not found`,
            "i"
          )
        )
      ).toBeInTheDocument();
    });
  });

  describe("Without ZMKAppContext", () => {
    it("should not render when ZMKAppContext is not provided", () => {
      const { container } = render(<SettingsPanel />);

      expect(container.firstChild).toBeNull();
    });
  });
});
