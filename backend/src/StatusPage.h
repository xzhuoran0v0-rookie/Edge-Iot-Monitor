#pragma once

/**
 * @brief 状态页（单文件 HTML，编译进二进制）
 *
 * 为什么内联成字符串而不是读文件：
 * - 部署只需要拷一个 edge_server，不用同步 static 目录
 * - 不依赖进程 cwd —— 从任何目录启动都能打开
 * - 无外部 CDN/字体/框架：演示现场没网也照样渲染
 *
 * 页面只读，轮询 GET /api/status?device_id=；不发任何写请求。
 */
static constexpr const char *kStatusPageHtml = R"PAGE(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Edge IoT Monitor</title>
<style>
  :root {
    --bg:#0e1116; --panel:#171b22; --line:#262c36;
    --fg:#e6edf3; --dim:#8b949e;
    --ok:#3fb950; --watch:#d29922; --alert:#f85149;
  }
  @media (prefers-color-scheme: light) {
    :root {
      --bg:#f6f8fa; --panel:#ffffff; --line:#d8dee4;
      --fg:#1f2328; --dim:#636c76;
      --ok:#1a7f37; --watch:#9a6700; --alert:#cf222e;
    }
  }
  * { box-sizing:border-box; }
  body {
    margin:0; padding:24px; background:var(--bg); color:var(--fg);
    font:15px/1.55 ui-sans-serif,-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif;
  }
  .wrap { max-width:860px; margin:0 auto; }
  header { display:flex; align-items:baseline; gap:12px; flex-wrap:wrap; margin-bottom:20px; }
  h1 { font-size:19px; margin:0; font-weight:600; }
  .meta { color:var(--dim); font-size:13px; }
  .dot { display:inline-block; width:8px; height:8px; border-radius:50%; background:var(--dim); margin-right:6px; }
  .dot.live { background:var(--ok); }
  .dot.down { background:var(--alert); }
  .grid { display:grid; grid-template-columns:repeat(auto-fit,minmax(190px,1fr)); gap:14px; margin-bottom:18px; }
  .card { background:var(--panel); border:1px solid var(--line); border-radius:10px; padding:16px 18px; }
  .label { color:var(--dim); font-size:12px; text-transform:uppercase; letter-spacing:.05em; }
  .value { font-size:38px; font-weight:600; font-variant-numeric:tabular-nums; margin-top:6px; line-height:1.1; }
  .unit { font-size:18px; color:var(--dim); margin-left:3px; font-weight:400; }
  .stamp { color:var(--dim); font-size:12px; margin-top:6px; }
  .badge {
    display:inline-block; padding:2px 9px; border-radius:999px;
    font-size:12px; font-weight:600; text-transform:uppercase; letter-spacing:.04em;
    border:1px solid currentColor;
  }
  .sev-normal { color:var(--ok); } .sev-watch { color:var(--watch); } .sev-alert { color:var(--alert); }
  .narr-head { display:flex; align-items:center; gap:10px; margin-bottom:10px; flex-wrap:wrap; }
  .verdict { font-size:17px; font-weight:600; }
  .situation { margin:0 0 10px; }
  .explanation, .action { color:var(--dim); margin:0 0 8px; font-size:14px; }
  .action b { color:var(--fg); font-weight:600; }
  pre.raw { white-space:pre-wrap; word-break:break-word; font-size:12.5px; color:var(--dim); margin:0; }
  table { width:100%; border-collapse:collapse; font-size:13.5px; }
  th, td { text-align:left; padding:7px 10px; border-bottom:1px solid var(--line); }
  th { color:var(--dim); font-weight:500; font-size:12px; text-transform:uppercase; letter-spacing:.04em; }
  td.num { font-variant-numeric:tabular-nums; }
  .empty { color:var(--dim); font-size:14px; }
  h2 { font-size:13px; color:var(--dim); text-transform:uppercase; letter-spacing:.05em; margin:0 0 10px; font-weight:600; }
  .scroll { overflow-x:auto; }
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>Edge IoT Monitor</h1>
    <span class="meta"><span id="dot" class="dot"></span><span id="conn">connecting</span></span>
    <span class="meta" id="dev"></span>
  </header>

  <div class="grid" id="readings"></div>

  <div class="card" style="margin-bottom:18px;">
    <h2>What the model says</h2>
    <div id="narration"><span class="empty">No narration yet.</span></div>
  </div>

  <div class="card">
    <h2>Recent anomalies (IQR)</h2>
    <div id="anomalies"><span class="empty">None recorded.</span></div>
  </div>
</div>

<script>
// device_id comes from the query string so one page can serve several devices.
var deviceId = new URLSearchParams(location.search).get('device_id') || 'esp32s3-001';
document.getElementById('dev').textContent = deviceId;

function esc(s) {
  return String(s).replace(/[&<>"']/g, function (c) {
    return { '&':'&amp;', '<':'&lt;', '>':'&gt;', '"':'&quot;', "'":'&#39;' }[c];
  });
}

