#pragma once

/**
 * @brief 状态页（单文件 HTML，编译进二进制）
 *
 * 为什么内联成字符串而不是读文件：
 * - 部署只需要拷一个 edge_server，不用同步 static 目录
 * - 不依赖进程 cwd —— 从任何目录启动都能打开
 * - 无外部 CDN/字体/框架：演示现场没网也照样渲染
 *
 * 页面只读：轮询 GET /api/status（当前值 + 叙述，2s）和 GET /api/history
 * （曲线，5s），不发任何写请求，也不会触发推理。
 *
 * 图表是手写 SVG。不引第三方图表库不是洁癖 —— CDN 在断网的演示现场就是
 * 一块空白，而把库打包进来又要引入构建步骤。
 */
static constexpr const char *kStatusPageHtml = R"PAGE(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>边缘物联网监测</title>
<style>
  :root {
    --bg:#0e1116; --panel:#171b22; --line:#262c36; --grid:#222831;
    --fg:#e6edf3; --dim:#8b949e;
    --ok:#3fb950; --watch:#d29922; --alert:#f85149;
    --trace:#58a6ff; --trace-fill:rgba(88,166,255,.13);
  }
  @media (prefers-color-scheme: light) {
    :root {
      --bg:#f6f8fa; --panel:#ffffff; --line:#d8dee4; --grid:#eaeef2;
      --fg:#1f2328; --dim:#636c76;
      --ok:#1a7f37; --watch:#9a6700; --alert:#cf222e;
      --trace:#0969da; --trace-fill:rgba(9,105,218,.10);
    }
  }
  * { box-sizing:border-box; }
  /* An author `display` rule outranks the UA style for [hidden], so .legend's
     display:flex would keep it visible while hidden is set. */
  [hidden] { display:none !important; }
  body {
    margin:0; padding:24px; background:var(--bg); color:var(--fg);
    font:15px/1.55 ui-sans-serif,-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif;
  }
  .wrap { max-width:900px; margin:0 auto; }
  header { display:flex; align-items:baseline; gap:12px; flex-wrap:wrap; margin-bottom:18px; }
  h1 { font-size:19px; margin:0; font-weight:600; }
  .meta { color:var(--dim); font-size:13px; }
  .dot { display:inline-block; width:8px; height:8px; border-radius:50%; background:var(--dim); margin-right:6px; }
  .dot.live { background:var(--ok); }
  .dot.down { background:var(--alert); }
  .card { background:var(--panel); border:1px solid var(--line); border-radius:10px; padding:16px 18px; margin-bottom:14px; }
  .label { color:var(--dim); font-size:12px; text-transform:uppercase; letter-spacing:.05em; }
  .chart-head { display:flex; align-items:flex-end; justify-content:space-between; gap:12px; margin-bottom:4px; }
  .reading { text-align:right; }
  .now { font-size:34px; font-weight:600; font-variant-numeric:tabular-nums; line-height:1; }
  .age { margin-top:4px; }
  .now .unit { font-size:17px; color:var(--dim); margin-left:3px; font-weight:400; }
  .plot { margin-top:6px; }
  .plot svg { display:block; width:100%; }
  .collecting { color:var(--dim); font-size:13px; padding:26px 0; text-align:center; }
  .legend { color:var(--dim); font-size:12px; margin:-4px 0 16px; display:flex; gap:16px; flex-wrap:wrap; }
  .swatch { display:inline-block; width:8px; height:8px; border-radius:50%; background:var(--alert); margin-right:5px; vertical-align:middle; }
  .tickmark { display:inline-block; width:2px; height:11px; background:var(--dim); margin-right:5px; vertical-align:middle; }
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
  .stamp { color:var(--dim); font-size:12px; margin-top:6px; }
  pre.raw { white-space:pre-wrap; word-break:break-word; font-size:12.5px; color:var(--dim); margin:0; }
  .empty { color:var(--dim); font-size:14px; }
  h2 { font-size:13px; color:var(--dim); text-transform:uppercase; letter-spacing:.05em; margin:0 0 10px; font-weight:600; }
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>边缘物联网监测</h1>
    <span class="meta"><span id="dot" class="dot"></span><span id="conn">连接中</span></span>
    <span class="meta" id="dev"></span>
  </header>

  <div id="charts"></div>
  <div class="card" id="nodata" hidden><span class="empty">该设备暂无数据。</span></div>
  <div class="legend" id="legend" hidden>
    <span><span class="swatch"></span>IQR 判定的异常点（服务端 C++，非模型）</span>
    <span><span class="tickmark"></span>模型在此生成叙述</span>
  </div>

  <div class="card">
    <h2>模型解读</h2>
    <div id="narration"><span class="empty">尚无叙述。</span></div>
  </div>
