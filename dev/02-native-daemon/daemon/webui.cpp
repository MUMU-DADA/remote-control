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
    $('meta').textContent = sw + '×' + sh + ' · ' + r.capture.backend
        + ' · pid ' + r.pid + ' · 协议 v' + r.protocolVersion;
  }).catch(e => setStatus('取状态失败：' + e, true));
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

function tap(x, y) {
  api('/tap', { method:'POST', headers:{'Content-Type':'application/json'},
                body: JSON.stringify({x:x, y:y, ms:50}) })
    .then(() => setStatus('点击 ' + x + ',' + y))
    .catch(e => setStatus('点击失败：' + e, true));
}
function longPress(x, y) {
  api('/longpress', { method:'POST', headers:{'Content-Type':'application/json'},
                      body: JSON.stringify({x:x, y:y, ms:800}) })
    .then(() => setStatus('长按 ' + x + ',' + y))
    .catch(e => setStatus('长按失败：' + e, true));
}
function drag(x1, y1, x2, y2) {
  api('/drag', { method:'POST', headers:{'Content-Type':'application/json'},
                 body: JSON.stringify({x1:x1, y1:y1, x2:x2, y2:y2, ms:600}) })
    .then(() => setStatus('拖拽 ' + x1 + ',' + y1 + ' → ' + x2 + ',' + y2))
    .catch(e => setStatus('拖拽失败：' + e, true));
}

// ── 鼠标 / 触摸交互 ──
// 用 pointer 事件而不是分别处理 mouse/touch：一套代码两条输入都覆盖。
const LONG_PRESS_MS = 600;
const DRAG_SLOP = 8;          // 超过这个位移就算拖拽，不再触发长按/点击
let press = null;

img.addEventListener('pointerdown', (ev) => {
  ev.preventDefault();
  img.setPointerCapture(ev.pointerId);
  const p = toScreen(ev);
  press = { start: p, cur: p, timer: setTimeout(() => {
    // 时间到了还没松开、也没移动 → 长按
    longPress(p.x, p.y);
    press.fired = 'long';
  }, LONG_PRESS_MS), moved: false };
});

img.addEventListener('pointermove', (ev) => {
  const p = toScreen(ev);
  $('coord').textContent = p.x + ',' + p.y;
  if (!press) return;
  press.cur = p;
  const dx = p.x - press.start.x, dy = p.y - press.start.y;
  if (Math.abs(dx) > DRAG_SLOP || Math.abs(dy) > DRAG_SLOP) {
    press.moved = true;
    if (press.timer) { clearTimeout(press.timer); press.timer = null; }
  }
});

function endPress(ev) {
  if (!press) return;
  const p = press;
  if (p.timer) { clearTimeout(p.timer); p.timer = null; }
  press = null;
  if (p.fired === 'long') return;             // 长按已经发过了
  if (p.moved) {
    drag(p.start.x, p.start.y, p.cur.x, p.cur.y);
  } else {
    tap(p.cur.x, p.cur.y);
  }
}
img.addEventListener('pointerup', endPress);
img.addEventListener('pointercancel', () => { if (press && press.timer) clearTimeout(press.timer); press = null; });

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
setInterval(refresh, 10000);   // 定期刷状态，页面放着不动也不会显示过期信息
</script>
</body>
</html>
)HTML";
    return kHtml;
}

}  // namespace autod
