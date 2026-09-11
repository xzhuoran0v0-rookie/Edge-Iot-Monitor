import type { EdgeAssessment } from "../types";
import { edgeStateLabel, edgeReasonLabel, SEVERITY_LABEL } from "../dict";

interface Props {
  edge: EdgeAssessment | null;
}

function formatTime(iso: string): string {
  return new Date(iso).toLocaleTimeString("en-GB", {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  });
}

export default function EdgeCard({ edge }: Props) {
  if (!edge) {
    return (
      <div className="card grid-full">
        <div className="card-title">On-device reasoning</div>
        <div className="reasoning-message muted">
          The device has not reported an assessment yet.
        </div>
      </div>
    );
  }

  return (
    <div className="card grid-full">
      <div className="card-title">
        On-device reasoning
        <span className="card-note">Decided locally on ESP32-S3; the server records the result</span>
      </div>

      <div className="reasoning-headline">
        <span className={`severity-badge ${edge.severity}`}>
          {SEVERITY_LABEL[edge.severity] ?? edge.severity}
        </span>
        <span className="reasoning-state">{edgeStateLabel(edge.state)}</span>
      </div>

      <div className="reasoning-message">
        {edgeReasonLabel(edge.reason_code, edge.reason)}
      </div>

      <div className="reasoning-meta">
        <span>Confidence {(edge.confidence * 100).toFixed(0)}%</span>
        <span className="mono">{edge.state}</span>
        <span>Since {formatTime(edge.since)}</span>
      </div>
    </div>
  );
}
