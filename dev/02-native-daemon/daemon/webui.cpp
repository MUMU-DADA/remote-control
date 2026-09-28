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
  /* 可收起的卡片头。整行可点，不只小三角 —— 手机上点一个小图标很难命中 */
  .card h2.clickable { cursor:pointer; user-select:none; display:flex;
                       align-items:center; gap:6px; margin-bottom:0; }
  .card h2.clickable .arrow { font-size:10px; transition:transform .15s;
                              display:inline-block; color:#666; }
  .card.collapsed h2.clickable .arrow { transform:rotate(-90deg); }
  .card.collapsed .body { display:none; }
  .card h2.clickable + .body { margin-top:8px; }
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
  .app button { padding:2px 7px; font-size:11px; }
  #logbox { margin-top:6px; height:220px; overflow:auto; background:#0d0d0d;
            border:1px solid #333; border-radius:4px; padding:5px;
            font:11px/1.45 ui-monospace,Menlo,Consolas,monospace;
            white-space:pre-wrap; word-break:break-all; }
  .lg-I { color:#9cf; } .lg-W { color:#fc6; } .lg-E { color:#f88; }
  .lg-D { color:#888; } .lg-T { color:#666; }
</style>
</head>
<body>
<header>
  <h1>autod 控制台</h1>
  <span class="dim" id="meta">连接中…</span>
  <!-- 服务对外开关。放在最显眼的位置：它是"这台设备还能不能被控制"
       的总闸，而不是某个功能的设置。 -->
  <label id="svcwrap" style="margin-left:auto; display:flex; align-items:center;
         gap:6px; font-size:12px; color:#4a9">
    <input type="checkbox" id="svcsw" onchange="toggleService(this.checked)">
    <span id="svctext">服务对外可用</span>
  </label>
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
        <button id="btn-h264" onclick="setCodec('h264',75)"
                title="H.264：帧比 JPEG 小两个数量级，需要浏览器支持 WebCodecs">H.264</button>
      </div>
      <div class="row" style="margin-top:6px">
        <label style="font-size:12px; display:flex; align-items:center; gap:5px">
          <input type="checkbox" id="skipsw" checked
                 onchange="setSkipUnchanged(this.checked)">
          画面停止检测（静止时不重发）
        </label>
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

    <div class="card" id="card-apps">
      <h2 class="clickable" onclick="toggleCard('card-apps')">
        <span class="arrow">▼</span>应用
      </h2>
      <div class="body">
      <div class="row">
        <label style="font-size:12px"><input type="checkbox" id="appSys"> 含系统应用</label>
        <button onclick="loadApps()">刷新列表</button>
        <button onclick="$('apkfile').click()">上传安装 APK</button>
        <input type="file" id="apkfile" accept=".apk,application/vnd.android.package-archive"
               style="display:none" onchange="uploadApk(this)">
        <label style="font-size:12px;align-self:center">
          <input type="checkbox" id="apkReplace" checked>
          覆盖安装
        </label>
      </div>
      <div id="apkmsg" class="dim" style="font-size:12px;margin-top:5px"></div>
      <div id="apps" class="dim" style="margin-top:6px;font-size:12px">
        （点「刷新列表」加载）
      </div>
      </div>
    </div>

    <div class="card" id="card-running">
      <h2 class="clickable" onclick="toggleCard('card-running')">
        <span class="arrow">▼</span>运行中的应用
      </h2>
      <div class="body">
      <div class="row">
        <button onclick="loadRunning()">刷新</button>
        <label style="font-size:12px;align-self:center">
          <input type="checkbox" id="runAuto" onchange="setRunAuto(this.checked)">
          自动刷新
        </label>
        <label style="font-size:12px;align-self:center">
          <input type="checkbox" id="runSys" onchange="loadRunning()">
          含系统应用
        </label>
      </div>
      <div id="running" class="dim" style="margin-top:6px;font-size:12px">
        （点「刷新」加载）
      </div>
      </div>
    </div>

    <div class="card">
      <h2>日志</h2>
      <div class="row">
        <button onclick="logClear()">清屏</button>
        <button onclick="logHistory()">读历史文件</button>
        <label style="font-size:12px;align-self:center">
          <input type="checkbox" id="logAuto" checked
                 onchange="setLogAuto(this.checked)">
          实时
        </label>
      </div>
      <div id="logbox"></div>
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

// ── H.264 / WebCodecs ────────────────────────────────────────────────────
//
// H.264 的帧比 JPEG 小两个数量级（P 帧几十~几百字节 vs 11 KB），
// 但它**必须由 WebCodecs 解码**，而那需要 Chrome/WebView 94+。
//
// 设备上的 WebView 是 91 —— 也就是说在**设备本机**打开这个控制台
// 用不了 H.264；从桌面浏览器打开可以（那才是常规用法）。
//
// 所以要**探测 + 回退**：探测不到就禁用按钮，别让用户选到一个永远
// 黑屏的选项；中途发现不支持也要退回去。
const hasWebCodecs = (typeof VideoDecoder !== 'undefined' &&
                      typeof EncodedVideoChunk !== 'undefined');

let h264Dec = null;        // VideoDecoder 实例
let h264Ready = false;     // 配置成功、可以喂数据了
let h264Ts = 0;            // 时间戳（WebCodecs 要求单调递增）

function stopH264() {
  if (h264Dec) {
    try { h264Dec.close(); } catch (e) {}
    h264Dec = null;
  }
  h264Ready = false;
  h264Ts = 0;
}

// 一段 Annex-B 里有没有 IDR（NAL 类型 5）？
//
// WebCodecs 的 EncodedVideoChunk 必须标明 key/delta，而服务端发的是
// 裸 Annex-B，没有额外元信息 —— 只能自己扫 NAL 头。
function hasIdr(buf) {
  const d = new Uint8Array(buf);
  for (let i = 0; i + 4 < d.length; i++) {
    if (d[i] === 0 && d[i + 1] === 0 &&
        (d[i + 2] === 1 || (d[i + 2] === 0 && d[i + 3] === 1))) {
      const off = (d[i + 2] === 1) ? i + 3 : i + 4;
      if (off < d.length && (d[off] & 0x1F) === 5) return true;
    }
  }
  return false;
}

function startH264(codecStr) {
  stopH264();
  if (!hasWebCodecs) return false;
  try {
    h264Dec = new VideoDecoder({
      output: (frame) => {
        if (cvs.width !== frame.displayWidth || cvs.height !== frame.displayHeight) {
          cvs.width = frame.displayWidth;
          cvs.height = frame.displayHeight;
          sw = cvs.width; sh = cvs.height;
          updateMeta();
        }
        ctx.drawImage(frame, 0, 0);
        frame.close();     // 不 close 会攒着不放，几秒就吃满内存
        ++streamFrames;
      },
      error: (e) => {
        dbg('VideoDecoder 错误: ' + e);
        h264Ready = false;
        setStatus('H.264 解码器出错，退回 JPEG', true);
        fallbackToJpeg();
      }
    });
    // optimizeForLatency：别为了重排攒缓冲。画面流要的是低延迟。
    h264Dec.configure({ codec: codecStr, optimizeForLatency: true });
    h264Ready = true;
    dbg('H.264 解码器已配置: ' + codecStr);
    return true;
  } catch (e) {
    dbg('VideoDecoder configure 失败: ' + e);
    stopH264();
    return false;
  }
}

// 退回 JPEG 并重连。只在确认不支持时调，避免来回切。
let fallingBack = false;
function fallbackToJpeg() {
  if (fallingBack || codec !== 'h264') return;
  fallingBack = true;
  codec = 'jpeg';
  stopH264();
  stopStream();
  setTimeout(() => { fallingBack = false; startStream(); pushStreamParams(); }, 400);
}
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
    if (r.status === 503) {
      // 服务被关了。这不是错误，是用户自己按的 —— 把开关状态同步过来，
      // 而不是弹一堆失败提示。
      serving = false;
      updateServiceUi('服务已关闭对外能力');
      throw new Error('服务已关闭');
    }
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
       + '&maxWidth=' + maxW + '&skipUnchanged=' + (skipUnchanged ? 1 : 0));
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
      } else if (m.t === 'codec') {
        // 服务端在**第一帧之前**告诉 codec 串 —— WebCodecs 必须要它，
        // 而各设备的 profile/level 不同，不能写死。
        if (!startH264(m.codec)) {
          setStatus('这个浏览器不支持 H.264（需要 WebCodecs），已退回 JPEG', true);
          dbg('没有 WebCodecs，退回 JPEG');
          fallbackToJpeg();
        } else {
          setStatus('画面流 H.264（' + m.codec + '）');
        }
      } else if (m.t === 'hello') {
        setStatus('画面流 ' + m.format + ' @' + m.fps + 'fps');
      }
      return;
    }

    // H.264：喂给 WebCodecs，不走 createImageBitmap
    if (h264Ready && h264Dec) {
      try {
        h264Dec.decode(new EncodedVideoChunk({
          type: hasIdr(ev.data) ? 'key' : 'delta',
          timestamp: (h264Ts += 33333),   // 微秒；单调递增即可
          data: ev.data
        }));
      } catch (e) {
        if (streamFrames === 0) dbg('H.264 decode 失败: ' + e);
      }
      return;
    }
    // 选了 H.264 但解码器还没起来（codec 消息没到 / 配置失败）：
    // 这些是裸 H.264 字节，当图片解只会报错 —— 直接丢。
    if (codec === 'h264') return;

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
  stopH264();
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


