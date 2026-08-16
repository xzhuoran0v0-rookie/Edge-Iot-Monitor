import type { EdgeAssessment } from "../types";
import { edgeStateZh, edgeReasonZh, SEVERITY_ZH } from "../dict";

interface Props {
  edge: EdgeAssessment | null;
}

function formatTime(iso: string): string {
  return new Date(iso).toLocaleTimeString("zh-CN", {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  });
}

export default function EdgeCard({ edge }: Props) {
  if (!edge) {
    return (
      <div className="card grid-full">
        <div className="card-title">边缘推理 · 芯片内</div>
        <div className="reasoning-message muted">
          设备尚未上报推理结果。
        </div>
      </div>
    );
  }

  return (
    <div className="card grid-full">
      <div className="card-title">
        边缘推理 · 芯片内
        <span className="card-note">ESP32-S3 本地判定，服务端不参与推理</span>
      </div>

      <div className="reasoning-headline">
        <span className={`severity-badge ${edge.severity}`}>
          {SEVERITY_ZH[edge.severity] ?? edge.severity}
        </span>
        <span className="reasoning-state">{edgeStateZh(edge.state)}</span>
      </div>

      <div className="reasoning-message">
        {edgeReasonZh(edge.reason_code, edge.reason)}
      </div>

      <div className="reasoning-meta">
        <span>置信度 {(edge.confidence * 100).toFixed(0)}%</span>
        <span className="mono">{edge.state}</span>
        <span>自 {formatTime(edge.since)} 起</span>
      </div>
    </div>
  );
}
