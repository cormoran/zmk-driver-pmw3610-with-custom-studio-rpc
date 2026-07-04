import { useContext, useEffect, useState } from "react";
import {
  ZMKCustomSubsystem,
  ZMKAppContext,
} from "@cormoran/zmk-studio-react-hook";
import {
  Notification as SettingsNotification,
  Request,
  Response,
  Setting,
  SettingNotificationKind,
  SettingWriteMode,
} from "./proto/cormoran/zmk/custom_settings/custom_settings";

/**
 * Generic custom-settings editor for the PMW3610 sensor's own settings
 * subsystem ("cormoran__pmw3610"). Talks to zmk-feature-custom-settings'
 * separate Studio RPC subsystem (identifier "cormoran_custom_settings",
 * see dependencies/zmk-feature-custom-settings/src/studio/custom_settings_handler.c),
 * not the pmw3610 subsystem itself -- settings get/set/save/discard/reset
 * is implemented generically there.
 *
 * Adapted from zmk-feature-custom-settings' own reference web app
 * (App.tsx's RPCTestSection), trimmed to only the pieces relevant to a
 * single, known custom_subsystem_id (no free-form subsystem/key filters,
 * no JSON import/export panel, no array push/pop -- this module's
 * settings are all scalar).
 */

export const CUSTOM_SETTINGS_SUBSYSTEM_IDENTIFIER = "cormoran_custom_settings";
export const PMW3610_SETTINGS_SUBSYSTEM_ID = "cormoran__pmw3610";

const LIST_NOTIFICATION_TIMEOUT_MS = 750;
const LIST_REQUEST_TIMEOUT_MS = 5000;
const SOURCE_LOCAL = 0;

type EditingValue = { key: string; text: string };

