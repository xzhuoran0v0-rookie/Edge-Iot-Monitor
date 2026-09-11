/**
 * 本地网页看板
 *
 * 状态：可运行，非交付重点。
 *
 * - 数据来源：轮询后端 /api/readings，轮询间隔见 src/api.ts
 * - 部署范围：仅本地网页，需与设备和后端处于同一网段，未做公网部署
 * - components/DataQa.tsx 依赖后端的 AI 叙述模块。该模块默认关闭，
 *   关闭时该面板不返回内容，其余面板不受影响
 *
 * 边界：本页只展示设备上报的结论，不做任何判定。状态、严重度、置信度与原因码
 * 均由 ESP32-S3 计算，前端与后端都不重新计算。
 */

import { useEffect, useRef, useState } from "react";
import type { ReadingsSnapshot } from "./types";
import { fetchReadings, POLL_INTERVAL_MS } from "./api";
import StatusBar from "./components/StatusBar";
import MiniChart from "./components/MiniChart";
import EdgeCard from "./components/EdgeCard";
import DeviceControl from "./components/DeviceControl";
import DataQa from "./components/DataQa";

function formatTime(iso: string): string {
  return new Date(iso).toLocaleTimeString("en-GB", {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  });
}

function latestOf(snapshot: ReadingsSnapshot, key: string): number | null {
  const points = snapshot.series[key]?.points;
  if (!points || points.length === 0) return null;
  return points[points.length - 1].value;
}

export default function App() {
  const [snapshot, setSnapshot] = useState<ReadingsSnapshot | null>(null);
  const [reachable, setReachable] = useState(true);
  const [loading, setLoading] = useState(true);
  // Keep the last good snapshot on screen through a transient failure; the
  // status pill goes red, but the charts do not blank out and come back.
  const mounted = useRef(true);

  useEffect(() => {
    mounted.current = true;
    const controller = new AbortController();

    async function poll() {
      try {
        const data = await fetchReadings(undefined, 120, controller.signal);
        if (!mounted.current) return;
        setSnapshot(data);
        setReachable(true);
      } catch (err) {
        if (!mounted.current || controller.signal.aborted) return;
        setReachable(false);
      } finally {
        if (mounted.current) setLoading(false);
      }
    }

    poll();
    const timer = setInterval(poll, POLL_INTERVAL_MS);
    return () => {
      mounted.current = false;
      controller.abort();
      clearInterval(timer);
    };
  }, []);

  const temp = snapshot ? latestOf(snapshot, "temperature") : null;
  const hum = snapshot ? latestOf(snapshot, "humidity") : null;
  const tempPoints = snapshot?.series.temperature?.points ?? [];
  const humPoints = snapshot?.series.humidity?.points ?? [];

  return (
    <div className="app">
      <header className="header">
        <h1>Edge Environment Monitor</h1>
        <span className="subtitle">ESP32-S3 · SHT30 · On-device reasoning</span>
      </header>

      {!reachable && (
        <div className="banner banner-error">
          Cannot reach the backend. Showing the last successfully received data.
        </div>
      )}

      {loading ? (
        <div className="card grid-full">
          <div className="reasoning-message muted">Loading device data…</div>
        </div>
      ) : (
        <>
          {/* Row 1: live values + device status */}
          <div className="grid">
            <div className="card">
              <div className="card-title">Live readings</div>
              <div className="stat-row">
                <div className="stat">
                  <span className="stat-value">
                    {temp !== null ? `${temp.toFixed(1)}°C` : "—"}
                  </span>
                  <span className="stat-label">Temperature</span>
                </div>
                <div className="stat">
                  <span className="stat-value">
                    {hum !== null ? `${hum.toFixed(1)}%` : "—"}
                  </span>
                  <span className="stat-label">Humidity</span>
                </div>
              </div>
              <div className="timestamp" style={{ marginTop: 12 }}>
                {snapshot?.last_seen
                  ? `Updated at ${formatTime(snapshot.last_seen)}`
                  : "No data yet"}
              </div>
            </div>

            <div className="card">
              <div className="card-title">Device status</div>
              <StatusBar
                backendReachable={reachable}
                deviceOnline={snapshot?.online ?? false}
              />
              <div className="timestamp" style={{ marginTop: 12 }}>
                {snapshot?.online ? "Device online" : "Device offline"}
                {snapshot?.last_seen
                  ? ` · Last report ${formatTime(snapshot.last_seen)}`
                  : ""}
              </div>
            </div>
          </div>

          {/* Row 2: charts */}
          <div className="grid">
            <div className="card">
              <div className="card-title">Temperature trend</div>
              <MiniChart data={tempPoints} color="#2563eb" />
            </div>
            <div className="card">
              <div className="card-title">Humidity trend</div>
              <MiniChart data={humPoints} color="#16a34a" />
            </div>
          </div>

          {/* Row 3: on-device reasoning */}
          <div className="grid">
            <EdgeCard edge={snapshot?.edge ?? null} />
          </div>

          {/* Row 4: command downlink */}
          <div className="grid">
            <DeviceControl />
          </div>

          {/* Row 5: data Q&A */}
          <DataQa />
        </>
      )}
    </div>
  );
}
