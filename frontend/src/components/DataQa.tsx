import { useState, useCallback } from "react";
import { askBackend } from "../api";

type QaState =
  | { step: "idle" }
  | { step: "loading" }
  | { step: "done"; answer: string }
  | { step: "error"; message: string };

/**
 * Free-text questions about recent readings.
 *
 * This is a convenience feature outside the monitoring path — the device's own
 * assessment is what drives alerts, and it is computed on-chip. Answers render
 * in the browser, so they come back in Chinese; the OLED has its own command
 * channel with its own ASCII rules.
 */
export default function DataQa() {
  const [input, setInput] = useState("");
  const [state, setState] = useState<QaState>({ step: "idle" });

  const handleAsk = useCallback(async () => {
    const prompt = input.trim();
    if (!prompt) return;

    setState({ step: "loading" });
    try {
      const answer = await askBackend(prompt);
      setState({ step: "done", answer });
    } catch {
      setState({ step: "error", message: "AI 不可用，请检查推理后端是否运行" });
    }
  }, [input]);

  const busy = state.step === "loading";

  return (
    <div className="card prompt-section">
      <div className="card-title">
        数据问答
        <span className="card-note">可选功能，不参与告警判定</span>
      </div>
      <div className="prompt-row">
        <input
          className="prompt-input"
          type="text"
          placeholder="问一下传感器数据…"
          value={input}
          onChange={(e) => setInput(e.target.value)}
          onKeyDown={(e) => e.key === "Enter" && !busy && handleAsk()}
          maxLength={200}
        />
        <button
          className="btn btn-primary"
          onClick={handleAsk}
          disabled={!input.trim() || busy}
        >
          {busy ? "思考中…" : "提问"}
        </button>
      </div>

      {state.step === "done" && (
        <div className="prompt-result">
          <div className="prompt-result-text">{state.answer}</div>
        </div>
      )}

      {state.step === "error" && (
        <div className="prompt-status prompt-error">{state.message}</div>
      )}
    </div>
  );
}
