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
            touch-action:none; user-select:none; }
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

<main>
  <div class="screen">
    <img id="screen" alt="屏幕">
    <div id="coord"></div>
  </div>

  <div class="panel">
    <div class="card">
      <h2>状态</h2>
      <div id="status" class="dim">就绪</div>
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
const img = $('screen');
// 默认 30。
//
// ⚠️ 这里原来是 5，而按钮最高只到 10 —— 服务端明明能跑 30fps，
//    网页却一直在放幻灯片。实测踩过：命令行 curl 测出来 30fps，
//    但页面上"卡到爆炸"，就是因为这个默认值。
//    帧率不该由前端偷偷限死，服务端会按自己的能力截断。
let fps = 30;
// 降采样宽度。设备屏幕往往比展示区域大得多，全分辨率纯属浪费带宽。
let maxW = 720;
let sw = 0, sh = 0;          // 屏幕真实尺寸
let streamKey = 0;

function setStatus(t, err) {
  const el = $('status');
  el.textContent = t;
  el.className = err ? 'err' : 'ok';
}

function api(path, opts) {
  return fetch('/api/v1' + path, opts).then(r => r.json());
}

// ── 实时画面 ──
// MJPEG：浏览器把 multipart/x-mixed-replace 当成会不断更新的图，
// 原生支持，不需要 JS 解帧 —— 这是最省事也最省电的做法。
function startStream() {
  if (fps <= 0) { img.removeAttribute('src'); return; }
  streamKey++;
  img.src = '/api/v1/stream?fps=' + fps
          + '&format=' + codec + '&quality=' + quality
          + '&maxWidth=' + maxW + '&_=' + streamKey;
}
function setFps(v) { fps = v; startStream(); setStatus('流帧率：' + (v ? v + ' fps' : '已暂停')); }

// 编码格式。
//
// PNG 是无损的，一帧要 100+ KB —— 对"看画面、点坐标"来说完全不划算。
// JPEG/WebP 有损但小一个数量级，浏览器解码也快得多。默认用 JPEG。
let codec = 'jpeg';
let quality = 75;
function setCodec(c, q) {
  codec = c; quality = q;
  startStream();
  setStatus('编码：' + c.toUpperCase() + ' 质量 ' + q);
}

function refresh() {
  api('/config').then(d => {
    const r = d.runtime, c = d.config;
    sw = (r.capture && r.capture.primaryWidth) || 0;
    sh = (r.capture && r.capture.primaryHeight) || 0;
    updateMeta(r);
  }).catch(e => setStatus('取状态失败：' + e, true));
}

let metaInfo = '';
function updateMeta(r) {
  if (r) {
    metaInfo = (r.capture.primaryWidth || sw) + '×'
             + (r.capture.primaryHeight || sh) + ' · ' + r.capture.backend
             + ' · pid ' + r.pid + ' · 协议 v' + r.protocolVersion;
  }
  // 把往返延迟显示出来 —— 触控手感好不好，用户感受到的是这个数
  $('meta').textContent = metaInfo + (wsReady ? ' · ' + rtt + 'ms' : ' · 触控流断开');
}

// 画面尺寸拿到之前先轮询 —— img 的 naturalWidth 要等第一帧到达
img.addEventListener('load', () => {
  if (!sw) {
    sw = img.naturalWidth; sh = img.naturalHeight;
    $('meta').textContent = sw + '×' + sh;
  }
});

// ── 坐标换算 ──
// 页面上的像素 → 屏幕像素。必须按**渲染后的显示尺寸**换算，
// 而不是 naturalWidth：画面被 CSS 缩放过。
function toScreen(ev) {
  const rect = img.getBoundingClientRect();
  const x = Math.round((ev.clientX - rect.left) / rect.width * (sw || img.naturalWidth));
  const y = Math.round((ev.clientY - rect.top) / rect.height * (sh || img.naturalHeight));
  return {
    x: Math.max(0, Math.min((sw || img.naturalWidth) - 1, x)),
    y: Math.max(0, Math.min((sh || img.naturalHeight) - 1, y))
  };
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
  return proto + '//' + location.host + '/api/v1/touch';
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
  const rect = img.getBoundingClientRect();
  const w = sw || img.naturalWidth || 1;
  const h = sh || img.naturalHeight || 1;
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

img.addEventListener('pointerdown', (ev) => {
  ev.preventDefault();
  img.setPointerCapture(ev.pointerId);
  const p = toScreen(ev);
  const slot = slotFor(ev.pointerId);
  if (!send({t: 'down', x: p.x, y: p.y, id: slot})) {
    // 流没连上时退回一次性 POST，至少还能点
    tapPost(p.x, p.y);
  }
});

img.addEventListener('pointermove', (ev) => {
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
img.addEventListener('pointerup', endPointer);
img.addEventListener('pointercancel', endPointer);
// 指针离开画面（比如拖到窗口外松手）也要收尾，
// 否则那根"手指"会在设备上一直按着
img.addEventListener('pointerleave', (ev) => {
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