// 没有 WebCodecs 就把 H.264 按钮置灰。
//
// 只在点击时报错是不够的 —— 用户会以为"这个选项应该有画面"，
// 点几下才发现不行。置灰 + title 说明是更清楚的表达。
(function disableH264IfUnsupported() {
  if (hasWebCodecs) return;
  const b = document.getElementById('btn-h264');
  if (b) {
    b.disabled = true;
    b.title = '这个浏览器不支持 WebCodecs（需要 Chrome/WebView 94+），无法解码 H.264';
    b.style.opacity = '0.4';
    b.style.cursor = 'not-allowed';
  }
})();

function setCodec(c, q) {
  if (c === 'h264' && !hasWebCodecs) {
    setStatus('这个浏览器不支持 H.264（WebCodecs 需要 Chrome/WebView 94+）', true);
    return;
  }
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

// ── 画面流停止检测 ──
//
// "停止检测"= 画面没变时整帧跳过编码。静止画面下能省掉全部编码开销，
// 但刚打开页面时如果画面一直不动，会让人以为流挂了 —— 所以给个开关。
let skipUnchanged = true;

function setSkipUnchanged(on) {
  skipUnchanged = on;
  if (streamReady) {
    // 走 WS 命令，不用重连
    sendStream({t: 'skipUnchanged', v: on ? 1 : 0});
  }
  setStatus(on ? '已开启画面停止检测（静止时不重发）' : '已关闭 —— 每帧都会发送');
}

function sendStream(obj) {
  if (streamReady && streamWs) {
    streamWs.send(JSON.stringify(obj));
    return true;
  }
  return false;
}

// ── 服务对外开关 ──
let serving = true;

function toggleService(on) {
  api('/service', { method:'POST', headers:{'Content-Type':'application/json'},
                    body: JSON.stringify({on: on}) })
    .then(d => {
      serving = d.serving;
      updateServiceUi(d.note || '');
    })
    .catch(e => setStatus('切换服务开关失败：' + e, true));
}

function refreshService() {
  api('/service').then(d => {
    serving = d.serving;
    updateServiceUi('');
  }).catch(() => {});
}

function updateServiceUi(note) {
  $('svcsw').checked = serving;
  $('svctext').textContent = serving ? '服务对外可用' : '服务已关闭';
  $('svcwrap').style.color = serving ? '#4a9' : '#c55';
  if (note) setStatus(note);
}

// ── 卡片收起 / 展开 ──
//
// 状态记在 localStorage：控制台的面板很多，用户收起某几块是长期偏好，
// 刷新一次就复位会很烦。
function toggleCard(id) {
  const el = $(id);
  if (!el) return;
  el.classList.toggle('collapsed');
  const st = readCollapsed();
  if (el.classList.contains('collapsed')) st[id] = 1; else delete st[id];
  try { localStorage.setItem('autod_collapsed', JSON.stringify(st)); } catch (e) {}
}

function readCollapsed() {
  try { return JSON.parse(localStorage.getItem('autod_collapsed') || '{}'); }
  catch (e) { return {}; }
}

function applyCollapsed() {
  const st = readCollapsed();
  for (const id of Object.keys(st)) {
    const el = $(id);
    if (el) el.classList.add('collapsed');
  }
}

// ── 上传安装 APK ──
//
// 直接把文件字节当请求体 POST，不用 multipart —— 服务端只收一个文件，
// 为它实现一遍 multipart 解析不划算，而 fetch 传 File 本来就能直接当 body。
function uploadApk(input) {
  const f = input.files && input.files[0];
  if (!f) return;
  const msg = $('apkmsg');
  const mb = (f.size / 1048576).toFixed(1);
  msg.className = '';
  msg.textContent = '上传中… ' + f.name + '（' + mb + ' MB）';

  const replace = $('apkReplace').checked ? '1' : '0';
  fetch('/api/v1/install?replace=' + replace, {
    method: 'POST',
    headers: authHeaders({'Content-Type': 'application/vnd.android.package-archive'}),
    body: f
  })
    .then(r => r.json().then(j => ({ status: r.status, j: j })))
    .then(({status, j}) => {
      // 显示服务端的原话：安装失败的原因（签名冲突、版本降级、空间不足）
      // 是用户唯一能据此行动的信息，包装成"安装失败"就没用了
      if (j && j.ok) {
        msg.className = 'ok';
        msg.textContent = '✓ 已安装 ' + (j.package || f.name);
      } else {
        msg.className = 'err';
        msg.textContent = '✗ 安装失败：' + ((j && j.error) || ('HTTP ' + status));
      }
      setStatus(msg.textContent, !(j && j.ok));
      setTimeout(loadRunning, 800);
    })
    .catch(e => {
      msg.className = 'err';
      msg.textContent = '✗ 上传失败：' + e;
    })
    .finally(() => { input.value = ''; });   // 允许重复选同一个文件
}

// ── 运行中的应用 ──
let runTimer = null;

function loadRunning() {
  const box = $('running');
  api('/running').then(d => {
    box.textContent = '';
    box.className = '';
    const all = d.apps || [];
    // 默认只看第三方应用。系统组件占了大半，默认全列出来的话
    // 真正要找的那个应用反而被淹了。
    const showSys = $('runSys').checked;
    const apps = showSys ? all : all.filter(a => !a.system);
    if (!apps.length) {
      box.innerHTML = '<span class="dim">（没有匹配的进程'
          + (showSys ? '' : '；勾上「含系统应用」看全部 ' + all.length + ' 个') + '）</span>';
      return;
    }
    // 前台和可见的排前面 —— 那才是用户关心的
    const rank = st => st.startsWith('fg') ? 0 : st.startsWith('vis') ? 1
                    : st.startsWith('prcp') ? 2 : 3;
    apps.sort((a, b) => rank(a.state) - rank(b.state));
    for (const a of apps) {
      const row = document.createElement('div');
      row.className = 'app';
      const name = document.createElement('span');
      name.className = 'pkg';
      name.textContent = a.pid + '  [' + a.state + ']  ' + a.process;
      name.title = a.process + '  uid=' + a.uid;
      const bKill = document.createElement('button');
      bKill.textContent = '关闭';
      // 系统进程不给按钮：误点会重启系统 UI，而不是关掉一个应用
      if (/^(android|com\.android\.systemui)$/.test(a.package)) {
        bKill.disabled = true;
        bKill.title = '系统进程，不提供关闭';
      } else {
        bKill.onclick = () => killApp(a.package);
      }
      row.append(name, bKill);
      box.append(row);
    }
    setStatus('运行中 ' + apps.length + ' 个'
              + (showSys ? '（含系统）' : '（仅第三方，共 ' + all.length + ' 个进程）'));
  }).catch(e => setStatus('取运行状态失败：' + e, true));
}

function killApp(pkg) {
  if (!confirm('强制停止 ' + pkg + '？')) return;
  api('/apps/' + encodeURIComponent(pkg) + '/kill',
      { method:'POST', headers:{'Content-Type':'application/json'}, body:'{}' })
    .then(() => { setStatus('已停止 ' + pkg); setTimeout(loadRunning, 600); })
    .catch(e => setStatus('停止失败：' + e, true));
}

function setRunAuto(on) {
  if (runTimer) { clearInterval(runTimer); runTimer = null; }
  if (on) runTimer = setInterval(loadRunning, 3000);
}

// ── 日志流 ──
//
// 用 WebSocket 增量推，而不是轮询 /api/v1/log：
// 日志是"有就推、没有就没有"，轮询在没日志时纯属白问，
// 有日志时又必然滞后一个轮询周期。
let logWs = null;
let logReady = false;
let logSeen = 0;
let logFilterLevel = -1;   // -1 = 不过滤

function connectLog() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  try { logWs = new WebSocket(withToken(proto + '//' + location.host + '/api/v1/logstream')); }
  catch (e) { return; }
  logWs.onopen = () => { logReady = true; };
  logWs.onclose = () => {
    logReady = false; logWs = null;
    setTimeout(connectLog, 2000);
  };
  logWs.onerror = () => { logReady = false; };
  logWs.onmessage = (ev) => {
    let m; try { m = JSON.parse(ev.data); } catch (e) { return; }
    if (m.t === 'lines') appendLogs(m.lines);
  };
}