</div>

<script>
var deviceId = new URLSearchParams(location.search).get('device_id') || 'esp32s3-001';
var WINDOW_MIN = 10;
document.getElementById('dev').textContent = deviceId;

var SVG_NS = 'http://www.w3.org/2000/svg';
var PLOT_H = 150, PAD_TOP = 14, PAD_BOTTOM = 20, PAD_RIGHT = 44;

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

// Timestamps are UTC ISO8601. Show wall-clock time plus an age, because the
// useful question on a demo table is "is this still updating?", not the date.
function fmtTime(iso) {
  var t = Date.parse(iso);
  if (isNaN(t)) return iso;
  var age = Math.max(0, Math.round((Date.now() - t) / 1000));
  var ago = age < 60 ? age + ' 秒前'
          : age < 3600 ? Math.floor(age / 60) + ' 分钟前'
          : Math.floor(age / 3600) + ' 小时前';
  return new Date(t).toTimeString().slice(0, 8) + ' · ' + ago;
}

// Temperature reads first on the demo table; anything unknown sorts after the
// two the hardware actually reports.
var SENSOR_ORDER = ['temperature', 'humidity'];
function sensorRank(t) {
  var i = SENSOR_ORDER.indexOf(String(t).toLowerCase());
  return i === -1 ? SENSOR_ORDER.length : i;
}

// sensor_type stays English on the wire; only the label shown is translated.
// An unknown type falls back to its raw name rather than disappearing.
var SENSOR_LABEL = { temperature: '温度', humidity: '湿度' };
function sensorLabel(t) {
  return SENSOR_LABEL[String(t).toLowerCase()] || t;
}

function el(tag, attrs) {
  var n = document.createElementNS(SVG_NS, tag);
  for (var k in attrs) n.setAttribute(k, attrs[k]);
  return n;
}

/**
 * Draw one sensor's trace, with the anomalies the server flagged and the
 * moments the model spoke on the same time axis. That alignment is the point:
 * it shows detection and narration as separate steps, not one black box.
 */