export function SettingsPanel() {
  const zmkApp = useContext(ZMKAppContext);
  const [settings, setSettings] = useState<Setting[]>([]);
  const [editing, setEditing] = useState<EditingValue | null>(null);
  const [writeMode, setWriteMode] = useState<SettingWriteMode>(
    SettingWriteMode.SETTING_WRITE_MODE_MEMORY
  );
  const [isLoading, setIsLoading] = useState(false);
  const [statusMessage, setStatusMessage] = useState<string | null>(null);

  const settingsSubsystem = zmkApp?.findSubsystem(
    CUSTOM_SETTINGS_SUBSYSTEM_IDENTIFIER
  );
  const pmw3610Subsystem = zmkApp?.findSubsystem("cormoran__pmw3610");

  const callCustomRequest = async (request: Request): Promise<Response> => {
    const connection = zmkApp?.state.connection;
    if (!connection || !settingsSubsystem) {
      throw new Error("Custom settings subsystem is not available");
    }

    const service = new ZMKCustomSubsystem(connection, settingsSubsystem.index);
    const payload = Request.encode(request).finish();
    const responsePayload = await service.callRPC(payload);
    if (!responsePayload) {
      throw new Error("Empty response");
    }
    return Response.decode(responsePayload);
  };

  const loadSettings = async () => {
    if (!zmkApp || !settingsSubsystem || pmw3610Subsystem === null) {
      return;
    }
    if (pmw3610Subsystem === undefined) {
      return;
    }

    setIsLoading(true);
    setStatusMessage(null);

    const collected: Setting[] = [];
    let expectedCount: number | undefined;
    let quietTimeout: ReturnType<typeof setTimeout> | undefined;
    let resolveList: () => void = () => {};
    let isComplete = false;

    const listComplete = new Promise<void>((resolve) => {
      resolveList = resolve;
    });

    const completeList = () => {
      if (isComplete) return;
      isComplete = true;
      if (quietTimeout) clearTimeout(quietTimeout);
      resolveList();
    };

    const scheduleQuietResolve = () => {
      if (quietTimeout) clearTimeout(quietTimeout);
      quietTimeout = setTimeout(completeList, LIST_NOTIFICATION_TIMEOUT_MS);
    };

    const unsubscribe = zmkApp.onNotification({
      type: "custom",
      subsystemIndex: settingsSubsystem.index,
      callback: (customNotification) => {
        let notification;
        try {
          notification = SettingsNotification.decode(
            customNotification.payload
          );
        } catch {
          return;
        }
        if (
          notification.setting?.kind ===
            SettingNotificationKind.SETTING_NOTIFICATION_KIND_LIST_ITEM &&
          notification.setting.setting
        ) {
          if (
            notification.setting.setting.customSubsystemIndex ===
            pmw3610Subsystem.index
          ) {
            collected.push(notification.setting.setting);
          }
          if (
            expectedCount !== undefined &&
            collected.length >= expectedCount
          ) {
            completeList();
          } else {
            scheduleQuietResolve();
          }
        }
      },
    });

    try {
      const resp = await withTimeout(
        callCustomRequest(
          Request.create({
            listSettings: {
              scope: {
                customSubsystemIndex: pmw3610Subsystem.index,
                source: SOURCE_LOCAL,
              },
              requireMeta: true,
            },
          })
        ),
        LIST_REQUEST_TIMEOUT_MS,
        "List request timed out"
      );
      if (resp.error) {
        throw new Error(resp.error.message || "List failed");
      }
      expectedCount = resp.status?.affectedCount ?? 0;
      if (collected.length >= expectedCount) {
        completeList();
      } else {
        scheduleQuietResolve();
      }
      await listComplete;
      setSettings(collected);
      setStatusMessage(`Loaded ${collected.length} setting(s)`);
    } catch (error) {
      setStatusMessage(
        `Failed to load settings: ${error instanceof Error ? error.message : "Unknown error"}`
      );
    } finally {
      unsubscribe();
      if (quietTimeout) clearTimeout(quietTimeout);
      setIsLoading(false);
    }
  };

  useEffect(() => {
    // Fire-and-forget: loadSettings manages its own loading/error state
    // asynchronously (react-hooks/set-state-in-effect flags the eventual
    // setState inside it, which is intentional -- there is no synchronous
    // render-phase setState here).
    // eslint-disable-next-line react-hooks/set-state-in-effect
    void loadSettings();
    // Reload once the settings subsystems become available after connect.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [settingsSubsystem?.index, pmw3610Subsystem?.index]);

  const startEditing = (setting: Setting) => {
    setEditing({ key: setting.key, text: formatEditableValue(setting) });
  };

  const cancelEditing = () => setEditing(null);

  const writeSetting = async (setting: Setting) => {
    if (!editing || editing.key !== setting.key || !pmw3610Subsystem) {
      return;
    }

    setIsLoading(true);
    setStatusMessage(null);
    try {
      const value = parseEditableValue(setting, editing.text);
      const resp = await callCustomRequest(
        Request.create({
          writeSetting: {
            setting: {
              customSubsystemIndex: pmw3610Subsystem.index,
              key: setting.key,
              source: SOURCE_LOCAL,
            },
            value,
            mode: writeMode,
          },
        })
      );
      if (resp.error) {
        throw new Error(resp.error.message);
      }
      setStatusMessage(`Wrote ${setting.key}`);
      setEditing(null);
      await loadSettings();
    } catch (error) {
      setStatusMessage(
        `Write failed: ${error instanceof Error ? error.message : "Unknown error"}`
      );
    } finally {
      setIsLoading(false);
    }
  };

  const runScopeAction = async (
    action: "saveSettings" | "discardSettings" | "resetSettings",
    label: string
  ) => {
    if (!pmw3610Subsystem) return;

    setIsLoading(true);
    setStatusMessage(null);
    try {
      const resp = await callCustomRequest(
        Request.create({
          [action]: {
            scope: {
              customSubsystemIndex: pmw3610Subsystem.index,
              source: SOURCE_LOCAL,
            },
          },
        })
      );
      if (resp.error) {
        throw new Error(resp.error.message);
      }
      setStatusMessage(
        `${label}: ${resp.status?.affectedCount ?? 0} setting(s)`
      );
      await loadSettings();
    } catch (error) {
      setStatusMessage(
        `${label} failed: ${error instanceof Error ? error.message : "Unknown error"}`
      );
    } finally {
      setIsLoading(false);
    }
  };

  if (!zmkApp) return null;

  if (!settingsSubsystem) {
    return (
      <section className="card">
        <h2>Settings</h2>
        <div className="warning-message">
          <p>
            Subsystem &quot;{CUSTOM_SETTINGS_SUBSYSTEM_IDENTIFIER}&quot; not
            found. Enable CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC in the firmware
            to manage settings here.
          </p>
        </div>
      </section>
    );
  }

  if (!pmw3610Subsystem) {
    return (
      <section className="card">
        <h2>Settings</h2>
        <div className="warning-message">
          <p>
            Subsystem &quot;{PMW3610_SETTINGS_SUBSYSTEM_ID}&quot; not found.
            Enable CONFIG_ZMK_PMW3610_CUSTOM_SETTINGS in the firmware to manage
            settings here.
          </p>
        </div>
      </section>
    );
  }

  return (
    <section className="card">
      <h2>Settings</h2>
      <p>
        Sensor parameters (subsystem &quot;{PMW3610_SETTINGS_SUBSYSTEM_ID}
        &quot;).
      </p>

      <div className="toolbar">
        <button
          className="btn"
          disabled={isLoading}
          onClick={() => void loadSettings()}
        >
          Refresh
        </button>
        <label htmlFor="write-mode-select" className="inline-label">
          Write mode
        </label>
        <select
          id="write-mode-select"
          value={writeMode}
          onChange={(e) => setWriteMode(Number(e.target.value))}
        >
          <option value={SettingWriteMode.SETTING_WRITE_MODE_MEMORY}>
            memory
          </option>
          <option value={SettingWriteMode.SETTING_WRITE_MODE_PERSIST}>
            persist
          </option>
        </select>
        <button
          className="btn"
          disabled={isLoading}
          onClick={() => void runScopeAction("saveSettings", "Save")}
        >
          Save
        </button>
        <button
          className="btn"
          disabled={isLoading}
          onClick={() => void runScopeAction("discardSettings", "Discard")}
        >
          Discard
        </button>
        <button
          className="btn btn-danger"
          disabled={isLoading}
          onClick={() => void runScopeAction("resetSettings", "Reset")}
        >
          Reset
        </button>
      </div>

      {settings.length > 0 ? (
        <div className="settings-table-wrap">
          <table className="settings-table">
            <thead>
              <tr>
                <th>Key</th>
                <th>Value</th>
                <th>Constraint</th>
                <th>Unsaved</th>
                <th></th>
              </tr>
            </thead>
            <tbody>
              {settings.map((setting) => (
                <tr key={setting.key}>
                  <td>{setting.key}</td>
                  <td>
                    {editing?.key === setting.key ? (
                      <input
                        aria-label={`${setting.key} value`}
                        value={editing.text}
                        onChange={(e) =>
                          setEditing({ key: setting.key, text: e.target.value })
                        }
                      />
                    ) : (
                      formatDisplayValue(setting)
                    )}
                  </td>
                  <td>{formatConstraint(setting)}</td>
                  <td>{setting.hasUnsavedValue ? "yes" : "no"}</td>
                  <td>
                    {editing?.key === setting.key ? (
                      <div className="toolbar">
                        <button
                          className="btn btn-primary"
                          disabled={isLoading}
                          onClick={() => void writeSetting(setting)}
                        >
                          Write
                        </button>
                        <button
                          className="btn"
                          disabled={isLoading}
                          onClick={cancelEditing}
                        >
                          Cancel
                        </button>
                      </div>
                    ) : (
                      <button
                        className="btn"
                        disabled={isLoading}
                        onClick={() => startEditing(setting)}
                      >
                        Edit
                      </button>
                    )}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      ) : (
        <p className="empty-message">
          {isLoading ? "Loading settings..." : "No settings loaded yet."}
        </p>
      )}

      {statusMessage && (
        <div className="response-box">
          <pre>{statusMessage}</pre>
        </div>
      )}
    </section>
  );
}

function formatDisplayValue(setting: Setting): string {
  const value = setting.value;
  if (!value) return "(hidden)";
  if (value.int32Value !== undefined) return `${value.int32Value}`;
  if (value.boolValue !== undefined) return value.boolValue ? "true" : "false";
  if (value.stringValue !== undefined) return value.stringValue;
  if (value.bytesValue !== undefined)
    return Array.from(value.bytesValue).join(",");
  return "";
}

function formatEditableValue(setting: Setting): string {
  const value = setting.value;
  if (!value) return "";
  if (value.boolValue !== undefined) return value.boolValue ? "true" : "false";
  return formatDisplayValue(setting);
}

function parseEditableValue(setting: Setting, text: string) {
  const value = setting.value;
  if (value?.boolValue !== undefined) {
    return {
      boolValue: text.trim().toLowerCase() === "true" || text.trim() === "1",
    };
  }
  if (value?.int32Value !== undefined) {
    const parsed = Number.parseInt(text, 10);
    if (Number.isNaN(parsed)) {
      throw new Error(`"${text}" is not a valid integer`);
    }
    return { int32Value: parsed };
  }
  if (value?.stringValue !== undefined) {
    return { stringValue: text };
  }
  throw new Error(`Unsupported value type for ${setting.key}`);
}

function formatConstraint(setting: Setting): string {
  const constraints = setting.meta?.constraints ?? [];
  const range = constraints.find((c) => c.range !== undefined)?.range;
  if (range && range.min && range.max) {
    const min =
      range.min.int32Value ?? range.min.boolValue ?? range.min.stringValue;
    const max =
      range.max.int32Value ?? range.max.boolValue ?? range.max.stringValue;
    return `${min} - ${max}`;
  }
  return "-";
}

async function withTimeout<T>(
  promise: Promise<T>,
  timeoutMs: number,
  message: string
): Promise<T> {
  let timeout: ReturnType<typeof setTimeout> | undefined;
  try {
    return await Promise.race([
      promise,
      new Promise<never>((_, reject) => {
        timeout = setTimeout(() => reject(new Error(message)), timeoutMs);
      }),
    ]);
  } finally {
    if (timeout) clearTimeout(timeout);
  }
}
