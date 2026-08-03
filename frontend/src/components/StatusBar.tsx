import type { DeviceStatus } from "../types";

interface Props {
  status: DeviceStatus;
}

const labels: Record<string, string> = {
  wifi: "WiFi",
  mqtt: "MQTT",
  backend: "后端",
  ntp: "NTP",
};

export default function StatusBar({ status }: Props) {
  const items = (["wifi", "mqtt", "backend", "ntp"] as const).map((key) => ({
    key,
    label: labels[key],
    ok: status[key],
  }));

  return (
    <div className="status-grid">
      {items.map((item) => (
        <span key={item.key} className={`status-pill ${item.ok ? "ok" : "err"}`}>
          <span className="status-dot" />
          {item.label}
        </span>
      ))}
      {status.drift_alert && (
        <span className="status-pill err">
          <span className="status-dot" />
          传感器漂移
        </span>
      )}
    </div>
  );
}
