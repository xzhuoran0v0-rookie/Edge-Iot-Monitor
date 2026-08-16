/** One point of a sensor series, as returned by GET /api/readings. */
export interface SeriesPoint {
  timestamp: string;
  value: number;
}

export interface Series {
  unit: string;
  points: SeriesPoint[];
}

/**
 * The device's own verdict, computed by EdgeReasoner on the ESP32 and shipped
 * with each report. The server stores and forwards it unchanged — nothing here
 * is recomputed in the cloud.
 */
export interface EdgeAssessment {
  state: string;
  severity: "info" | "watch" | "warning" | string;
  confidence: number;
  reason_code: string;
  reason: string;
  /** When this state began. The backend only records transitions. */
  since: string;
}

/** Full dashboard snapshot: GET /api/readings?device_id=...&limit=... */
export interface ReadingsSnapshot {
  status: string;
  device_id: string;
  server_time: string;
  online: boolean;
  last_seen: string | null;
  series: Record<string, Series | undefined>;
  edge: EdgeAssessment | null;
}
