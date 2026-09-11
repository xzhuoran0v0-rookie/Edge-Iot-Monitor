import { useState, useCallback } from "react";
import { sendCommand } from "../api";

/**
 * Fixed messages rather than a free-text box.
 *
 * The firmware accepts only printable ASCII. Presets keep commands within
 * the display's supported character set: what is sent is exactly what the
 * device can display.
 */
const PRESETS = [
  { label: "Show alert", command: "oled:CHECK ROOM NOW" },
  { label: "Show all clear", command: "oled:ALL CLEAR" },
  { label: "Clear message", command: "oled:OK" },
] as const;

type SendState =
  | { step: "idle" }
  | { step: "sending"; command: string }
  | { step: "done"; label: string }
  | { step: "error"; message: string };

export default function DeviceControl() {
  const [state, setState] = useState<SendState>({ step: "idle" });

  const send = useCallback(async (label: string, command: string) => {
    setState({ step: "sending", command });
    try {
      await sendCommand(command);
      setState({ step: "done", label });
    } catch {
      setState({ step: "error", message: "Command failed. Check that the backend is running." });
    }
  }, []);

  return (
    <div className="card grid-full">
      <div className="card-title">
        Device control
        <span className="card-note">OLED commands are acknowledged after execution</span>
      </div>

      <div className="control-row">
        {PRESETS.map((preset) => (
          <button
            key={preset.command}
            className="btn"
            onClick={() => send(preset.label, preset.command)}
            disabled={state.step === "sending"}
          >
            {state.step === "sending" && state.command === preset.command
              ? "Sending…"
              : preset.label}
          </button>
        ))}
      </div>

      <div className="control-status">
        {state.step === "done" && `Queued: ${state.label}. The device will fetch it on its next poll.`}
        {state.step === "error" && (
          <span className="prompt-error">{state.message}</span>
        )}
        {state.step === "idle" && "Display text must be ASCII; use the presets above."}
      </div>
    </div>
  );
}
