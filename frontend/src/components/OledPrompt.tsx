import { useState, useCallback } from "react";
import { enToZh } from "../dict";

async function askBackend(prompt: string): Promise<string> {
  const res = await fetch("/api/prompt", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ prompt, device_id: "esp32s3-001" }),
  });
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  const data = await res.json();
  return data.answer;
}

type PromptState =
  | { step: "idle" }
  | { step: "loading" }
  | { step: "done"; en: string; zh: string }
  | { step: "error"; message: string };

export default function OledPrompt() {
  const [input, setInput] = useState("");
  const [state, setState] = useState<PromptState>({ step: "idle" });

  const handleAsk = useCallback(async () => {
    const prompt = input.trim();
    if (!prompt) return;

    setState({ step: "loading" });
    try {
      const en = await askBackend(prompt);
      const zh = enToZh(en);
      setState({ step: "done", en, zh });
    } catch {
      setState({ step: "error", message: "AI 不可用，后端未启动" });
    }
  }, [input]);

  const busy = state.step === "loading";

  return (
    <div className="card prompt-section">
      <div className="card-title">数据问答</div>
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
          <div className="prompt-result-text">{state.zh}</div>
          <div className="prompt-result-source">{state.en}</div>
        </div>
      )}

      {state.step === "error" && (
        <div className="prompt-status prompt-error">{state.message}</div>
      )}
    </div>
  );
}
