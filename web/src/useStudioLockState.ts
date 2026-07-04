import { useContext, useEffect, useState } from "react";
import { ZMKAppContext } from "@cormoran/zmk-studio-react-hook";
import { MetaError } from "@zmkfirmware/zmk-studio-ts-client";
import { ErrorConditions } from "@zmkfirmware/zmk-studio-ts-client/meta";
import { LockState } from "@zmkfirmware/zmk-studio-ts-client/core";

/**
 * Tracks ZMK Studio's core lock state for the connected device.
 *
 * The `cormoran__pmw3610` subsystem is secured (Phase E): every RPC call
 * against it (and the settings subsystem) fails with an `UNLOCK_REQUIRED`
 * meta error while Studio is locked. This hook subscribes to core lock
 * notifications (`onNotification({type: "core", ...})`) so the UI can show
 * a banner and disable controls proactively, without waiting for a call to
 * fail first.
 *
 * There is no RPC to *query* the initial lock state upfront in this
 * react-hook's current API (`zmk.core.Request` only has getDeviceInfo /
 * getLockState / lock / resetSettings, and getLockState is not currently
 * plumbed by useZMKApp()) -- so the initial state defaults to "unknown"
 * (treated as unlocked/optimistic) until either a lock notification arrives
 * or an RPC call fails with UNLOCK_REQUIRED (see `isUnlockRequiredError`
 * below, used by callers to react to that case too).
 */
export function useStudioLockState(): {
  locked: boolean;
} {
  const zmkApp = useContext(ZMKAppContext);
  const [locked, setLocked] = useState(false);

  useEffect(() => {
    if (!zmkApp) return;
    // Reset to the optimistic default whenever the connection changes (e.g.
    // reconnecting to a different device should not keep a stale locked
    // banner around). This mirrors an external system (the connection)
    // rather than deriving from props/state, so a direct setState here is
    // intentional -- see react-hooks/set-state-in-effect's rationale.
    // eslint-disable-next-line react-hooks/set-state-in-effect
    setLocked(false);
    return zmkApp.onNotification({
      type: "core",
      callback: (notification) => {
        if (notification.lockStateChanged !== undefined) {
          setLocked(
            notification.lockStateChanged ===
              LockState.ZMK_STUDIO_CORE_LOCK_STATE_LOCKED
          );
        }
      },
    });
  }, [zmkApp, zmkApp?.state.connection]);

  return { locked };
}

/** True if `error` is the MetaError raised by call_rpc() for a secured RPC
 * call made while ZMK Studio is locked (zmk.meta.ErrorConditions.UNLOCK_REQUIRED). */
export function isUnlockRequiredError(error: unknown): boolean {
  return (
    error instanceof MetaError &&
    error.condition === ErrorConditions.UNLOCK_REQUIRED
  );
}