function appendLogs(lines) {
  if (!lines || !lines.length) return;
  const box = $('logbox');
  if (!$('logAuto').checked) return;

  // 只在贴近底部时自动滚动。用户往上翻看历史时把他拽回底部
  // 是最招人烦的行为之一。
  const atBottom = box.scrollTop + box.clientHeight >= box.scrollHeight - 24;

  const frag = document.createDocumentFragment();
  for (const l of lines) {
    logSeen = Math.max(logSeen, l.seq || 0);
    const lv = 'IWED'[l.level] || '?';
    const d = document.createElement('div');
    d.className = 'lg-' + lv;
    // 时间前缀。服务端已经格式化好了（MM-DD HH:MM:SS），
    // 这里原样带上 —— 各端各自转时区的话，同一份日志在不同机器上
    // 会显示出不同的时间。
    const t = l.time ? (l.time + ' ') : '';
    d.textContent = t + lv + ' ' + (l.text || '');
    frag.append(d);
  }
  box.append(frag);
  // 上限：日志流是持续的，DOM 不限行数的话开一天就能把标签页拖死
  while (box.childElementCount > 500) box.removeChild(box.firstChild);
  if (atBottom) box.scrollTop = box.scrollHeight;
}

function logClear() {
  $('logbox').textContent = '';
  logSeen = 0;
  if (logReady) logWs.send(JSON.stringify({t: 'clear'}));
}

function logHistory() {
  api('/logfile').then(d => {
    $('logbox').textContent = '';
    const lines = (d.text || '').split('\n').filter(x => x);
    for (const t of lines) {
      const el = document.createElement('div');
      el.className = 'lg-T';
      el.textContent = t;
      $('logbox').append(el);
    }
    setStatus('历史日志 ' + d.bytes + ' 字节（' + d.path + '）');
  }).catch(e => setStatus('读历史日志失败：' + e, true));
}

// ── 启动 ──
applyCollapsed();
refresh();
refreshService();
startStream();
connectTouch();
connectLog();
loadRunning();
setRunAuto(true);
setInterval(refresh, 10000);   // 定期刷状态，页面放着不动也不会显示过期信息
</script>
</body>
</html>
)HTML";
    return kHtml;
}

}  // namespace autod
