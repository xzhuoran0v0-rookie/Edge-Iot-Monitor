import type { ReadingsSnapshot } from "./types";

export const DEVICE_ID = "esp32s3-001";

/** Device reports every 10s (REPORT_INTERVAL_MS); polling faster buys nothing. */
export const POLL_INTERVAL_MS = 5000;

export async function fetchReadings(
  deviceId: string = DEVICE_ID,
  limit = 120,
  signal?: AbortSignal
): Promise<ReadingsSnapshot> {
  const params = new URLSearchParams({ device_id: deviceId, limit: String(limit) });
  const res = await fetch(`/api/readings?${params}`, { signal });
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  return res.json();
}

/**
 * Queue a device command.
 *
 * No API key is sent: `security.command_api_key` must stay empty for the
 * dashboard to use this endpoint, since a key embedded in frontend JavaScript
 * would be readable by anyone loading the page. Setting a key means commands
 * have to go through a server-side route instead.
 */
export async function sendCommand(
  command: string,
  deviceId: string = DEVICE_ID,
  durationMs = 0
): Promise<{ command_id: number }> {
  const res = await fetch("/api/commands", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      device_id: deviceId,
      command,
      duration_ms: durationMs,
    }),
  });
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  return res.json();
}

export async function askBackend(
  prompt: string,
  deviceId: string = DEVICE_ID
): Promise<string> {
  const res = await fetch("/api/prompt", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ prompt, device_id: deviceId }),
  });
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  const data = await res.json();
  return data.answer;
}