function drawChart(host, series, anomalies, narrations, sinceMs, nowMs) {
  host.innerHTML = '';
  var pts = series.points || [];

  if (pts.length < 2) {
    var msg = document.createElement('div');
    msg.className = 'collecting';
    msg.textContent = pts.length ? '正在采集数据（当前仅 1 个采样点）' : '等待数据…';
    host.appendChild(msg);
    return;
  }

  var w = host.clientWidth || 640;
  var plotW = Math.max(80, w - PAD_RIGHT);

  // Right edge is always "now", so a stalled feed shows as a trace that stops
  // short of the edge. The left edge is the window start only once there is
  // enough history to fill it — otherwise a fresh boot draws its first minute
  // as a sliver against ten minutes of blank.
  var firstMs = Date.parse(pts[0].t);
  var startMs = isNaN(firstMs) ? sinceMs : Math.max(sinceMs, firstMs);
  var span = Math.max(1000, nowMs - startMs);

  function sx(ms) { return (ms - startMs) / span * plotW; }

  var vals = pts.map(function (p) { return p.v; });
  var lo = Math.min.apply(null, vals), hi = Math.max.apply(null, vals);
  var pad = (hi - lo) < 0.5 ? 1 : (hi - lo) * 0.08;
  lo -= pad; hi += pad;

  function sy(v) { return PAD_TOP + (hi - v) / (hi - lo) * (PLOT_H - PAD_TOP - PAD_BOTTOM); }

  var svg = el('svg', { viewBox: '0 0 ' + w + ' ' + PLOT_H, height: PLOT_H });

  // horizontal guides at the value extremes
  [hi, lo].forEach(function (v) {
    svg.appendChild(el('line', {
      x1: 0, y1: sy(v), x2: plotW, y2: sy(v),
      stroke: 'var(--grid)', 'stroke-width': 1
    }));
    var lbl = el('text', {
      x: plotW + 6, y: sy(v) + 4, fill: 'var(--dim)', 'font-size': 11
    });
    lbl.textContent = v.toFixed(1);
    svg.appendChild(lbl);
  });

  // narration marks first, so the trace draws over them
  narrations.forEach(function (n) {
    var ms = Date.parse(n.timestamp);
    if (isNaN(ms) || ms < sinceMs) return;
    var x = sx(ms);
    var line = el('line', {
      x1: x, y1: PAD_TOP - 6, x2: x, y2: PLOT_H - PAD_BOTTOM,
      stroke: 'var(--dim)', 'stroke-width': 1.5, 'stroke-dasharray': '2 3'
    });
    var title = el('title', {});
    title.textContent = (n.verdict || '叙述') + ' · ' + n.timestamp;
    line.appendChild(title);
    svg.appendChild(line);
  });

  // Break the trace where reporting stopped. Joining across a gap draws a
  // straight line that looks exactly like a measured steady state — the chart
  // would be inventing the readings a dropped WiFi link never sent.
  var times = pts.map(function (p) { return Date.parse(p.t); });
  var deltas = [];
  for (var i = 1; i < times.length; i++) deltas.push(times[i] - times[i - 1]);
  var median = deltas.slice().sort(function (a, b) { return a - b; })[Math.floor(deltas.length / 2)] || 1000;
  var gapMs = Math.max(30000, median * 4);

  var segments = [[]];
  pts.forEach(function (p, i) {
    if (i > 0 && (times[i] - times[i - 1]) > gapMs) segments.push([]);
    segments[segments.length - 1].push({ x: sx(times[i]), y: sy(p.v) });
  });

  segments.forEach(function (seg) {
    if (seg.length < 2) {
      if (seg.length === 1) {
        svg.appendChild(el('circle', { cx: seg[0].x, cy: seg[0].y, r: 2, fill: 'var(--trace)' }));
      }
      return;
    }
    var coords = seg.map(function (p) { return p.x + ',' + p.y; });
    var floorY = PLOT_H - PAD_BOTTOM;

    svg.appendChild(el('polygon', {
      points: seg[0].x + ',' + floorY + ' ' + coords.join(' ') + ' ' +
              seg[seg.length - 1].x + ',' + floorY,
      fill: 'var(--trace-fill)', stroke: 'none'
    }));

    svg.appendChild(el('polyline', {
      points: coords.join(' '), fill: 'none',
      stroke: 'var(--trace)', 'stroke-width': 2,
      'stroke-linejoin': 'round', 'stroke-linecap': 'round'
    }));
  });

  // anomaly points for this sensor only
  anomalies.forEach(function (a) {
    if (a.sensor_type !== series.sensor_type) return;
    var ms = Date.parse(a.timestamp);
    if (isNaN(ms) || ms < sinceMs) return;
    var c = el('circle', {
      cx: sx(ms), cy: sy(a.value), r: 3.5,
      fill: 'var(--alert)', stroke: 'var(--panel)', 'stroke-width': 1
    });
    var t = el('title', {});
    t.textContent = 'IQR 异常 · ' + a.value + ' · ' + a.timestamp;
    c.appendChild(t);
    svg.appendChild(c);
  });

  var last = pts[pts.length - 1];
  svg.appendChild(el('circle', {
    cx: sx(Date.parse(last.t)), cy: sy(last.v), r: 3, fill: 'var(--trace)'
  }));

  host.appendChild(svg);
}

// Card skeletons are created once and reused: the value updates every 2s while
// the trace redraws every 5s, so they cannot share a render pass.
var cards = {};
function ensureCard(sensorType) {
  if (cards[sensorType]) return cards[sensorType];

  var card = document.createElement('div');
  card.className = 'card';
  card.innerHTML =
    '<div class="chart-head">' +
      '<div class="label"></div>' +
      '<div class="reading">' +
        '<div class="now"><span class="v">—</span><span class="unit"></span></div>' +
        '<div class="stamp age"></div>' +
      '</div>' +
    '</div><div class="plot"></div>';
  card.querySelector('.label').textContent = sensorLabel(sensorType);

  document.getElementById('charts').appendChild(card);
  cards[sensorType] = {
    root: card,
    value: card.querySelector('.v'),
    unit: card.querySelector('.unit'),
    age: card.querySelector('.age'),
    plot: card.querySelector('.plot')
  };
  return cards[sensorType];
}

