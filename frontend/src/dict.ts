const ZH_TO_EN: Record<string, string> = {
  "温度正常": "TEMP NORMAL",
  "温度过高": "TEMP HIGH",
  "温度过低": "TEMP LOW",
  "湿度正常": "HUMIDITY NORMAL",
  "湿度过高": "HUMIDITY HIGH",
  "湿度过低": "HUMIDITY LOW",
  "环境正常": "ENV NORMAL",
  "环境异常": "ENV ABNORMAL",
  "传感器离线": "SENSOR OFFLINE",
  "传感器漂移": "SENSOR DRIFT",
  "网络断开": "NETWORK DOWN",
  "网络恢复": "NETWORK OK",
  "设备重启": "DEVICE REBOOT",
  "请注意": "ATTENTION",
  "警告": "WARNING",
  "紧急": "URGENT",
  "正常运行": "RUNNING OK",
  "待机中": "STANDBY",
  "开始采集": "START SAMPLING",
  "停止采集": "STOP SAMPLING",
  "校准中": "CALIBRATING",
  "固件升级": "FW UPDATE",
  "你好": "HELLO",
  "测试": "TEST",
  "稳定": "STABLE",
  "上升": "RISING",
  "下降": "FALLING",
  "异常": "ANOMALY",
  "正常": "NORMAL",
  "环境参数在正常范围内，无异常波动": "ENV params within normal range, no anomalies",
};

const EN_TO_ZH: Record<string, string> = {};
for (const [zh, en] of Object.entries(ZH_TO_EN)) {
  EN_TO_ZH[en.toUpperCase()] = zh;
}

export function zhToEn(text: string): string | null {
  const trimmed = text.trim();
  if (ZH_TO_EN[trimmed]) return ZH_TO_EN[trimmed];
  for (const [zh, en] of Object.entries(ZH_TO_EN)) {
    if (trimmed.includes(zh)) {
      return trimmed.replace(zh, en);
    }
  }
  return null;
}

export function enToZh(text: string): string {
  const upper = text.trim().toUpperCase();
  if (EN_TO_ZH[upper]) return EN_TO_ZH[upper];
  let result = text;
  const sorted = Object.entries(EN_TO_ZH).sort((a, b) => b[0].length - a[0].length);
  for (const [en, zh] of sorted) {
    const re = new RegExp(en.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"), "gi");
    result = result.replace(re, zh);
  }
  return result;
}
