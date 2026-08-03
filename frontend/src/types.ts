export interface SensorReading {
  device_id: string;
  sensor_type: "temperature" | "humidity";
  value: number;
  unit: string;
  server_timestamp: string;
  device_timestamp?: string;
}

export interface DeviceStatus {
  online: boolean;
  wifi: boolean;
  mqtt: boolean;
  backend: boolean;
  ntp: boolean;
  drift_alert: boolean;
  last_seen: string;
}

export interface EdgeReasoning {
  baseline_temp: number;
  baseline_hum: number;
  trend: "stable" | "rising" | "falling";
  anomaly: boolean;
  message: string;
  timestamp: string;
}
