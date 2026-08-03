import type { SensorReading, DeviceStatus, EdgeReasoning } from "./types";

const NOW = Date.now();
const INTERVAL = 60_000;

function ts(offsetMinutes: number): string {
  return new Date(NOW - offsetMinutes * INTERVAL).toISOString();
}

function genReadings(
  type: "temperature" | "humidity",
  base: number,
  variance: number,
  unit: string,
  count: number
): SensorReading[] {
  return Array.from({ length: count }, (_, i) => ({
    device_id: "esp32s3-001",
    sensor_type: type,
    value: +(base + (Math.random() - 0.5) * variance).toFixed(1),
    unit,
    server_timestamp: ts(count - 1 - i),
  }));
}

export const mockTemp = genReadings("temperature", 26.5, 3, "°C", 60);
export const mockHum = genReadings("humidity", 58, 10, "%", 60);

export const mockStatus: DeviceStatus = {
  online: true,
  wifi: true,
  mqtt: true,
  backend: true,
  ntp: true,
  drift_alert: false,
  last_seen: ts(0),
};

export const mockReasoning: EdgeReasoning = {
  baseline_temp: 26.5,
  baseline_hum: 58,
  trend: "stable",
  anomaly: false,
  message: "ENV params within normal range, no anomalies",
  timestamp: ts(0),
};
