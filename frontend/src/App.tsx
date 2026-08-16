import { useEffect, useRef, useState } from "react";
import type { ReadingsSnapshot } from "./types";
import { fetchReadings, POLL_INTERVAL_MS } from "./api";
import StatusBar from "./components/StatusBar";
import MiniChart from "./components/MiniChart";
import EdgeCard from "./components/EdgeCard";
import DeviceControl from "./components/DeviceControl";
import DataQa from "./components/DataQa";

function formatTime(iso: string): string {
  return new Date(iso).toLocaleTimeString("zh-CN", {
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
        <h1>边缘环境监测台</h1>
        <span className="subtitle">ESP32-S3 · SHT30 · 边缘推理</span>
      </header>

      {!reachable && (
        <div className="banner banner-error">
          无法连接后端服务，显示的是最后一次成功获取的数据。
        </div>
      )}

      {loading ? (
        <div className="card grid-full">
          <div className="reasoning-message muted">正在读取设备数据…</div>
        </div>
      ) : (
        <>
          {/* Row 1: live values + device status */}
          <div className="grid">
            <div className="card">
              <div className="card-title">实时数据</div>
              <div className="stat-row">
                <div className="stat">
                  <span className="stat-value">
                    {temp !== null ? `${temp.toFixed(1)}°C` : "—"}
                  </span>
                  <span className="stat-label">温度</span>
                </div>
                <div className="stat">
                  <span className="stat-value">
                    {hum !== null ? `${hum.toFixed(1)}%` : "—"}
                  </span>
                  <span className="stat-label">湿度</span>
                </div>
              </div>
              <div className="timestamp" style={{ marginTop: 12 }}>
                {snapshot?.last_seen
                  ? `更新于 ${formatTime(snapshot.last_seen)}`
                  : "尚无数据"}
              </div>
            </div>

            <div className="card">
              <div className="card-title">设备状态</div>
              <StatusBar
                backendReachable={reachable}
                deviceOnline={snapshot?.online ?? false}
              />
              <div className="timestamp" style={{ marginTop: 12 }}>
                {snapshot?.online ? "设备在线" : "设备离线"}
                {snapshot?.last_seen
                  ? ` · 最后上报 ${formatTime(snapshot.last_seen)}`
                  : ""}
              </div>
            </div>
          </div>

          {/* Row 2: charts */}
          <div className="grid">
            <div className="card">
              <div className="card-title">温度趋势</div>
              <MiniChart data={tempPoints} color="#2563eb" />
            </div>
            <div className="card">
              <div className="card-title">湿度趋势</div>
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
