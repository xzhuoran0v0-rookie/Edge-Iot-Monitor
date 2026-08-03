import { mockTemp, mockHum, mockStatus, mockReasoning } from "./mock";
import { enToZh } from "./dict";
import StatusBar from "./components/StatusBar";
import MiniChart from "./components/MiniChart";
import OledPrompt from "./components/OledPrompt";

function formatTime(iso: string): string {
  return new Date(iso).toLocaleTimeString("zh-CN", {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  });
}

const trendLabel: Record<string, string> = {
  stable: "稳定",
  rising: "上升",
  falling: "下降",
};

export default function App() {
  const latestTemp = mockTemp[mockTemp.length - 1];
  const latestHum = mockHum[mockHum.length - 1];

  return (
    <div className="app">
      <header className="header">
        <h1>边缘环境监测台</h1>
        <span className="subtitle">ESP32-S3 · SHT30 · 边缘推理</span>
      </header>

      {/* Row 1: live values + device status */}
      <div className="grid">
        <div className="card">
          <div className="card-title">实时数据</div>
          <div className="stat-row">
            <div className="stat">
              <span className="stat-value">{latestTemp.value}°C</span>
              <span className="stat-label">温度</span>
            </div>
            <div className="stat">
              <span className="stat-value">{latestHum.value}%</span>
              <span className="stat-label">湿度</span>
            </div>
          </div>
          <div className="timestamp" style={{ marginTop: 12 }}>
            更新于 {formatTime(latestTemp.server_timestamp)}
          </div>
        </div>

        <div className="card">
          <div className="card-title">设备状态</div>
          <StatusBar status={mockStatus} />
          <div className="timestamp" style={{ marginTop: 12 }}>
            {mockStatus.online ? "设备在线" : "设备离线"} · 最后上报{" "}
            {formatTime(mockStatus.last_seen)}
          </div>
        </div>
      </div>

      {/* Row 2: charts */}
      <div className="grid">
        <div className="card">
          <div className="card-title">温度趋势（60 分钟）</div>
          <MiniChart data={mockTemp} color="#2563eb" />
        </div>
        <div className="card">
          <div className="card-title">湿度趋势（60 分钟）</div>
          <MiniChart data={mockHum} color="#16a34a" />
        </div>
      </div>

      {/* Row 3: edge reasoning */}
      <div className="grid">
        <div className="card grid-full">
          <div className="card-title">边缘推理</div>
          <div className="reasoning-message">{enToZh(mockReasoning.message)}</div>
          <div className="reasoning-meta">
            <span>基线温度 {mockReasoning.baseline_temp}°C</span>
            <span>基线湿度 {mockReasoning.baseline_hum}%</span>
            <span className={`trend-badge ${mockReasoning.trend}`}>
              {trendLabel[mockReasoning.trend]}
            </span>
            <span>{formatTime(mockReasoning.timestamp)}</span>
          </div>
        </div>
      </div>

      {/* Row 4: OLED prompt */}
      <OledPrompt />
    </div>
  );
}
