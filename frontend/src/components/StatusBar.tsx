interface Props {
  /** The dashboard reached the backend on the last poll. */
  backendReachable: boolean;
  /** The device is still reporting (derived server-side from the last reading). */
  deviceOnline: boolean;
}

/**
 * Only states the system can actually observe are shown.
 *
 * Wi-Fi / MQTT / NTP live on the device and are not part of the ingest payload,
 * so the dashboard has no way to know them. Displaying a green pill for a link
 * nobody measured is worse than showing nothing — surfacing them needs a status
 * field in the firmware's report first.
 */
export default function StatusBar({ backendReachable, deviceOnline }: Props) {
  const items = [
    { key: "backend", label: "Backend", ok: backendReachable },
    { key: "device", label: "Device reports", ok: deviceOnline },
  ];

  return (
    <div className="status-grid">
      {items.map((item) => (
        <span key={item.key} className={`status-pill ${item.ok ? "ok" : "err"}`}>
          <span className="status-dot" />
          {item.label}
        </span>
      ))}
    </div>
  );
}
