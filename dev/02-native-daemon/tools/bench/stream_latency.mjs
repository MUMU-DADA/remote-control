#!/usr/bin/env node
// Node 22+ provides the WebSocket client; no protocol implementation needed.
import { performance } from 'node:perf_hooks';
import { parseArgs } from 'node:util';

const { values } = parseArgs({ options: {
  base: { type: 'string', default: 'http://127.0.0.1:18088' },
  clients: { type: 'string', default: '1' },
  seconds: { type: 'string', default: '6' },
  format: { type: 'string', default: 'jpeg' },
  fps: { type: 'string', default: '60' },
  width: { type: 'string', default: '0' },
  skip: { type: 'boolean', default: false },
  token: { type: 'string', default: process.env.RC_TOKEN ?? '' },
} });
const clients = Number(values.clients), seconds = Number(values.seconds);
if (!Number.isInteger(clients) || clients < 1 || clients > 16 || !(seconds > 0)) {
  throw new Error('clients must be 1..16 and seconds must be positive');
}
// 访问令牌。交付镜像默认开鉴权。
//
// ⚠️ 这里走**查询参数**而不是 Authorization 头：WebSocket 那条（下面）
//    没法自己加头 —— 浏览器的 WebSocket 构造函数根本不接受 headers。
//    服务端两种都收（见 docs/api/01-http.md），所以统一用 ?token= 最省事。
const tokenQuery = values.token ? { token: values.token } : {};
const withToken = path => {
  const q = new URLSearchParams(tokenQuery).toString();
  return values.base + path + (q ? (path.includes('?') ? '&' : '?') + q : '');
};
const getJson = async path => {
  const response = await fetch(withToken(path), { signal: AbortSignal.timeout(5000) });
  if (response.status === 401) {
    throw new Error(`${path}: 需要访问令牌 —— 用 --token=<令牌> 或 RC_TOKEN=<令牌>；`
      + ` 令牌在设备上的 /data/misc/remote-control/remote-control.conf，`
      + `或宿主 .run/instances/<名字>.token`);
  }
  if (!response.ok) throw new Error(`${path}: HTTP ${response.status}`);
  return response.json();
};
const before = await getJson('/api/v1/params');
const config = await getJson('/api/v1/config');
const url = new URL(values.base + '/api/v1/stream');
url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
url.search = new URLSearchParams({ format: values.format, fps: values.fps,
  maxWidth: values.width, skipUnchanged: values.skip ? '1' : '0',
  ...tokenQuery }).toString();
const sockets = [], measurements = [], pings = new Map(), rtts = [];
let pingTimer;
const percentile = (samples, fraction) => {
  if (!samples.length) return null;
  const sorted = [...samples].sort((a, b) => a - b);
  return Math.round(sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * fraction))] * 100) / 100;
};
try {
  const started = performance.now();
  for (let i = 0; i < clients; ++i) {
    const ws = new WebSocket(url);
    ws.binaryType = 'arraybuffer';
    sockets.push(ws);
    const m = { frames: 0, bytes: 0, firstFrameMs: null, closed: false, errors: [] };
    measurements.push(m);
    ws.onmessage = event => {
      if (typeof event.data === 'string') {
        const message = JSON.parse(event.data);
        if (message.t === 'error') m.errors.push(message.error);
        if (message.t === 'pong' && pings.has(message.s)) {
          rtts.push(performance.now() - pings.get(message.s));
          pings.delete(message.s);
        }
      } else {
        if (m.firstFrameMs === null) m.firstFrameMs = performance.now() - started;
        ++m.frames;
        m.bytes += event.data.byteLength;
      }
    };
    ws.onclose = () => { m.closed = true; };
  }
  await Promise.all(sockets.map(ws => new Promise((resolve, reject) => {
    ws.addEventListener('open', resolve, { once: true });
    ws.addEventListener('error', reject, { once: true });
  })));
  let sequence = 0;
  pingTimer = setInterval(() => {
    if (sockets[0].readyState !== WebSocket.OPEN) return;
    pings.set(++sequence, performance.now());
    sockets[0].send(JSON.stringify({ t: 'ping', s: sequence }));
  }, 100);
  await new Promise(resolve => setTimeout(resolve, seconds * 1000));
  const elapsedSeconds = (performance.now() - started) / 1000;
  const after = await getJson('/api/v1/params');
  const measuredStreams = measurements.map(m => ({ ...m,
    fps: Math.round(m.frames / elapsedSeconds * 10) / 10 }));
  const measuredRtts = [...rtts];
  clearInterval(pingTimer);
  const screenshotTimes = [];
  for (let i = 0; i < 8; ++i) {
    const start = performance.now();
    const response = await fetch(withToken('/api/v1/capture?format=' +
      (values.format === 'h264' ? 'jpeg' : values.format)), { signal: AbortSignal.timeout(5000) });
    if (!response.ok) throw new Error(`capture: HTTP ${response.status}`);
    await response.arrayBuffer();
    screenshotTimes.push(performance.now() - start);
  }
  console.log(JSON.stringify({
    backend: config.runtime?.capture?.backend, format: values.format,
    width: Number(values.width), requestedFps: Number(values.fps), clients,
    skipUnchanged: values.skip, elapsedSeconds,
    streams: measuredStreams,
    pingMs: { samples: measuredRtts.length, p50: percentile(measuredRtts, 0.5),
      p95: percentile(measuredRtts, 0.95), max: percentile(measuredRtts, 1) },
    screenshotMs: { samples: screenshotTimes.length, p50: percentile(screenshotTimes, 0.5), p95: percentile(screenshotTimes, 0.95) },
    capture: { frames: after.capture.frames - before.capture.frames,
      lastCaptureMs: after.capture.lastCaptureMs,
      changedFrames: after.capture.changeGen - before.capture.changeGen,
      unchangedFrames: after.capture.unchanged - before.capture.unchanged },
    encoding: after.encoding && Object.fromEntries(Object.entries(after.encoding)
      .map(([key, count]) => [key, count - (before.encoding?.[key] ?? 0)])),
  }, null, 2));
} finally {
  clearInterval(pingTimer);
  sockets.forEach(ws => ws.close());
}