// Severity is model output, so it is untrusted: map only known values to a
// class and fall back to neutral. Never interpolate it into a class name.
function sevClass(s) {
  s = String(s || '').toLowerCase();
  if (s === 'normal' || s === 'ok') return 'sev-normal';
  if (s === 'watch' || s === 'warn' || s === 'warning') return 'sev-watch';
  if (s === 'alert' || s === 'critical' || s === 'high') return 'sev-alert';
  return '';
}

// Timestamps are UTC ISO8601. Show wall-clock time plus an age, because on a
// demo table the useful question is "is this still updating?", not the date.
function fmtTime(iso) {
  var t = Date.parse(iso);
  if (isNaN(t)) return iso;
  var d = new Date(t);
  var hhmmss = d.toTimeString().slice(0, 8);
  var age = Math.max(0, Math.round((Date.now() - t) / 1000));
  var ago = age < 60 ? age + 's ago'
          : age < 3600 ? Math.floor(age / 60) + 'm ago'
          : Math.floor(age / 3600) + 'h ago';
  return hhmmss + ' · ' + ago;
}

// Temperature reads first on the demo table; anything unknown keeps its
// alphabetical position after the two the hardware actually reports.
var SENSOR_ORDER = ['temperature', 'humidity'];
function sensorRank(t) {
  var i = SENSOR_ORDER.indexOf(String(t).toLowerCase());
  return i === -1 ? SENSOR_ORDER.length : i;
}

function renderReadings(list) {
  var el = document.getElementById('readings');
  if (!list || !list.length) {
    el.innerHTML = '<div class="card"><span class="empty">No readings for this device yet.</span></div>';
    return;
  }
  el.innerHTML = list.slice().sort(function (a, b) {
    return sensorRank(a.sensor_type) - sensorRank(b.sensor_type) ||
           String(a.sensor_type).localeCompare(String(b.sensor_type));
  }).map(function (r) {
    var v = typeof r.value === 'number' ? r.value.toFixed(1) : r.value;
    return '<div class="card">' +
      '<div class="label">' + esc(r.sensor_type) + '</div>' +
      '<div class="value">' + esc(v) + '<span class="unit">' + esc(r.unit || '') + '</span></div>' +
      '<div class="stamp">' + esc(fmtTime(r.timestamp)) + '</div>' +
    '</div>';
  }).join('');
}

function renderNarration(n) {
  var el = document.getElementById('narration');
  if (!n) { el.innerHTML = '<span class="empty">No narration yet.</span>'; return; }

  // The backend parses the model's JSON when it can and passes the raw text
  // through when it cannot. Both are shown rather than hiding a parse failure.
  if (!n.parsed) {
    el.innerHTML = '<pre class="raw">' + esc(n.raw || '') + '</pre>' +
                   '<div class="stamp">' + esc(fmtTime(n.timestamp)) + ' — unparsed model output</div>';
    return;
  }
  var html = '<div class="narr-head">';
  if (n.verdict) html += '<span class="verdict">' + esc(n.verdict) + '</span>';
  if (n.severity) html += '<span class="badge ' + sevClass(n.severity) + '">' + esc(n.severity) + '</span>';
  html += '</div>';
  if (n.situation) html += '<p class="situation">' + esc(n.situation) + '</p>';
  if (n.explanation) html += '<p class="explanation">' + esc(n.explanation) + '</p>';
  if (n.suggested_action) html += '<p class="action"><b>Suggested:</b> ' + esc(n.suggested_action) + '</p>';
  html += '<div class="stamp">' + esc(fmtTime(n.timestamp)) + '</div>';
  el.innerHTML = html;
}

function renderAnomalies(list) {
  var el = document.getElementById('anomalies');
  if (!list || !list.length) { el.innerHTML = '<span class="empty">None recorded.</span>'; return; }
  el.innerHTML = '<div class="scroll"><table><thead><tr>' +
    '<th>Time</th><th>Sensor</th><th>Value</th></tr></thead><tbody>' +
    list.map(function (a) {
      var v = typeof a.value === 'number' ? a.value.toFixed(1) : a.value;
      return '<tr><td>' + esc(fmtTime(a.timestamp)) + '</td><td>' + esc(a.sensor_type) +
             '</td><td class="num">' + esc(v) + '</td></tr>';
    }).join('') + '</tbody></table></div>';
}

function setConn(ok, msg) {
  document.getElementById('dot').className = 'dot ' + (ok ? 'live' : 'down');
  document.getElementById('conn').textContent = msg;
}

function poll() {
  fetch('/api/status?device_id=' + encodeURIComponent(deviceId), { cache: 'no-store' })
    .then(function (r) { return r.json(); })
    .then(function (d) {
      if (d.status !== 'ok') { setConn(false, d.message || 'error'); return; }
      setConn(true, 'live');
      renderReadings(d.readings);
      renderNarration(d.narration);
      renderAnomalies(d.anomalies);
    })
    .catch(function () { setConn(false, 'backend unreachable'); });
}

poll();
setInterval(poll, 2000);
</script>
</body>
</html>
)PAGE";
