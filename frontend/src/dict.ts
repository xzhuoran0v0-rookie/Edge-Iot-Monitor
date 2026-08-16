/**
 * EdgeReasoner state codes → Chinese.
 *
 * The state set is closed (see firmware/combined/src/edge_reasoner.cpp), so this
 * is an exact lookup. An unknown code falls through and is displayed as-is
 * rather than being mangled — a new firmware state should look unfamiliar,
 * not wrong.
 */
const EDGE_STATE_ZH: Record<string, string> = {
  // EdgeReasoner (edge_reasoner.cpp)
  WARMUP: "基线学习中",
  NORMAL: "环境正常",
  UNSTABLE: "读数不稳定",
  HEAT_HUMID_RISK: "高温高湿",
  TEMP_RISING: "温度快速上升",
  HUMIDITY_RISING: "湿度快速上升",
  HIGH_TEMPERATURE: "温度偏高",
  HIGH_HUMIDITY: "湿度偏高",
  // AdaptiveBaseline overlay (main.cpp applyAdaptiveAssessment) — these
  // override the states above, so they are the ones that matter most.
  HARD_LIMIT: "超出安全限值",
  BASELINE_SHIFT: "偏离学习基线",
};

const EDGE_REASON_ZH: Record<string, string> = {
  LEARNING_BASELINE: "正在采集本地趋势基线。",
  STABLE_ENVIRONMENT: "温湿度稳定。",
  ERRATIC_SIGNAL: "读数变化过于剧烈，暂不可信。",
  HOT_AND_HUMID: "温度与湿度同时偏高。",
  RAPID_TEMP_RISE: "温度正在快速上升。",
  RAPID_HUMIDITY_RISE: "湿度正在快速上升。",
  TEMP_ABOVE_COMFORT: "温度高于舒适阈值。",
  HUMIDITY_ABOVE_COMFORT: "湿度高于舒适阈值。",
  FIXED_SAFETY_LIMIT: "已越过固定安全限值。",
  TEMP_HUMIDITY_OUTSIDE_BASELINE: "温度与湿度同时超出学习到的正常范围。",
  TEMP_OUTSIDE_BASELINE: "温度超出学习到的正常范围。",
  HUMIDITY_OUTSIDE_BASELINE: "湿度超出学习到的正常范围。",
};

export function edgeStateZh(state: string): string {
  return EDGE_STATE_ZH[state] ?? state;
}

export function edgeReasonZh(reasonCode: string, fallback: string): string {
  return EDGE_REASON_ZH[reasonCode] ?? fallback;
}

export const SEVERITY_ZH: Record<string, string> = {
  info: "正常",
  watch: "关注",
  warning: "警告",
  urgent: "紧急",
};