function renderNow(list) {
  (list || []).slice().sort(function (a, b) {
    return sensorRank(a.sensor_type) - sensorRank(b.sensor_type);
  }).forEach(function (r) {
    var c = ensureCard(r.sensor_type);
    c.value.textContent = typeof r.value === 'number' ? r.value.toFixed(1) : r.value;
    c.unit.textContent = r.unit || '';

    // The header's "live" only means the backend answered. A value with no age
    // beside it looks current even when the device stopped reporting minutes
    // ago, which is the one thing a monitoring page must never do.
    c.age.textContent = fmtTime(r.timestamp);
  });
}

function renderNarration(n) {
  var host = document.getElementById('narration');
  if (!n) { host.innerHTML = '<span class="empty">尚无叙述。</span>'; return; }

  // The backend parses the model's JSON when it can and passes the raw text
  // through when it cannot. Both are shown rather than hiding a parse failure.
  if (!n.parsed) {
    host.innerHTML = '<pre class="raw">' + esc(n.raw || '') + '</pre>' +
                     '<div class="stamp">' + esc(fmtTime(n.timestamp)) + ' — 模型输出无法解析</div>';
    return;
  }
  var html = '<div class="narr-head">';
  if (n.verdict) html += '<span class="verdict">' + esc(n.verdict) + '</span>';
  if (n.severity) html += '<span class="badge ' + sevClass(n.severity) + '">' + esc(n.severity) + '</span>';
  html += '</div>';
  if (n.situation) html += '<p class="situation">' + esc(n.situation) + '</p>';
  if (n.explanation) html += '<p class="explanation">' + esc(n.explanation) + '</p>';
  if (n.suggested_action) html += '<p class="action"><b>建议：</b>' + esc(n.suggested_action) + '</p>';
  html += '<div class="stamp">' + esc(fmtTime(n.timestamp)) + '</div>';
  host.innerHTML = html;
}

function setConn(ok, msg) {
  document.getElementById('dot').className = 'dot ' + (ok ? 'live' : 'down');
  document.getElementById('conn').textContent = msg;
}

function pollStatus() {
  fetch('/api/status?device_id=' + encodeURIComponent(deviceId), { cache: 'no-store' })
    .then(function (r) { return r.json(); })
    .then(function (d) {
      if (d.status !== 'ok') { setConn(false, d.msg || '错误'); return; }
      setConn(true, '实时');
      renderNow(d.readings);
      renderNarration(d.narration);
    })
    .catch(function () { setConn(false, '后端连接失败'); });
}

var lastHistory = null;
function pollHistory() {
  fetch('/api/history?device_id=' + encodeURIComponent(deviceId) + '&minutes=' + WINDOW_MIN,
        { cache: 'no-store' })
    .then(function (r) { return r.json(); })
    .then(function (d) {
      if (d.status !== 'ok') return;
      lastHistory = d;
      drawAll();
    })
    .catch(function () { /* the status poll already reports connectivity */ });
}

function drawAll() {
  var d = lastHistory;
  if (!d) return;
  var sinceMs = Date.parse(d.since);
  var nowMs = Date.now();
  var list = (d.series || []).slice().sort(function (a, b) {
    return sensorRank(a.sensor_type) - sensorRank(b.sensor_type);
  });

  // The legend explains marks on a chart; with no chart it explains nothing.
  document.getElementById('legend').hidden = list.length === 0;
  document.getElementById('nodata').hidden = list.length !== 0;

  list.forEach(function (s) {
    var c = ensureCard(s.sensor_type);
    drawChart(c.plot, s, d.anomalies || [], d.narrations || [], sinceMs, nowMs);
  });
}

pollStatus();
pollHistory();
setInterval(pollStatus, 2000);
setInterval(pollHistory, 5000);
// SVG geometry is computed from the measured element width, so a resize needs
// a redraw; without this the trace keeps the old width until the next poll.
window.addEventListener('resize', drawAll);
</script>
</body>
</html>
)PAGE";
