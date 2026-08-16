import { useState, useCallback } from "react";
import { sendCommand } from "../api";

/**
 * Fixed messages rather than a free-text box.
 *
 * The OLED has no Chinese font and the firmware rejects anything outside
 * printable ASCII, so a free-text field would need a translation or
 * transliteration layer to be usable from a Chinese page. Presets sidestep that
 * entirely: what is sent is exactly what the device can display.
 */
const PRESETS = [
  { label: "显示告警", command: "oled:CHECK ROOM NOW" },
  { label: "显示正常", command: "oled:ALL CLEAR" },
  { label: "清除消息", command: "oled:OK" },
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
      setState({ step: "error", message: "下发失败，请检查后端是否运行" });
    }
  }, []);

  return (
    <div className="card grid-full">
      <div className="card-title">
        设备控制
        <span className="card-note">命令下发到 OLED，设备执行后回执</span>
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
              ? "下发中…"
              : preset.label}
          </button>
        ))}
      </div>

      <div className="control-status">
        {state.step === "done" && `已排队：${state.label}，设备将在下次轮询时取走`}
        {state.step === "error" && (
          <span className="prompt-error">{state.message}</span>
        )}
        {state.step === "idle" && "屏幕文本必须是 ASCII，因此使用固定预设"}
      </div>
    </div>
  );
}
