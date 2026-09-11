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
 * in the browser, so they come back in English; the OLED has its own command
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
      setState({ step: "error", message: "AI unavailable. Check that the inference backend is running." });
    }
  }, [input]);

  const busy = state.step === "loading";

  return (
    <div className="card prompt-section">
      <div className="card-title">
        Data Q&A
        <span className="card-note">Optional; does not determine alarms</span>
      </div>
      <div className="prompt-row">
        <input
          className="prompt-input"
          type="text"
          placeholder="Ask about the sensor data…"
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
          {busy ? "Thinking…" : "Ask"}
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
