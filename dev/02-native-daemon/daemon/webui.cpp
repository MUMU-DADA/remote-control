// webui.cpp — 内置网页控制台（单文件，零依赖）

#include "webui.h"

namespace autod {

const std::string& WebUiHtml() {
    // 用原始字符串字面量：HTML 里有大量引号和反斜杠，
    // 逐个转义既难写又难读。分隔符用 )HTML" 避免和内容冲突。
    static const std::string kHtml = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>autod 控制台</title>
<style>
  :root { --bg:#111; --fg:#eee; --dim:#888; --accent:#4a9; --warn:#c55; }
  * { box-sizing: border-box; }
  body { margin:0; background:var(--bg); color:var(--fg);
         font:13px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif; }
  header { padding:8px 12px; background:#1b1b1b; border-bottom:1px solid #333;
           display:flex; gap:12px; align-items:center; flex-wrap:wrap; }
  header h1 { font-size:15px; margin:0; font-weight:600; }
  .dim { color:var(--dim); }
  main { display:flex; gap:12px; padding:12px; align-items:flex-start;
         flex-wrap:wrap; }
  .screen { flex:0 0 auto; position:relative; background:#000;
            border:1px solid #333; }
  /* 画面用 image-rendering:pixelated 保持原始比例 —— 默认的平滑
     会让小屏截图糊成一团，坐标也难对准 */
  #screen { display:block; max-width:min(90vw,560px); height:auto;
            image-rendering:pixelated; cursor:crosshair;
            touch-action:none; user-select:none; background:#000; }
  .panel { flex:1 1 260px; min-width:260px; display:flex; flex-direction:column;
           gap:10px; }
  .card { background:#1b1b1b; border:1px solid #333; border-radius:6px;
          padding:10px; }
  .card h2 { font-size:12px; margin:0 0 8px; color:var(--dim);
             font-weight:600; text-transform:uppercase; letter-spacing:.5px; }
  .row { display:flex; gap:6px; flex-wrap:wrap; }
  button { background:#2a2a2a; color:var(--fg); border:1px solid #444;
           border-radius:4px; padding:6px 10px; font-size:12px; cursor:pointer; }
  button:hover { background:#333; }
  button:active { background:var(--accent); color:#000; }
  input[type=text] { flex:1; background:#0d0d0d; color:var(--fg);
                     border:1px solid #444; border-radius:4px; padding:6px 8px;
                     font-size:12px; min-width:0; }
  #status { font-size:12px; min-height:1.4em; }
  .ok { color:var(--accent); } .err { color:var(--warn); }
  #clip { white-space:pre-wrap; word-break:break-all; font-size:12px;
          max-height:120px; overflow:auto; background:#0d0d0d; padding:6px;
          border-radius:4px; border:1px solid #333; }
  #coord { position:absolute; right:4px; bottom:4px; background:rgba(0,0,0,.7);
           padding:2px 6px; border-radius:3px; font-size:11px; }
  .app { display:flex; align-items:center; gap:6px; padding:3px 0;
         border-bottom:1px solid #262626; }
  .app:last-child { border-bottom:none; }
  .app .pkg { flex:1; min-width:0; overflow:hidden; text-overflow:ellipsis;
              white-space:nowrap; }
  .app button { padding:2px 7px; font-size:11px; }
  #apps { max-height:240px; overflow:auto; }
  #apps input[type=text] { margin-bottom:6px; }
</style>
</head>
<body>
<header>
  <h1>autod 控制台</h1>
  <span class="dim" id="meta">连接中…</span>
</header>

<!-- 令牌条。开启鉴权后才需要填，平时隐藏（display:none）——
     平时摆一个用不上的输入框只会让人以为哪里要授权。 -->
<div id="authbar" style="display:none; padding:6px 12px; background:#3a2a12;
     border-bottom:1px solid #553; align-items:center; gap:6px;">
  <span style="font-size:12px; color:#dda">此服务需要访问令牌</span>
  <input type="text" id="tokeninput" placeholder="粘贴令牌" style="flex:1; min-width:0">
  <button onclick="saveToken()">保存</button>
</div>

<main>
  <div class="screen">
    <canvas id="screen"></canvas>
    <div id="coord"></div>
  </div>

  <div class="panel">
    <div class="card">
      <h2>状态</h2>
      <div id="status" class="dim">就绪</div>
      <div id="dbg" class="dim" style="font-size:11px;margin-top:4px"></div>
      <div class="row" style="margin-top:8px">
        <button onclick="refresh()">刷新状态</button>
        <button onclick="setFps(0)">暂停</button>
        <button onclick="setFps(10)">10</button>
        <button onclick="setFps(20)">20</button>
        <button onclick="setFps(30)">30</button>
        <button onclick="setFps(60)">60</button>
      </div>
      <div class="row" style="margin-top:6px">
        <span class="dim" style="font-size:11px;align-self:center">画质</span>
        <button onclick="setCodec('jpeg',70)">JPEG 快</button>
        <button onclick="setCodec('jpeg',90)">JPEG 清</button>
        <button onclick="setCodec('webp',75)">WebP</button>
        <button onclick="setCodec('png',1)">PNG 无损</button>
      </div>
    </div>

    <div class="card">
      <h2>按键</h2>
      <div class="row">
        <button onclick="key('home')">Home</button>
        <button onclick="key('back')">返回</button>
        <button onclick="key('appswitch')">最近</button>
        <button onclick="key('menu')">菜单</button>
        <button onclick="key('enter')">回车</button>
        <button onclick="key('power')">电源</button>
        <button onclick="key('volumeup')">音量+</button>
        <button onclick="key('volumedown')">音量−</button>
        <button onclick="key('up')">↑</button>
        <button onclick="key('down')">↓</button>
        <button onclick="key('left')">←</button>
        <button onclick="key('right')">→</button>
        <button onclick="key('backspace')">⌫</button>
        <button onclick="key('tab')">Tab</button>
        <button onclick="key('space')">空格</button>
      </div>
      <div class="row" style="margin-top:8px">
        <input type="text" id="keytext" placeholder="输入键名或键码，回车发送"
               onkeydown="if(event.key==='Enter'){key(this.value);this.value='';}">
      </div>
      <div class="dim" style="margin-top:6px;font-size:11px">
        键名见 /api/v1/describe；也可直接填数字键码。长按用
        <button style="padding:1px 5px;font-size:11px"
                onclick="keyLong()">长按最后输入的键</button>
      </div>
    </div>

    <div class="card">
      <h2>剪贴板</h2>
      <div id="clip" class="dim">（点「读剪贴板」查看）</div>
      <div class="row" style="margin-top:8px">
        <input type="text" id="cliptext" placeholder="要写入的文本">
        <button onclick="clipSet()">写入</button>
        <button onclick="clipGet()">读取</button>
      </div>
    </div>

    <div class="card">
      <h2>应用</h2>
      <div class="row">
        <label style="font-size:12px"><input type="checkbox" id="appSys"> 含系统应用</label>
        <button onclick="loadApps()">刷新列表</button>
      </div>
      <div id="apps" class="dim" style="margin-top:6px;font-size:12px">
        （点「刷新列表」加载）
      </div>
    </div>

    <div class="card">
      <h2>电源</h2>
      <div class="row">
        <button onclick="power('reboot')">重启</button>
        <button onclick="power('shutdown')">关机</button>
        <button onclick="power('reboot-recovery')">重启到 recovery</button>
      </div>
      <div class="dim" style="margin-top:6px;font-size:11px">
        设备会立即执行，请先确认没有未保存的工作。
      </div>
    </div>

    <div class="card">
      <h2>操作提示</h2>
      <div class="dim" style="font-size:12px">
        在画面上：<b>单击</b>=点击 · <b>按住不动</b>=长按 ·
        <b>按住拖动</b>=拖拽
      </div>
    </div>
  </div>
</main>

<script>
const $ = (id) => document.getElementById(id);
// 默认 30。
//
// ⚠️ 这里原来是 5，而按钮最高只到 10 —— 服务端明明能跑 30fps，
//    网页却一直在放幻灯片。实测踩过：命令行 curl 测出来 30fps，
//    但页面上"卡到爆炸"，就是因为这个默认值。
//    帧率不该由前端偷偷限死，服务端会按自己的能力截断。
let fps = 30;
// 编码格式与质量。
//
// ⚠️ 这两个必须在**所有使用者之前**声明。用 let 声明的变量在声明前
//    处于 TDZ，读它会抛 ReferenceError；而 streamUrl() 里有
//    `'&format=' + codec`，它在 try 里被调用 —— 于是异常被吞掉、
//    WebSocket 根本没创建，页面上只显示"画面流断开"，
//    看不出任何原因。实测就是这么坑了半天。
let codec = 'jpeg';
let quality = 75;
// 降采样宽度。设备屏幕往往比展示区域大得多，全分辨率纯属浪费带宽。
let maxW = 720;
let sw = 0, sh = 0;          // 屏幕真实尺寸
let streamKey = 0;

// 画面流的诊断信息。写进 DOM 而不是 console.log ——
// WebView shell 不转发 console.log，只有未捕获异常才进 logcat，
// 所以"流为什么断了"在那边完全看不到。
let streamDbg = '';
function dbg(t) {
  streamDbg = t;
  const el = $('dbg');
  if (el) el.textContent = streamDbg;
}

function setStatus(t, err) {
  const el = $('status');
  el.textContent = t;
  el.className = err ? 'err' : 'ok';
}

// ── 状态行 ──
//
// 把三样东西拼在一起：设备信息、实际收到的帧率、触控往返延迟。
// 后两个才是用户"感觉卡不卡"的直接依据 —— 服务端说 30fps 不等于
// 客户端真收到了 30 帧。
let metaInfo = '';
function updateMeta(r) {
  if (r) {
    const cap = r.runtime ? r.runtime.capture : r.capture;
    metaInfo = (sw || cap.primaryWidth || '?') + '×' + (sh || cap.primaryHeight || '?')
             + ' · ' + cap.backend
             + ' · pid ' + r.runtime.pid
             + ' · 协议 v' + r.runtime.protocolVersion;
  }
  const stream = streamReady ? (' · ' + shownFps + 'fps') : ' · 画面流断开';
  const touch  = wsReady ? (' · ' + rtt + 'ms') : ' · 触控流断开';
  $('meta').textContent = metaInfo + stream + touch;
}

function refresh() {
  api('/config').then(d => {
    const r = d.runtime;
    const cap = r.capture;
    sw = cap.primaryWidth || sw;
    sh = cap.primaryHeight || sh;
    updateMeta(d);
  }).catch(e => setStatus('取状态失败：' + e, true));
}

// ── 访问令牌 ──
//
// 存在 localStorage 里：开启鉴权后每次打开页面都要重新粘贴令牌的话
// 没人受得了。服务端只对 /api/ 下的请求校验，网页本身不校验 ——
// 所以页面能打开、再由脚本补上令牌。
let token = localStorage.getItem('autod_token') || '';

function authHeaders(extra) {
  const h = Object.assign({}, extra || {});
  if (token) h['Authorization'] = 'Bearer ' + token;
  return h;
}

// 网页的 WebSocket 不能自定义请求头，所以令牌走查询参数。
// 服务端两种都收（见 HttpServer::CheckAuth）。
function withToken(url) {
  if (!token) return url;
  return url + (url.indexOf('?') >= 0 ? '&' : '?') + 'token=' + encodeURIComponent(token);
}

function api(path, opts) {
  const o = Object.assign({}, opts || {});
  o.headers = authHeaders(o.headers);
  return fetch('/api/v1' + path, o).then(r => {
    if (r.status === 401) {
      // 令牌缺失或不对 —— 把输入条亮出来，别让用户对着一个
      // 什么都点不动的页面猜
      $('authbar').style.display = 'flex';
      token = '';
      localStorage.removeItem('autod_token');
      throw new Error('需要访问令牌');
    }
    return r.json();
  });
}

function saveToken() {
  token = $('tokeninput').value.trim();
  localStorage.setItem('autod_token', token);
  $('authbar').style.display = token ? 'none' : 'flex';
  setStatus(token ? '令牌已保存' : '令牌已清除');
  // 两条长连接要重连才会带上新令牌
  if (streamWs) { try { streamWs.close(); } catch (e) {} streamWs = null; }
  if (ws) { try { ws.close(); } catch (e) {} ws = null; }
  refresh();
  startStream();
  connectTouch();
}

// ── 实时画面（WebSocket）──
//
// 画面走 WebSocket + canvas，不走 MJPEG 的 <img>：
//
//   MJPEG 让浏览器把一条 multipart 流当成"会不断更新的图"，
//   省事但可控性差 —— 解码和绘制都排在浏览器的图片管道里，
//   帧率只能靠重设 src 来改，而且没法测延迟。
//
//   WebSocket 收到的是二进制图片帧，用 createImageBitmap 解码到
//   canvas。解码在 worker 线程上，绘制是同步的，链路更短。
//   客户端还能反过来控制流（改帧率/画质不用重连）。
//
// MJPEG 那条留着（/api/v1/stream 不带 Upgrade 头就是它），
// 用来嵌到别的页面或者调试最省事，也是下面断线时的兜底。

const cvs = $('screen');
const ctx = cvs.getContext('2d', { alpha: false, desynchronized: true });

let streamWs = null;
let streamReady = false;
let streamFrames = 0;
let lastFpsAt = performance.now();
let shownFps = 0;

function streamUrl() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  return withToken(proto + '//' + location.host + '/api/v1/stream'
       + '?fps=' + fps + '&format=' + codec + '&quality=' + quality
       + '&maxWidth=' + maxW);
}

function startStream() {
  if (fps <= 0) { stopStream(); return; }
  if (streamReady) { pushStreamParams(); return; }
  if (streamWs) return;                      // 正在连接

  let url;
  try {
    url = streamUrl();
  } catch (e) {
    // 构造 URL 就失败 —— 多半是某个参数没定义（TDZ 或拼错）。
    // 必须留下痕迹：只 setStatus 的话会被随后的触控流连接覆盖掉，
    // 用户看到的只是"画面流断开"，无从下手。
    dbg('url 构造失败: ' + e);
    setStatus('画面流地址构造失败：' + e, true);
    return;
  }
  try { streamWs = new WebSocket(url); }
  catch (e) { dbg('new WebSocket 失败: ' + e); setStatus('画面流建立失败：' + e, true); return; }
  streamWs.binaryType = 'blob';

  streamWs.onopen = () => {
    streamReady = true;
    dbg('open');
    setStatus('画面流已连接');
    updateMeta();
  };
  streamWs.onclose = (e) => {
    streamReady = false;
    streamWs = null;
    // 把关闭码打进控制台。WebSocket 断了但页面上只会显示一句
    // "断开"，光看那个分不清是被服务端关的、握手失败、还是网络问题。
    dbg('close code=' + e.code + ' clean=' + e.wasClean + ' r=' + (e.reason || ''));
    updateMeta();
    if (fps > 0) setTimeout(startStream, 1500);   // 自动重连
  };
  streamWs.onerror = (e) => {
    streamReady = false;
    dbg('error ' + (e && e.message ? e.message : '(无消息)'));
  };

  streamWs.onmessage = async (ev) => {
    // 文本消息是控制信息，二进制才是图
    if (typeof ev.data === 'string') {
      dbg('text ' + ev.data.slice(0, 60));
      let m; try { m = JSON.parse(ev.data); } catch (e) { return; }
      if (m.t === 'size') {
        // 服务端在第一帧之前告诉尺寸 —— 客户端据此建 canvas，
        // 否则得等图到了才知道多大
        if (cvs.width !== m.w || cvs.height !== m.h) {
          cvs.width = m.w; cvs.height = m.h;
          sw = m.w; sh = m.h;
          updateMeta();
        }
      } else if (m.t === 'hello') {
        setStatus('画面流 ' + m.format + ' @' + m.fps + 'fps');
      }
      return;
    }

    // 二进制帧：解码到 canvas
    if (streamFrames === 0) {
      dbg('binary ' + (ev.data.size || ev.data.byteLength) + 'B cIB='
          + (typeof createImageBitmap));
    }
    try {
      const bmp = await createImageBitmap(ev.data);
      if (cvs.width !== bmp.width || cvs.height !== bmp.height) {
        cvs.width = bmp.width; cvs.height = bmp.height;
        sw = bmp.width; sh = bmp.height;
      }
      ctx.drawImage(bmp, 0, 0);
      bmp.close();                 // 不 close 会攒着不放，几分钟就吃满内存
      ++streamFrames;
    } catch (e) {
      // 解码失败只丢这一帧。但如果是 API 不可用，会每帧都失败 ——
      // 那时候页面就是一片黑，而没有任何提示。所以记一次。
      if (streamFrames === 0) dbg('decode failed: ' + e);
    }
  };
}

function pushStreamParams() {
  if (!streamReady) return;
  streamWs.send(JSON.stringify({t: 'fps', v: fps}));
  streamWs.send(JSON.stringify({t: 'quality', v: quality}));
  streamWs.send(JSON.stringify({t: 'format', v: codec}));
}

function stopStream() {
  if (streamWs) { try { streamWs.close(); } catch (e) {} streamWs = null; }
  streamReady = false;
  setStatus('画面流已暂停');
  updateMeta();
}

// 实际收到的帧率。服务端说 30fps 不等于客户端真收到了 30 帧 ——
// 网络丢包、解码跟不上都会让它更低，而这个数才是用户看到的。
setInterval(() => {
  const now = performance.now();
  const dt = (now - lastFpsAt) / 1000;
  if (dt > 0) shownFps = Math.round(streamFrames / dt);
  streamFrames = 0;
  lastFpsAt = now;
  updateMeta();
}, 1000);

function setFps(v) {
  fps = v;
  if (v <= 0) { stopStream(); return; }
  startStream();
  pushStreamParams();
  setStatus('流帧率：' + v + ' fps');
}

function setCodec(c, q) {
  codec = c; quality = q;
  startStream();
  pushStreamParams();
  setStatus('编码：' + c.toUpperCase() + ' 质量 ' + q);
}

// ── 流式触控 ──
//
// 为什么用 WebSocket 而不是每个手势一个 POST：
//
//   一次 POST 要 TCP 往返 + HTTP 头解析 + 分发。拖拽时每个移动点都这么
//   来一遍，手感就是"一顿一顿"的；而且服务端只能等整个手势发完才知道
//   轨迹，做不到实时。
//
//   WebSocket 建一次连接，之后每个触控点就是一个几字节的帧。
//   实测往返延迟中位 1.0ms（HTTP POST 要 5-15ms）。
//
// 三个原语：down / move / up。按下就发 down，指针动了就发 move，
// 抬起发 up —— 服务端收到立刻注入，不再等"整个手势"。

let ws = null;
let wsReady = false;
let rtt = 0;
let seq = 0;
const pendingPings = new Map();

function wsUrl() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  return withToken(proto + '//' + location.host + '/api/v1/touch');
}

function connectTouch() {
  try { ws = new WebSocket(wsUrl()); }
  catch (e) { setStatus('触控流建立失败：' + e, true); return; }

  ws.onopen = () => {
    wsReady = true;
    setStatus('触控流已连接');
    // 立刻发一个心跳，页面上能马上看到延迟
    ping();
  };
  ws.onclose = () => {
    wsReady = false;
    // 自动重连。断线时用户还在拖拽的话，重连后状态是干净的
    // （设备侧那条手势已经在 up 或超时里结束了）。
    setTimeout(connectTouch, 1500);
  };
  ws.onerror = () => { wsReady = false; };
  ws.onmessage = (ev) => {
    let m;
    try { m = JSON.parse(ev.data); } catch (e) { return; }
    if (m.t === 'pong') {
      const sent = pendingPings.get(m.s);
      if (sent !== undefined) {
        rtt = Math.round(performance.now() - sent);
        pendingPings.delete(m.s);
        updateMeta();
      }
      return;
    }
    if (m.ok === false) setStatus('触控失败：' + (m.error || '未知'), true);
  };
}

function ping() {
  if (!wsReady) return;
  const s = ++seq;
  pendingPings.set(s, performance.now());
  if (pendingPings.size > 16) {         // 防止断线期间无限堆积
    const first = pendingPings.keys().next().value;
    pendingPings.delete(first);
  }
  send({t: 'ping', s: s});
}

function send(obj) {
  if (!wsReady) return false;
  ws.send(JSON.stringify(obj));
  return true;
}

setInterval(ping, 2000);

// ── 指针 → 触控事件 ──
//
// 坐标换算必须按**渲染后的显示尺寸**，不能用 naturalWidth：
// 画面会被 CSS 缩放过。换算后还要夹到屏幕范围内，
// 否则边缘点击会越界（设备侧的注入会失败或落到屏幕外）。
function toScreen(ev) {
  const rect = cvs.getBoundingClientRect();
  const w = sw || cvs.width || 1;
  const h = sh || cvs.height || 1;
  const x = Math.round((ev.clientX - rect.left) / rect.width * w);
  const y = Math.round((ev.clientY - rect.top) / rect.height * h);
  return {
    x: Math.max(0, Math.min(w - 1, x)),
    y: Math.max(0, Math.min(h - 1, y))
  };
}

// pointerId → 触控槽位。支持多点触控（两根手指同时拖）。
const slotOf = new Map();
let nextSlot = 0;

function slotFor(pointerId) {
  if (!slotOf.has(pointerId)) {
    slotOf.set(pointerId, nextSlot);
    nextSlot = (nextSlot + 1) % 10;      // 设备侧 10 个槽位
  }
  return slotOf.get(pointerId);
}

// 移动事件合并。
//
// pointermove 在高刷屏上能到 200Hz，而设备侧的注入和屏幕刷新都跟不上。
// 每个事件都发只是白占带宽、还让服务端的锁更频繁地切换。
// 用 rAF 合并到"每帧一个"，既跟得上显示又不浪费。
const moveQueue = new Map();   // slot → {x,y}
let rafPending = false;

function queueMove(slot, x, y) {
  moveQueue.set(slot, {x, y});
  if (rafPending) return;
  rafPending = true;
  requestAnimationFrame(() => {
    rafPending = false;
    for (const [s, p] of moveQueue) send({t: 'move', x: p.x, y: p.y, id: s});
    moveQueue.clear();
  });
}

cvs.addEventListener('pointerdown', (ev) => {
  ev.preventDefault();
  cvs.setPointerCapture(ev.pointerId);
  const p = toScreen(ev);
  const slot = slotFor(ev.pointerId);
  if (!send({t: 'down', x: p.x, y: p.y, id: slot})) {
    // 流没连上时退回一次性 POST，至少还能点
    tapPost(p.x, p.y);
  }
});

cvs.addEventListener('pointermove', (ev) => {
  const p = toScreen(ev);
  $('coord').textContent = p.x + ',' + p.y;
  if (!slotOf.has(ev.pointerId)) return;   // 没按下就不发
  queueMove(slotOf.get(ev.pointerId), p.x, p.y);
});

function endPointer(ev) {
  if (!slotOf.has(ev.pointerId)) return;
  const slot = slotOf.get(ev.pointerId);
  const p = toScreen(ev);
  slotOf.delete(ev.pointerId);
  moveQueue.delete(slot);
  send({t: 'up', x: p.x, y: p.y, id: slot});
}
cvs.addEventListener('pointerup', endPointer);
cvs.addEventListener('pointercancel', endPointer);
// 指针离开画面（比如拖到窗口外松手）也要收尾，
// 否则那根"手指"会在设备上一直按着
cvs.addEventListener('pointerleave', (ev) => {
  if (ev.buttons === 0) endPointer(ev);
});

// 长按不需要专门的命令：流式模型里"按下 → 手指不动 → 延迟抬起"
// 本来就是长按，系统会自己识别（Android 的 longPressTimeout 约 500ms）。
// 一次性手势（下面这个）只在触控流断开时兜底。

// ── 一次性手势（走 POST，不需要保持连接）──
function tapPost(x, y) {
  api('/tap', { method:'POST', headers:{'Content-Type':'application/json'},
                body: JSON.stringify({x:x, y:y, ms:50}) })
    .then(() => setStatus('点击 ' + x + ',' + y))
    .catch(e => setStatus('点击失败：' + e, true));
}


// ── 按键 ──
function key(name, longPress) {
  if (!name) return;
  api('/key', { method:'POST', headers:{'Content-Type':'application/json'},
                body: JSON.stringify({key:name, long:!!longPress}) })
    .then(d => setStatus('按键 ' + d.key + '（键码 ' + d.keyCode + '）')
        )
    .catch(e => setStatus('按键失败：' + e, true));
}
function keyLong() {
  const v = $('keytext').value.trim();
  if (v) key(v, true); else setStatus('先在输入框里填键名', true);
}

// ── 剪贴板 ──
function clipGet() {
  api('/clipboard?op=get')
    .then(d => {
      $('clip').textContent = d.has ? d.text : '（剪贴板为空）';
      $('clip').className = d.has ? '' : 'dim';
      setStatus('已读取剪贴板');
    })
    .catch(e => setStatus('读剪贴板失败：' + e, true));
}
function clipSet() {
  const t = $('cliptext').value;
  if (!t) { setStatus('请输入要写入的文本', true); return; }
  api('/clipboard', { method:'POST', headers:{'Content-Type':'application/json'},
                      body: JSON.stringify({op:'set', text:t}) })
    .then(() => { setStatus('已写入剪贴板'); clipGet(); })
    .catch(e => setStatus('写剪贴板失败：' + e, true));
}

// ── 应用列表 ──
function loadApps() {
  const sys = $('appSys').checked ? '1' : '0';
  $('apps').textContent = '加载中…';
  api('/apps?system=' + sys + '&meta=0')
    .then(d => {
      const box = $('apps');
      box.textContent = '';
      box.className = '';
      const filter = document.createElement('input');
      filter.type = 'text';
      filter.placeholder = '过滤包名…';
      // 137 个应用在下拉里翻很痛苦，加个过滤框
      const render = () => {
        const q = filter.value.toLowerCase();
        // 只渲染前 300 条，避免一次插几千个 DOM 节点
        const list = (d.apps || []).filter(a =>
            !q || a.package.toLowerCase().includes(q)).slice(0, 300);
        listBox.textContent = '';
        for (const a of list) {
          const row = document.createElement('div');
          row.className = 'app';
          const name = document.createElement('span');
          name.className = 'pkg';
          name.textContent = a.package;
          name.title = a.package;
          const bLaunch = document.createElement('button');
          bLaunch.textContent = '启动';
          bLaunch.onclick = () => appAction(a.package, 'launch');
          const bKill = document.createElement('button');
          bKill.textContent = '停止';
          bKill.onclick = () => appAction(a.package, 'kill');
          row.append(name, bLaunch, bKill);
          listBox.append(row);
        }
        if (!list.length) {
          const e = document.createElement('div');
          e.className = 'dim';
          e.textContent = '（没有匹配的应用）';
          listBox.append(e);
        }
      };
      const listBox = document.createElement('div');
      filter.oninput = render;
      box.append(filter, listBox);
      render();
      setStatus('共 ' + (d.count || 0) + ' 个应用');
    })
    .catch(e => setStatus('取应用列表失败：' + e, true));
}

function appAction(pkg, what) {
  const path = what === 'launch' ? '/apps/' + encodeURIComponent(pkg) + '/launch'
                                 : '/apps/' + encodeURIComponent(pkg) + '/kill';
  api(path, { method: 'POST', headers: {'Content-Type': 'application/json'},
              body: '{}' })
    .then(() => setStatus((what === 'launch' ? '已启动 ' : '已停止 ') + pkg))
    .catch(e => setStatus(what + ' ' + pkg + ' 失败：' + e, true));
}

// ── 电源 ──
function power(action) {
  const label = action === 'shutdown' ? '关机' : '重启';
  if (!confirm('确定要' + label + '设备吗？')) return;
  api('/power', { method:'POST', headers:{'Content-Type':'application/json'},
                  body: JSON.stringify({action: action}) })
    .then(d => setStatus(d.note || (label + '已下发')))
    .catch(e => setStatus(label + '失败：' + e, true));
}

// ── 启动 ──
refresh();
startStream();
connectTouch();
setInterval(refresh, 10000);   // 定期刷状态，页面放着不动也不会显示过期信息
</script>
</body>
</html>
)HTML";
    return kHtml;
}

}  // namespace autod
