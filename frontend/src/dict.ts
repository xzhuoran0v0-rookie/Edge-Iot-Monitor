/**
 * EdgeReasoner state codes → English labels.
 *
 * The state set is closed (see firmware/combined/src/edge_reasoner.cpp), so this
 * is an exact lookup. An unknown code falls through and is displayed as-is
 * rather than being mangled — a new firmware state should look unfamiliar,
 * not wrong.
 */
const EDGE_STATE_LABEL: Record<string, string> = {
  // EdgeReasoner (edge_reasoner.cpp)
  WARMUP: "Learning baseline",
  NORMAL: "Normal environment",
  UNSTABLE: "Unstable readings",
  HEAT_HUMID_RISK: "Hot and humid",
  TEMP_RISING: "Temperature rising rapidly",
  HUMIDITY_RISING: "Humidity rising rapidly",
  HIGH_TEMPERATURE: "High temperature",
  HIGH_HUMIDITY: "High humidity",
  // AdaptiveBaseline overlay (main.cpp applyAdaptiveAssessment) — these
  // override the states above, so they are the ones that matter most.
  HARD_LIMIT: "Safety limit exceeded",
  BASELINE_SHIFT: "Baseline shift",
};

const EDGE_REASON_LABEL: Record<string, string> = {
  LEARNING_BASELINE: "Collecting a local trend baseline.",
  STABLE_ENVIRONMENT: "Temperature and humidity are stable.",
  ERRATIC_SIGNAL: "Readings are too erratic to trust.",
  HOT_AND_HUMID: "Temperature and humidity are both high.",
  RAPID_TEMP_RISE: "Temperature is rising rapidly.",
  RAPID_HUMIDITY_RISE: "Humidity is rising rapidly.",
  TEMP_ABOVE_COMFORT: "Temperature is above the comfort threshold.",
  HUMIDITY_ABOVE_COMFORT: "Humidity is above the comfort threshold.",
  FIXED_SAFETY_LIMIT: "A fixed safety limit has been crossed.",
  TEMP_HUMIDITY_OUTSIDE_BASELINE: "Temperature and humidity are outside the learned normal range.",
  TEMP_OUTSIDE_BASELINE: "Temperature is outside the learned normal range.",
  HUMIDITY_OUTSIDE_BASELINE: "Humidity is outside the learned normal range.",
};

export function edgeStateLabel(state: string): string {
  return EDGE_STATE_LABEL[state] ?? state;
}

export function edgeReasonLabel(reasonCode: string, fallback: string): string {
  return EDGE_REASON_LABEL[reasonCode] ?? fallback;
}

export const SEVERITY_LABEL: Record<string, string> = {
  info: "Normal",
  watch: "Watch",
  warning: "Warning",
  urgent: "Urgent",
};
